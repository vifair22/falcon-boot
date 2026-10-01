/* falcon-boot mkstick: `update`, a stick or a station brought to the
 * loaders as built now, in place (docs/DESIGN.md Section 13.14).
 *
 * What the disk is comes from updatecore.c; this file reads the disk into
 * its facts and carries out the plan. The halves:
 *
 *   BIOS   stages 1-3 rewritten with `mkstick bios`'s own code, every
 *          sector it overwrites saved first, stage 3 read back; for
 *          station-legacy its drivers replaced as files.
 *   UEFI   the loader signed with the install's MOK and written over the
 *          old one, which is saved first, then read back. The key comes
 *          off the shelf into sealed memfds (shelfkey.h) and never reaches
 *          a file; --key/--cert name it instead for an install whose shelf
 *          does not hold it (a --p3-plain stick).
 *   TOTP   a UEFI station with [totp] is resealed at once, to what the new
 *          loader will measure (stn_totp_reseal_update).
 *
 * It keeps every record, STATION.INI, /uki, shim, MokManager and the
 * certificate, the firmware's boot entries and every other partition. The
 * next-boot marker is cleared, as `mkstick bios` clears it: an update is
 * not a moment anything is queued for.
 */

#include <mkstick/mkstick.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <common/log.h>
#include <common/proc.h>
#include <common/util.h>
#include <mkstick/bioscore.h>
#include <mkstick/disk.h>
#include <mkstick/keys.h>
#include <mkstick/pass.h>
#include <mkstick/records.h>
#include <mkstick/shelfkey.h>
#include <mkstick/shimname.h>
#include <mkstick/station.h>
#include <mkstick/updatecore.h>

#include <fs/fs.h>
#include <ident/ident.h>
#include <media/stationini.h>

#include <falcon-bios/src/start/biosdefs.h>

#define DEFAULT_FROM "build/bin"
/* A loader, a stage 3 or a shim is well under this. */
#define FILE_MAX ((size_t)16 * 1024 * 1024)

static const char *const k_mods[] = {
    "ATA.MOD", "EHCI.MOD", "UHCI.MOD", "OHCI.MOD", "PCC.MOD"
};
static const char *const k_mod_paths[] = {
    "/ATA.MOD", "/EHCI.MOD", "/UHCI.MOD", "/OHCI.MOD", "/PCC.MOD"
};

typedef struct {
    const char *target;
    mks_disk    d;
    mks_records r;
    upd_facts   f;
    upd_plan    p;
    /* The boot partition (a stick's p1, a BIOS station's partition) and
     * the UEFI station's ESP, in bytes. */
    uint64_t    boot_off, boot_len;
    uint64_t    esp_off, esp_len;
    /* The loaders' names: shim's next stage on a stick's p1, and in the
     * station's EFI/falcon. */
    char        stick_name[SHIM_NEXT_STAGE_MAX];
    char        station_name[SHIM_NEXT_STAGE_MAX];
    /* The old station loader's digests, for the reseal. */
    uint8_t     old1[32];
    uint8_t     old256[32];
    int         have_old;
    char        stamp[32];
} upd_ctx;

static void part_span(const upd_ctx *c, int i, uint64_t *off, uint64_t *len)
{
    const part_entry *p = &c->r.parts[i];

    *off = p->start_lba * c->d.sector;
    *len = (p->end_lba - p->start_lba + 1u) * c->d.sector;
}

/* A file on a FAT volume of the target: 1 when it is there. */
static int has_file(const upd_ctx *c, uint64_t off, uint64_t len,
                    const char *path)
{
    char buf[16];
    size_t got = 0;
    int rc = stn_read_file(c->d.path, off, len, path, buf, sizeof buf, &got);

    return rc == FS_OK || rc == FS_ERR_RANGE;
}

/* A whole file off a FAT volume of the target, malloc'd. */
static cli_status read_file(const upd_ctx *c, uint64_t off, uint64_t len,
                            const char *path, uint8_t **out, size_t *n)
{
    char *buf = malloc(FILE_MAX);
    int rc;

    if (buf == NULL)
        return set_error(CLI_ERR_NOMEM, "reading %s", path);
    rc = stn_read_file(c->d.path, off, len, path, buf, FILE_MAX, n);
    if (rc != FS_OK) {
        free(buf);
        return set_error(rc == FS_ERR_NOT_FOUND ? CLI_ERR_NOTFOUND
                                                : CLI_ERR_IO,
                         "%s could not be read off the disk", path);
    }
    *out = (uint8_t *)buf;
    return CLI_OK;
}

