/* falcon-boot mkstick: `update`, a stick or a station brought to the
 * loaders as built now, in place (docs/DESIGN.md Section 12.14).
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
 *          does not hold it (a --p3-plain stick). A station without shim
 *          gets the jumper and the entry naming it as install makes them,
 *          and one from before the jumper is migrated (updatecore.h).
 *   TOTP  a UEFI station with [totp] is resealed at once, to what the new
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

#include <common/datadir.h>
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

#define DEFAULT_FROM FALCON_DATADIR "/bin"
#define DEFAULT_SHIM FALCON_DATADIR "/shim/bins"
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
    /* A stick's AArch64 chain (BOOTAA64.EFI on p1): its shim's next stage,
     * whether the loader under that name is there, and, for a stick made
     * before it had one, whether update adds the chain. */
    char        stick_name_aa64[SHIM_NEXT_STAGE_MAX];
    int         stick_aa64;
    int         stick_aa64_add;
    /* The old station loader's digests, for the reseal. */
    uint8_t     old1[32];
    uint8_t     old256[32];
    int         have_old;
    /* The jumper written has another Authenticode digest than the one it
     * replaced, so PCR 4 moves and the reseal cannot predict it. */
    int         jump_moved;
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

/* The next stage the shim at p1's /EFI/BOOT/<shim> names: shim copied off
 * p1 to a temporary file, since shimname.c reads a path. */