/* The next stage a stick's shim names: shim copied off p1 to a temporary
 * file, since shimname.c reads a path. */
static cli_status stick_loader_name(upd_ctx *c)
{
    char tmp[] = "/tmp/falcon-update-shim.XXXXXX";
    uint8_t *buf = NULL;
    size_t n = 0;
    int fd;
    cli_status rc;

    rc = read_file(c, c->boot_off, c->boot_len,
                   "/" MKS_ESP_BOOT_DIR "/" MKS_ESP_SHIM_NAME, &buf, &n);
    if (rc != CLI_OK)
        return rc;
    fd = mkstemp(tmp);
    if (fd < 0) {
        free(buf);
        return set_error_errno(CLI_ERR_IO, "a temporary copy of shim");
    }
    (void)close(fd);
    rc = util_write_file(tmp, buf, n, 0600);
    free(buf);
    if (rc == CLI_OK)
        rc = shim_next_stage_name(tmp, c->stick_name, sizeof c->stick_name);
    (void)unlink(tmp);
    return rc;
}

static int same_id(const upd_ctx *c, int a, int b)
{
    return a >= 0 && b >= 0 &&
           memcmp(c->r.recs[a].rec.install_id, c->r.recs[b].rec.install_id,
                  IDENT_ID_LEN) == 0;
}

/* The ESP whose `boot` record has no stage 3: the UEFI station's. */
static int esp_boot(const mks_records *r)
{
    int i = stn_find_esp(r);

    if (i < 0 || r->recs[i].state != REC_OK ||
        r->recs[i].rec.role != IDENT_ROLE_BOOT ||
        r->recs[i].rec.stage3_sector != 0u)
        return -1;
    return i;
}

static cli_status gather(upd_ctx *c)
{
    upd_facts *f = &c->f;
    uint8_t mbr[512];
    disk_io io;
    uint64_t lba = 0;
    uint64_t n = 0;
    size_t i;
    cli_status rc;

    rc = disk_stat(&c->d, c->target);
    if (rc != CLI_OK)
        return rc;
    rc = records_read(&c->r, c->d.path, c->d.sector, c->d.size);
    if (rc != CLI_OK)
        return rc;
    if (c->r.rc != PART_OK)
        return set_error(CLI_ERR_INVALID, "%s has no usable partition table",
                         c->d.path);

    memset(f, 0, sizeof *f);
    f->boot = records_bios_boot(&c->r);
    f->esp_boot = esp_boot(&c->r);
    f->library = records_find(&c->r, IDENT_ROLE_LIBRARY, 0);
    f->shelf = records_find(&c->r, IDENT_ROLE_SHELF, 0);
    if (!same_id(c, f->boot, f->library))
        f->library = -1;
    if (!same_id(c, f->boot, f->shelf))
        f->shelf = -1;

    if (f->boot >= 0) {
        part_span(c, f->boot, &c->boot_off, &c->boot_len);
        f->station_ini = has_file(c, c->boot_off, c->boot_len,
                                  "/" STATIONINI_FILE);
        for (i = 0; i < sizeof k_mods / sizeof k_mods[0]; i++)
            f->mods |= has_file(c, c->boot_off, c->boot_len, k_mod_paths[i]);
        if (has_file(c, c->boot_off, c->boot_len,
                     "/" MKS_ESP_BOOT_DIR "/" MKS_ESP_SHIM_NAME) &&
            stick_loader_name(c) == CLI_OK) {
            char path[MKS_PATH_MAX];

            (void)snprintf(path, sizeof path, "/%s/%s", MKS_ESP_BOOT_DIR,
                           c->stick_name);
            f->stick_loader = has_file(c, c->boot_off, c->boot_len, path);
        }
        cli_error_clear();
    }

    if (f->esp_boot >= 0) {
        part_span(c, f->esp_boot, &c->esp_off, &c->esp_len);
        if (stn_esp_loader_name(c->d.path, c->esp_off, c->esp_len,
                                c->station_name,
                                sizeof c->station_name) == CLI_OK) {
            char path[MKS_PATH_MAX];

            (void)snprintf(path, sizeof path, "/%s/%s", STN_ESP_DIR,
                           c->station_name);
            f->station_loader = has_file(c, c->esp_off, c->esp_len, path);
        }
        cli_error_clear();
        {
            static char ini[16384];
            static stationini parsed;
            size_t got = 0;

            if (stn_read_file(c->d.path, c->esp_off, c->esp_len,
                              "/" STN_ESP_DIR "/" STATIONINI_FILE, ini,
                              sizeof ini, &got) == FS_OK &&
                stationini_parse(ini, got, &parsed) == 0)
                f->totp = parsed.totp.present;
        }
    }

    if (disk_io_open(&io, c->d.path, 0, c->d.size) == CLI_OK) {
        if (disk_io_read(&io, 0, sizeof mbr, mbr) == 0)
            f->bios_installed = bioscore_stage1_fields(
                mbr, STAGE1_S2_LBA_OFF, STAGE1_S2_SECTORS_OFF, &lba, &n);
        disk_io_close(&io);
    }
    cli_error_clear();
    return CLI_OK;
}

/* ---- the key ---- */

/* The shelf the UEFI half signs with the key of: a stick's own p3, or the
 * station's, which the scan finds on whichever disk carries its install
 * ID. */
static cli_status open_key(upd_ctx *c, const cli_args *a, shelf_key *sk,
                           mok_keys *keys)
{
    const char *key = cli_value(a, "key", NULL);
    const char *cert = cli_value(a, "cert", NULL);
    static stn_scan s;
    const mks_disk *sd = &c->d;
    const part_entry *p;
    mks_pass pass;
    int luks;
    cli_status rc;

    memset(sk, 0, sizeof *sk);
    sk->key_fd = -1;
    sk->cert_fd = -1;
    if ((key == NULL) != (cert == NULL))
        return set_error(CLI_ERR_USAGE, "--key and --cert go together");
    if (key != NULL) {
        memset(keys, 0, sizeof *keys);
        (void)snprintf(keys->key, sizeof keys->key, "%s", key);
        (void)snprintf(keys->crt, sizeof keys->crt, "%s", cert);
        return CLI_OK;
    }

    if (c->p.kind == UPD_STICK) {
        p = &c->r.parts[c->f.shelf];
        luks = c->r.recs[c->f.shelf].luks;
    } else {
        rc = stn_scan_disks(&s, c->d.path, NULL, 0, 0);
        if (rc != CLI_OK)
            return rc;
        if (s.shelf_disk < 0 || s.shelf_part < 0)
            return set_error(CLI_ERR_NOTFOUND, "no shelf carries this "
                             "station's install ID; give the MOK with --key "
                             "and --cert");
        sd = &s.disks[s.shelf_disk].d;
        p = &s.disks[s.shelf_disk].r.parts[s.shelf_part];
        luks = s.shelf_luks;
    }
    memset(&pass, 0, sizeof pass);
    if (luks) {
        rc = pass_unlock(&pass, cli_value(a, "passphrase-file", NULL),
                         "passphrase for the shelf: ");
        if (rc != CLI_OK)
            return rc;
    }
    rc = shelf_key_open(sk, sd->path, p->start_lba * sd->sector,
                        (p->end_lba - p->start_lba + 1u) * sd->sector, luks,
                        luks ? &pass : NULL);
    pass_wipe(&pass);
    if (rc != CLI_OK)
        return rc;
    *keys = sk->k;
    log_info("the MOK is read off the shelf (partition %u of %s) into "
             "memory", p->number, sd->path);
    return CLI_OK;
}

/* ---- the halves ---- */