static cli_status stick_loader_name(upd_ctx *c, const char *shim, char *out,
                                    size_t cap)
{
    char tmp[] = "/tmp/falcon-update-shim.XXXXXX";
    char path[MKS_PATH_MAX];
    uint8_t *buf = NULL;
    size_t n = 0;
    int fd;
    cli_status rc;

    (void)snprintf(path, sizeof path, "/%s/%s", MKS_ESP_BOOT_DIR, shim);
    rc = read_file(c, c->boot_off, c->boot_len, path, &buf, &n);
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
        rc = shim_next_stage_name(tmp, out, cap);
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
            stick_loader_name(c, MKS_ESP_SHIM_NAME, c->stick_name,
                              sizeof c->stick_name) == CLI_OK) {
            char path[MKS_PATH_MAX];

            (void)snprintf(path, sizeof path, "/%s/%s", MKS_ESP_BOOT_DIR,
                           c->stick_name);
            f->stick_loader = has_file(c, c->boot_off, c->boot_len, path);
        }
        cli_error_clear();
        if (f->stick_loader &&
            has_file(c, c->boot_off, c->boot_len,
                     "/" MKS_ESP_BOOT_DIR "/" MKS_ESP_SHIM_NAME_AA64) &&
            stick_loader_name(c, MKS_ESP_SHIM_NAME_AA64, c->stick_name_aa64,
                              sizeof c->stick_name_aa64) == CLI_OK) {
            char path[MKS_PATH_MAX];

            (void)snprintf(path, sizeof path, "/%s/%s", MKS_ESP_BOOT_DIR,
                           c->stick_name_aa64);
            c->stick_aa64 = has_file(c, c->boot_off, c->boot_len, path);
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
        f->station_shim = has_file(c, c->esp_off, c->esp_len,
                                   stn_n()->shim_abs);
        if (stn_esp_jump_state(c->d.path, c->esp_off, c->esp_len,
                               &f->jump_present, &f->jump_ours) != CLI_OK) {
            /* Unreadable is someone else's until it reads as ours. */
            f->jump_present = 1;
            f->jump_ours = 0;
        }
        cli_error_clear();
        f->station_old_name = !f->station_shim &&
                              strcmp(c->station_name, STN_LOADER_OLD) == 0;
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
/* A shelf its OS has open (DESIGN 12.14): the keypair read through the
 * mapping, with no passphrase. 1 when it was, 0 when the shelf is not open
 * or the mapping does not give the keypair, which leaves the passphrase. */
static int open_key_mapped(shelf_key *sk, const mks_disk *sd, uint32_t num)
{
    char real[MKS_PATH_MAX];
    char node[MKS_PATH_MAX];
    char dm[256];
    char name[256];
    char dev[300];
    mks_disk mapped;

    if (!sd->is_block)
        return 0;
    if (realpath(sd->path, real) == NULL)
        (void)snprintf(real, sizeof real, "%s", sd->path);
    if (disk_partition_name(util_basename(real), num, node, sizeof node) !=
        CLI_OK) {
        cli_error_clear();
        return 0;
    }
    if (!disk_luks_holder("/sys/class/block", node, dm, sizeof dm, name,
                          sizeof name))
        return 0;
    (void)snprintf(dev, sizeof dev, "/dev/%s", dm);
    if (disk_stat(&mapped, dev) != CLI_OK ||
        shelf_key_open(sk, dev, 0, mapped.size, 0, NULL) != CLI_OK) {
        log_warn("the shelf is open as /dev/mapper/%s, but the MOK does not "
                 "read through it: %s", name, cli_error_msg());
        cli_error_clear();
        return 0;
    }
    log_info("the MOK is read off the shelf, open as /dev/mapper/%s, into "
             "memory", name);
    return 1;
}

/* dry_run: say where the MOK would come from, opening it only where that
 * asks nothing. */
static cli_status open_key(upd_ctx *c, const cli_args *a, shelf_key *sk,
                           mok_keys *keys, int dry_run)
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
    if (luks && cli_value(a, "passphrase-file", NULL) == NULL &&
        open_key_mapped(sk, sd, p->number)) {
        *keys = sk->k;
        return CLI_OK;
    }
    if (luks && dry_run) {
        log_info("the shelf is not open; its passphrase will be asked for");
        return CLI_OK;
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

/* The UEFI half: the image in dir/name on the FAT volume at [off, len),
 * signed from src. The old one is saved as <what>.efi, unless `fresh`
 * says there is none, and with `measure` its digests are kept for the
 * reseal. */
static cli_status do_uefi(upd_ctx *c, const cli_args *a, const mok_keys *keys,
                          uint64_t off, uint64_t len, const char *dir,
                          const char *name, const char *src, const char *what,
                          int measure, int fresh, const char *workdir)
{
    char path[MKS_PATH_MAX];
    char signed_efi[MKS_PATH_MAX];
    char backup[MKS_PATH_MAX] = "";
    char label[96];
    uint8_t *old = NULL;
    uint8_t *back = NULL;
    void *mine = NULL;
    size_t old_n = 0;
    size_t back_n = 0;
    size_t mine_n = 0;
    cli_status rc = CLI_OK;

    (void)snprintf(path, sizeof path, "/%s/%s", dir, name);
    (void)snprintf(label, sizeof label, "%s.efi", what);
    if (!fresh) {
        rc = backup_name(c, a, label, backup, sizeof backup);
        if (rc == CLI_OK)
            rc = read_file(c, off, len, path, &old, &old_n);
        if (rc == CLI_OK)
            rc = util_write_file(backup, old, old_n, 0644);
    }
    if (rc == CLI_OK && measure && old != NULL) {
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
    if (rc == CLI_OK && fresh)
        rc = stn_mtools_mkdir(c->d.path, off, dir);
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
    if (fresh)
        log_info("UEFI: %s signed, written and read back", path);
    else
        log_info("UEFI: %s signed, written and read back; the one it "
                 "replaced is %s", path, backup);
    return CLI_OK;
}

/* A station without shim from before the jumper whose loader is still
 * grubx64.efi: the old one saved and deleted, the new one written as
 * falconx64.efi, which is the file the jumper starts. */
static cli_status rename_loader(upd_ctx *c, const cli_args *a,
                                const mok_keys *keys, const char *src,
                                const char *workdir)
{
    char old[MKS_PATH_MAX];
    char backup[MKS_PATH_MAX];
    uint8_t *buf = NULL;
    size_t n = 0;
    cli_status rc;

    (void)snprintf(old, sizeof old, "/%s/%s", STN_ESP_DIR, STN_LOADER_OLD);
    rc = backup_name(c, a, "station-loader.efi", backup, sizeof backup);
    if (rc == CLI_OK)
        rc = read_file(c, c->esp_off, c->esp_len, old, &buf, &n);
    if (rc == CLI_OK)
        rc = util_write_file(backup, buf, n, 0644);
    free(buf);
    if (rc == CLI_OK)
        rc = do_uefi(c, a, keys, c->esp_off, c->esp_len, STN_ESP_DIR,
                     STN_LOADER_NOSHIM, src, "station-loader", 0, 1,
                     workdir);
    if (rc == CLI_OK)
        rc = stn_mtools_del(c->d.path, c->esp_off, old + 1);
    if (rc == CLI_OK)
        log_info("UEFI: %s deleted, saved as %s", old, backup);
    return rc;
}

/* A whole file's SHA-256 Authenticode digest, read off the ESP or a host
 * path (esp NULL). */
static int jump_digest(const upd_ctx *c, const char *esp, const char *host,
                       uint8_t out[32])
{
    uint8_t *buf = NULL;
    void *hbuf = NULL;
    size_t n = 0;
    cli_status rc;

    if (esp != NULL)
        rc = read_file(c, c->esp_off, c->esp_len, esp, &buf, &n);
    else
        rc = util_read_file(host, &hbuf, &n, FILE_MAX);
    if (rc == CLI_OK)
        rc = stn_image_digest(esp != NULL ? buf : (const uint8_t *)hbuf, n,
                              0, out);
    free(buf);
    free(hbuf);
    cli_error_clear();
    return rc == CLI_OK;
}

/* \EFI\BOOT\BOOTX64.EFI, the jumper (DESIGN Section 12.2): written where
 * there is none, signed again where it is ours, and where another
 * system's is there, that one saved to --backup-dir first. */
static cli_status do_jump(upd_ctx *c, const cli_args *a, const mok_keys *keys,
                          const char *from, const char *workdir)
{
    const char *path = stn_n()->jump_abs;
    char src[MKS_PATH_MAX];
    char backup[MKS_PATH_MAX];
    uint8_t before[32];
    uint8_t after[32];
    int had;
    cli_status rc;

    (void)snprintf(src, sizeof src, "%s/%s", from, stn_n()->jump_file);
    if (!util_is_file(src))
        return set_error(CLI_ERR_NOTFOUND, "no %s under %s",
                         stn_n()->jump_file, from);
    had = c->p.jump == STN_JUMP_RESIGN &&
          jump_digest(c, path, NULL, before);
    rc = do_uefi(c, a, keys, c->esp_off, c->esp_len, STN_ESP_BOOT_DIR,
                 STN_ESP_JUMP, src,
                 c->p.jump == STN_JUMP_TAKE ? "bootx64" : "esp-jump", 0,
                 c->p.jump == STN_JUMP_WRITE, workdir);
    if (rc != CLI_OK)
        return rc;
    if (c->p.jump == STN_JUMP_TAKE &&
        backup_name(c, a, "bootx64.efi", backup, sizeof backup) == CLI_OK)
        log_info("%s was another system's; saved to %s (`station remove "
                 "--bootx64 %s` puts it back)", STN_ESP_JUMP_PATH, backup,
                 backup);
    if (had)
        c->jump_moved = !jump_digest(c, path, NULL, after) ||
                        memcmp(before, after, sizeof before) != 0;
    return CLI_OK;
}

/* The `falcon` entry naming the jumper, first in BootOrder, on this
 * machine; for another machine's disk, the line to run there. */
static cli_status jump_entry(const upd_ctx *c)
{
    unsigned part = c->r.parts[c->f.esp_boot].number;
    int num = -1;

    if (stn_fw_here(&c->d))
        return stn_fw_jump_entry(c->d.path, part, &num);
    log_info("firmware entry: not this machine's (an image, or a machine "
             "that did not boot with UEFI); on the disk's machine delete a "
             "%s entry that names \\EFI\\falcon\\ and run efibootmgr "
             "--create --disk <disk> --part %u --label %s --loader '%s'",
             STN_FW_LABEL, part, STN_FW_LABEL, STN_ESP_JUMP_PATH);
    return CLI_OK;
}

/* ---- the command ---- */

/* dir/name as the ESP is named to a person, \EFI\BOOT\grubx64.efi. */
static const char *dos_path(const char *dir, const char *name, char *out,
                            size_t cap)
{
    char *q;

    (void)snprintf(out, cap, "\\%s\\%s", dir, name);
    for (q = out; *q != '\0'; q++) {
        if (*q == '/')
            *q = '\\';
    }
    return out;
}

static void describe(const upd_ctx *c)
{
    const upd_plan *p = &c->p;
    char path[MKS_PATH_MAX];

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
        log_info("  write: p%u %s, signed with the stick's MOK",
                 c->r.parts[c->f.boot].number,
                 dos_path(MKS_ESP_BOOT_DIR, c->stick_name, path,
                          sizeof path));
    if (p->uefi_stick && c->stick_aa64)
        log_info("  write: p%u %s, signed with the stick's MOK",
                 c->r.parts[c->f.boot].number,
                 dos_path(MKS_ESP_BOOT_DIR, c->stick_name_aa64, path,
                          sizeof path));
    else if (p->uefi_stick && c->stick_aa64_add)
        log_info("  write: p%u's AArch64 chain, which it has not had: "
                 "%s, its MokManager and %s, signed with the stick's MOK",
                 c->r.parts[c->f.boot].number, MKS_ESP_SHIM_NAME_AA64,
                 c->stick_name_aa64);
    if (p->uefi_station && p->rename)
        log_info("  write: partition %u \\EFI\\falcon\\%s in place of %s, "
                 "signed with the station's MOK",
                 c->r.parts[c->f.esp_boot].number, STN_LOADER_NOSHIM,
                 STN_LOADER_OLD);
    else if (p->uefi_station)
        log_info("  write: partition %u %s, signed with the station's MOK",
                 c->r.parts[c->f.esp_boot].number,
                 dos_path(STN_ESP_DIR, c->station_name, path, sizeof path));
    if (p->uefi_station && p->jump != STN_JUMP_NONE)
        log_info("  write: %s, the jumper, signed with the station's MOK%s",
                 STN_ESP_JUMP_PATH,
                 p->jump == STN_JUMP_TAKE ? "; the other system's there "
                                            "saved first" : "");
    if (p->uefi_station && p->jump != STN_JUMP_NONE)
        log_info("  entry: %s naming %s, first in BootOrder%s", STN_FW_LABEL,
                 STN_ESP_JUMP_PATH,
                 stn_fw_here(&c->d) ? "" : " (on the disk's machine)");
    if (p->totp_reenroll)
        log_info("  totp: not resealed; PCR 4 measures the jumper from now "
                 "on, so the check is enrolled again after the next boot");
    if (p->totp)
        log_info("  reseal: the TOTP seed, to the new loader; a new secret "
                 "for the authenticator");
    if (p->kind == UPD_STICK)
        log_info("  keep: every record, the library, the shelf, shim, "
                 "MokManager, the certificate, the other partitions");
    else
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

    if ((c->p.bios || c->p.uefi_stick) && c->f.boot >= 0)
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

/* The AArch64 chain's shim and MokManager, when update adds the chain. */
static char g_aa64_shim[MKS_PATH_MAX];
static char g_aa64_mm[MKS_PATH_MAX];
static char g_aa64_mm_name[SHIM_NEXT_STAGE_MAX];

/* What update does with a stick's AArch64 chain. One it has: its loader is
 * rewritten, when this release has an AArch64 loader. One it has not (a
 * stick made before 0.2.0): added, shimaa64 and its MokManager from --shim
 * beside the loader, when both are here; otherwise the stick stays x86-64
 * only, and update says so. */
static void plan_aa64(upd_ctx *c, const char *from, const char *shim_dir)
{
    char loader[MKS_PATH_MAX];

    (void)snprintf(loader, sizeof loader, "%s/falcon-boot-aa64.efi", from);
    if (!util_is_file(loader)) {
        if (c->stick_aa64)
            log_warn("%s has no falcon-boot-aa64.efi; the stick's AArch64 "
                     "loader is left as it is", from);
        c->stick_aa64 = 0;
        return;
    }
    if (c->stick_aa64)
        return;
    if (shim_find(shim_dir, "aa64", g_aa64_shim, sizeof g_aa64_shim) !=
            CLI_OK ||
        shim_next_stage_name(g_aa64_shim, c->stick_name_aa64,
                             sizeof c->stick_name_aa64) != CLI_OK ||
        shim_mokmanager_name(g_aa64_shim, g_aa64_mm_name,
                             sizeof g_aa64_mm_name) != CLI_OK ||
        shim_find_mokmanager(g_aa64_shim, "aa64", g_aa64_mm_name, g_aa64_mm,
                             sizeof g_aa64_mm) != CLI_OK) {
        log_warn("%s; the stick stays x86-64 only", cli_error_msg());
        cli_error_clear();
        return;
    }
    c->stick_aa64_add = 1;
}

/* shimaa64 and its MokManager onto p1, as they ship: they carry their own
 * signatures (DESIGN Section 4). */
static cli_status add_aa64_chain(upd_ctx *c)
{
    char dst[MKS_PATH_MAX];
    cli_status rc;

    (void)snprintf(dst, sizeof dst, "%s/%s", MKS_ESP_BOOT_DIR,
                   MKS_ESP_SHIM_NAME_AA64);
    rc = stn_mtools_put(c->d.path, c->boot_off, g_aa64_shim, dst);
    if (rc == CLI_OK) {
        (void)snprintf(dst, sizeof dst, "%s/%s", MKS_ESP_BOOT_DIR,
                       g_aa64_mm_name);
        rc = stn_mtools_put(c->d.path, c->boot_off, g_aa64_mm, dst);
    }
    if (rc == CLI_OK)
        log_info("UEFI: /%s/%s and %s written", MKS_ESP_BOOT_DIR,
                 MKS_ESP_SHIM_NAME_AA64, g_aa64_mm_name);
    return rc;
}

static cli_status update_run_arch(const cli_args *a)
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
    if (c.p.uefi_stick)
        plan_aa64(&c, from, cli_value(a, "shim", DEFAULT_SHIM));
    if (c.p.kind == UPD_REFUSE)
        return set_error(CLI_ERR_INVALID, "%s: %s; nothing written",
                         c.d.path, c.p.why);
    describe(&c);
    rc = writable(&c);
    if (rc != CLI_OK)
        return rc;
    if (dry_run) {
        /* mkstick bios's dry run says it for itself. */
        if (c.p.uefi_stick || c.p.uefi_station) {
            rc = open_key(&c, a, &sk, &keys, 1);
            shelf_key_close(&sk);
            if (rc != CLI_OK)
                return rc;
        }
        if (c.p.bios)
            rc = do_bios(&c, a, from, 1);
        else
            log_info("--dry-run: nothing written");
        return rc;
    }
    if (!cli_flag(a, "yes") && !confirm())
        return set_error(CLI_ERR_USAGE, "not confirmed; nothing written");

    if (c.p.uefi_stick || c.p.uefi_station) {
        rc = open_key(&c, a, &sk, &keys, 0);
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
                     c.stick_name, src, "stick-loader", 0, 0, workdir);
    }
    if (rc == CLI_OK && c.p.uefi_stick && c.stick_aa64_add)
        rc = add_aa64_chain(&c);
    if (rc == CLI_OK && c.p.uefi_stick && (c.stick_aa64 || c.stick_aa64_add)) {
        (void)snprintf(src, sizeof src, "%s/falcon-boot-aa64.efi", from);
        rc = do_uefi(&c, a, &keys, c.boot_off, c.boot_len, MKS_ESP_BOOT_DIR,
                     c.stick_name_aa64, src, "stick-loader-aa64", 0,
                     c.stick_aa64_add, workdir);
    }
    if (rc == CLI_OK && c.p.uefi_station) {
        (void)snprintf(src, sizeof src, "%s/%s", from, stn_n()->loader_file);
        if (c.p.rename)
            rc = rename_loader(&c, a, &keys, src, workdir);
        else
            rc = do_uefi(&c, a, &keys, c.esp_off, c.esp_len, STN_ESP_DIR,
                         c.station_name, src, "station-loader", 1, 0,
                         workdir);
    }
    if (rc == CLI_OK && c.p.uefi_station && c.p.jump != STN_JUMP_NONE)
        rc = do_jump(&c, a, &keys, from, workdir);
    shelf_key_close(&sk);
    if (c.p.uefi_stick || c.p.uefi_station)
        (void)rmdir(workdir);
    if (rc == CLI_OK && c.p.uefi_station && c.p.jump != STN_JUMP_NONE)
        rc = jump_entry(&c);
    if (rc != CLI_OK)
        return rc;

    if (c.p.totp_reenroll || c.jump_moved) {
        log_warn("the TOTP check is not resealed: %s, so PCR 4 is not what "
                 "it was", c.p.totp_reenroll
                     ? "the jumper now starts the loader"
                     : "the jumper is a new image");
        log_warn("enroll it again: boot the updated station, then run "
                 "`falcon-boot-mkstick station totp reseal %s` from the "
                 "system it started", c.d.path);
    } else if (c.p.totp) {
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

static cli_status update_run(const cli_args *a)
{
    cli_status rc = stn_arch_option(a);

    return rc != CLI_OK ? rc : update_run_arch(a);
}

static const cli_opt update_opts[] = {
    { "arch", CLI_OPT_VALUE, 0, "<x64|aa64>",
      "the machine the station boots (default: this one's)" },
    { "from", CLI_OPT_VALUE, 0, "<dir>",
      "the built loaders (default " DEFAULT_FROM ")" },
    { "shim", CLI_OPT_VALUE, 0, "<dir>",
      "shimaa64.efi and its MokManager, for a stick that gains the AArch64 "
      "chain (default " DEFAULT_SHIM ")" },
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
    "  UEFI   the loader, signed with the install's MOK off its shelf (read\n"
    "         through the shelf's mapping when its OS has it open, else the\n"
    "         passphrase is asked for; the key stays in memory) or --key,\n"
    "         checked, written over the old one, which is saved first, and\n"
    "         read back. A station without shim gets \\EFI\\BOOT\\BOOTX64.EFI,\n"
    "         the jumper (another system's there saved first), and on this\n"
    "         machine the falcon entry naming it, first in BootOrder. One\n"
    "         from before the jumper is migrated: its loader becomes\n"
    "         falconx64.efi, its old entry goes, and [totp] is enrolled\n"
    "         again (station totp reseal) after the next boot.\n"
    "  TOTP   a station with [totp] is resealed to the new loader and\n"
    "         prints a new secret; replace the authenticator's entry.\n"
    "\n"
    "It keeps every record, STATION.INI, /uki, shim, MokManager, the\n"
    "certificate and the firmware's boot entries, and clears the next-boot\n"
    "marker.\n",
    update_run
};