static cli_status backup_name(const upd_ctx *c, const cli_args *a,
                              const char *what, char *out, size_t cap)
{
    const char *dir = cli_value(a, "backup-dir", ".");
    const char *base = strrchr(c->d.path, '/');
    const char *ext = strrchr(what, '.');
    int stem;

    /* The stamp before the extension, so the backup still opens as what it
     * is: sdb-bios-20260930-232752.bin. */
    base = base != NULL ? base + 1 : c->d.path;
    if (ext == NULL)
        ext = what + strlen(what);
    stem = (int)(ext - what);
    if (snprintf(out, cap, "%s/%s-%.*s-%s%s", dir, base, stem, what,
                 c->stamp, ext) >= (int)cap)
        return set_error(CLI_ERR_RANGE, "--backup-dir is too long");
    return CLI_OK;
}

/* Stage 3 as the disk holds it, against the file it was written from. */
static cli_status stage3_readback(const upd_ctx *c, const char *file)
{
    const ident_rec *rec = &c->r.recs[c->f.boot].rec;
    uint64_t at = (c->r.parts[c->f.boot].start_lba + rec->stage3_sector) *
                  c->d.sector;
    void *want = NULL;
    uint8_t *have;
    size_t n = 0;
    disk_io io;
    cli_status rc;

    rc = util_read_file(file, &want, &n, FILE_MAX);
    if (rc != CLI_OK)
        return rc;
    have = malloc(n);
    if (have == NULL) {
        free(want);
        return set_error(CLI_ERR_NOMEM, "reading stage 3 back");
    }
    rc = disk_io_open(&io, c->d.path, 0, c->d.size);
    if (rc == CLI_OK) {
        if (disk_io_read(&io, at, n, have) != 0 || memcmp(have, want, n) != 0)
            rc = set_error(CLI_ERR_IO, "stage 3 did not read back as it was "
                           "written");
        disk_io_close(&io);
    }
    free(have);
    free(want);
    return rc;
}

static cli_status do_bios(const upd_ctx *c, const cli_args *a,
                          const char *from, int dry_run)
{
    mks_bios_req req;
    char s1[MKS_PATH_MAX];
    char s2[MKS_PATH_MAX];
    char s3[MKS_PATH_MAX];
    char backup[MKS_PATH_MAX];
    size_t i;
    cli_status rc;

    (void)snprintf(s1, sizeof s1, "%s/falcon-bios-stage1.bin", from);
    (void)snprintf(s2, sizeof s2, "%s/falcon-bios-stage2.bin", from);
    (void)snprintf(s3, sizeof s3, "%s/%s", from,
                   c->p.kind == UPD_STICK ? "falcon-bios-stage3.bin"
                   : c->p.legacy ? "falcon-bios-station-legacy-stage3.bin"
                                 : "falcon-bios-station-stage3.bin");
    rc = backup_name(c, a, "bios.bin", backup, sizeof backup);
    if (rc != CLI_OK)
        return rc;

    memset(&req, 0, sizeof req);
    req.target = c->d.path;
    req.stage1 = s1;
    req.stage2 = s2;
    req.stage3 = s3;
    req.backup = backup;
    req.dry_run = dry_run;
    rc = mks_bios_install(&req);
    if (rc != CLI_OK || dry_run)
        return rc;
    rc = stage3_readback(c, s3);
    if (rc != CLI_OK)
        return rc;
    log_info("BIOS: stages 1-3 written, stage 3 read back; the sectors they "
             "replaced are in %s", backup);

    if (!c->p.legacy)
        return CLI_OK;
    for (i = 0; i < sizeof k_mods / sizeof k_mods[0]; i++) {
        char src[MKS_PATH_MAX];

        (void)snprintf(src, sizeof src, "%s/mod/%s.gz", from, k_mods[i]);
        if (!util_is_file(src))
            (void)snprintf(src, sizeof src, "%s/mod/%s", from, k_mods[i]);
        if (!util_is_file(src)) {
            log_warn("no %s under %s/mod; the one on the disk stays",
                     k_mods[i], from);
            continue;
        }
        rc = stn_mtools_put(c->d.path, c->boot_off, src, k_mods[i]);
        if (rc != CLI_OK)
            return rc;
    }
    log_info("station-legacy: the drivers replaced");
    return CLI_OK;
}

/* `sbverify --list` names exactly one signature, from the cert, and
 * `sbverify --cert` accepts it. */
static cli_status verify_signed(const mok_keys *k, const char *file)
{
    char out[4096];
    size_t n = 0;
    cli_status rc;

    rc = proc_capture_l(out, sizeof out, &n, "sbverify", "--list", file,
                        PROC_END);
    if (rc != CLI_OK)
        return rc;
    out[n < sizeof out ? n : sizeof out - 1u] = '\0';
    if (strstr(out, "signature 1") == NULL ||
        strstr(out, "signature 2") != NULL)
        return set_error(CLI_ERR_INVALID, "the signed loader does not carry "
                         "exactly one signature");
    return proc_runl_quiet("sbverify", "--cert", k->crt, file, PROC_END);
}

/* The UEFI half: the loader in dir/name on the FAT volume at [off, len),
 * signed from src, the old one saved and, for the station, measured. */
static cli_status do_uefi(upd_ctx *c, const cli_args *a, const mok_keys *keys,
                          uint64_t off, uint64_t len, const char *dir,
                          const char *name, const char *src, int station,
                          const char *workdir)
{
    char path[MKS_PATH_MAX];
    char signed_efi[MKS_PATH_MAX];
    char backup[MKS_PATH_MAX];
    char what[96];
    uint8_t *old = NULL;
    uint8_t *back = NULL;
    void *mine = NULL;
    size_t old_n = 0;
    size_t back_n = 0;
    size_t mine_n = 0;
    cli_status rc;

    (void)snprintf(path, sizeof path, "/%s/%s", dir, name);
    (void)snprintf(what, sizeof what, "%s.efi",
                   station ? "station-loader" : "stick-loader");
    rc = backup_name(c, a, what, backup, sizeof backup);
    if (rc == CLI_OK)
        rc = read_file(c, off, len, path, &old, &old_n);
    if (rc == CLI_OK)
        rc = util_write_file(backup, old, old_n, 0644);
    if (rc == CLI_OK && station) {
        rc = stn_image_digest(old, old_n, 1, c->old1);
        if (rc == CLI_OK)
            rc = stn_image_digest(old, old_n, 0, c->old256);
        c->have_old = rc == CLI_OK;
        /* An old loader that does not hash is only a reseal lost. */
        cli_error_clear();
        rc = CLI_OK;
    }
    free(old);
    if (rc != CLI_OK)
        return rc;

    (void)snprintf(signed_efi, sizeof signed_efi, "%s/%s", workdir, name);
    rc = mok_sign(keys, src, signed_efi);
    if (rc == CLI_OK)
        rc = verify_signed(keys, signed_efi);
    if (rc == CLI_OK)
        rc = stn_mtools_put(c->d.path, off, signed_efi, path + 1);
    if (rc == CLI_OK)
        rc = util_read_file(signed_efi, &mine, &mine_n, FILE_MAX);
    if (rc == CLI_OK)
        rc = read_file(c, off, len, path, &back, &back_n);
    if (rc == CLI_OK && (back == NULL || mine == NULL || back_n != mine_n ||
                         memcmp(back, mine, mine_n) != 0))
        rc = set_error(CLI_ERR_IO, "%s did not read back as it was written",
                       path);
    free(back);
    free(mine);
    (void)unlink(signed_efi);
    if (rc != CLI_OK)
        return rc;
    log_info("UEFI: %s signed, written and read back; the one it replaced "
             "is %s", path, backup);
    return CLI_OK;
}

/* ---- the command ---- */

static void describe(const upd_ctx *c)
{
    const upd_plan *p = &c->p;

    log_info("%s is a %s", c->d.path,
             p->kind == UPD_STICK ? "falcon stick"
             : p->legacy          ? "station (station-legacy)"
                                  : "station");
    if (p->bios)
        log_info("  write: stages 1-3 (LBA 0's boot code, stage 2's gap, "
                 "partition %u's reserved sectors)%s",
                 c->r.parts[c->f.boot].number,
                 p->legacy ? ", and the drivers" : "");
    if (p->uefi_stick)
        log_info("  write: p%u \\%s\\%s, signed with the stick's MOK",
                 c->r.parts[c->f.boot].number, MKS_ESP_BOOT_DIR,
                 c->stick_name);
    if (p->uefi_station)
        log_info("  write: partition %u \\%s\\%s, signed with the station's "
                 "MOK", c->r.parts[c->f.esp_boot].number, STN_ESP_DIR,
                 c->station_name);
    if (p->totp)
        log_info("  reseal: the TOTP seed, to the new loader; a new secret "
                 "for the authenticator");
    log_info("  keep: every record, STATION.INI, /uki, shim, MokManager, "
             "the certificate, the firmware's boot entries, the other "
             "partitions");
    if (p->bios)
        log_info("  clear: the next-boot marker");
}

static int confirm(void)
{
    char line[16];
    int tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
    ssize_t n;
    static const char q[] = "update it? type yes to go on: ";

    if (tty < 0)
        return 0;
    n = write(tty, q, sizeof q - 1u);
    (void)n;
    n = read(tty, line, sizeof line - 1u);
    (void)close(tty);
    if (n <= 0)
        return 0;
    line[n] = '\0';
    return strncmp(line, "yes", 3) == 0 && (line[3] == '\n' ||
                                            line[3] == '\0');
}

static cli_status writable(const upd_ctx *c)
{
    char where[MKS_PATH_MAX];

    if (c->p.bios || c->p.uefi_stick)
        if (stn_part_mounted(&c->d, c->r.parts[c->f.boot].number, where,
                             sizeof where))
            return set_error(CLI_ERR_BUSY, "partition %u is mounted at %s; "
                             "unmount it first",
                             c->r.parts[c->f.boot].number, where);
    if (c->p.uefi_station)
        if (stn_part_mounted(&c->d, c->r.parts[c->f.esp_boot].number, where,
                             sizeof where))
            return set_error(CLI_ERR_BUSY, "the ESP (partition %u) is "
                             "mounted at %s; unmount it first",
                             c->r.parts[c->f.esp_boot].number, where);
    return CLI_OK;
}

static cli_status update_run(const cli_args *a)
{
    static upd_ctx c;
    const char *from = cli_value(a, "from", DEFAULT_FROM);
    int dry_run = cli_flag(a, "dry-run");
    char workdir[] = "/tmp/falcon-update.XXXXXX";
    char src[MKS_PATH_MAX];
    shelf_key sk;
    mok_keys keys;
    time_t now = time(NULL);
    struct tm tm;
    cli_status rc;

    memset(&c, 0, sizeof c);
    memset(&sk, 0, sizeof sk);
    sk.key_fd = -1;
    sk.cert_fd = -1;
    c.target = a->pos[0];
    (void)localtime_r(&now, &tm);
    (void)strftime(c.stamp, sizeof c.stamp, "%Y%m%d-%H%M%S", &tm);

    rc = gather(&c);
    if (rc != CLI_OK)
        return rc;
    upd_plan_make(&c.f, &c.p);
    if (c.p.kind == UPD_REFUSE)
        return set_error(CLI_ERR_INVALID, "%s: %s; nothing written",
                         c.d.path, c.p.why);
    describe(&c);
    rc = writable(&c);
    if (rc != CLI_OK)
        return rc;
    if (dry_run) {
        if (c.p.bios)
            rc = do_bios(&c, a, from, 1);
        log_info("--dry-run: nothing written");
        return rc;
    }
    if (!cli_flag(a, "yes") && !confirm())
        return set_error(CLI_ERR_USAGE, "not confirmed; nothing written");

    if (c.p.uefi_stick || c.p.uefi_station) {
        rc = open_key(&c, a, &sk, &keys);
        if (rc != CLI_OK)
            return rc;
        if (mkdtemp(workdir) == NULL) {
            shelf_key_close(&sk);
            return set_error_errno(CLI_ERR_IO, "a working directory");
        }
    }

    if (c.p.bios)
        rc = do_bios(&c, a, from, 0);
    if (rc == CLI_OK && c.p.uefi_stick) {
        (void)snprintf(src, sizeof src, "%s/falcon-boot.efi", from);
        rc = do_uefi(&c, a, &keys, c.boot_off, c.boot_len, MKS_ESP_BOOT_DIR,
                     c.stick_name, src, 0, workdir);
    }
    if (rc == CLI_OK && c.p.uefi_station) {
        (void)snprintf(src, sizeof src, "%s/falcon-boot-station.efi", from);
        rc = do_uefi(&c, a, &keys, c.esp_off, c.esp_len, STN_ESP_DIR,
                     c.station_name, src, 1, workdir);
    }
    shelf_key_close(&sk);
    if (c.p.uefi_stick || c.p.uefi_station)
        (void)rmdir(workdir);
    if (rc != CLI_OK)
        return rc;

    if (c.p.totp) {
        stn_totp_update u;
        cli_status t;

        memset(&u, 0, sizeof u);
        u.disk = c.d.path;
        u.old_sha1 = c.have_old ? c.old1 : NULL;
        u.old_sha256 = c.have_old ? c.old256 : NULL;
        u.owner_auth_file = cli_value(a, "owner-auth-file", NULL);
        u.tpm_socket = cli_value(a, "tpm-socket", NULL);
        u.event_log = cli_value(a, "event-log", NULL);
        u.saved_log = cli_value(a, "saved-log", NULL);
        t = stn_totp_reseal_update(&u);
        if (t != CLI_OK) {
            log_warn("the TOTP seed was not resealed: %s", cli_error_msg());
            log_warn("the station boots; its code line stays dark until "
                     "`mkstick station totp reseal %s`, run from a system "
                     "the new station started", c.d.path);
            cli_error_clear();
        }
    }
    log_info("%s is updated", c.d.path);
    return CLI_OK;
}

static const cli_opt update_opts[] = {
    { "from", CLI_OPT_VALUE, 0, "<dir>",
      "the built loaders (default build/bin)" },
    { "backup-dir", CLI_OPT_VALUE, 0, "<dir>",
      "where what is replaced is saved (default .)" },
    { "passphrase-file", CLI_OPT_VALUE, 0, "<file>",
      "the shelf's passphrase, instead of the prompt" },
    { "key", CLI_OPT_VALUE, 0, "<file>",
      "the MOK private key, for an install whose shelf does not hold it" },
    { "cert", CLI_OPT_VALUE, 0, "<file>", "its certificate, with --key" },
    { "owner-auth-file", CLI_OPT_VALUE, 0, "<file>",
      "the TPM owner's authorisation, for the reseal" },
    { "tpm-socket", CLI_OPT_VALUE, 0, "<path>",
      "TEST RIG: a swtpm socket instead of the TPM" },
    { "event-log", CLI_OPT_VALUE, 0, "<file>",
      "TEST RIG: the firmware's event log" },
    { "saved-log", CLI_OPT_VALUE, 0, "<file>",
      "TEST RIG: the event log the station saved" },
    { "dry-run", CLI_OPT_FLAG, 0, NULL, "report the plan and write nothing" },
    { "yes", CLI_OPT_FLAG, 0, NULL, "do not ask" },
};

const cli_cmd mks_cmd_update = {
    "update",
    "bring a stick or a station to the loaders as built now, in place",
    "<disk>",
    update_opts,
    sizeof update_opts / sizeof update_opts[0],
    1, 1,
    "Finds what the disk is by its records: a stick (boot, library and\n"
    "shelf records sharing an install ID), a BIOS station or station-legacy\n"
    "(STATION.INI beside its boot record), a UEFI station (EFI/falcon on an\n"
    "ESP with a boot record). Says what it will write and what it keeps,\n"
    "asks, and then:\n"
    "\n"
    "  BIOS   stages 1-3, as `mkstick bios` writes them, the sectors they\n"
    "         replace saved to --backup-dir first; station-legacy's drivers.\n"
    "  UEFI   the loader, signed with the install's MOK off its shelf (the\n"
    "         passphrase is asked for; the key stays in memory) or --key,\n"
    "         checked, written over the old one, which is saved first, and\n"
    "         read back.\n"
    "  TOTP   a station with [totp] is resealed to the new loader and\n"
    "         prints a new secret; replace the authenticator's entry.\n"
    "\n"
    "It keeps every record, STATION.INI, /uki, shim, MokManager, the\n"
    "certificate and the firmware's boot entries, and clears the next-boot\n"
    "marker.\n",
    update_run
};
