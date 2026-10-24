/* falcon-boot mkstick: `station install`, falcon onto a machine's own disk
 * as its boot loader (docs/DESIGN.md Section 12.9).
 *
 * The disk is someone's. It has a Windows, or a Linux, or both, and the
 * installer's whole contract with it is DESIGN 12.9: it takes space that is
 * already free and never moves a partition; everything it will change is
 * copied to the backup file before the first byte is written; LBA 0 goes
 * down last, so an install that stops anywhere leaves the old boot code in
 * charge of a disk whose partitions are all still there.
 *
 * What it writes, by firmware:
 *
 *   BIOS  the station partition in free space (MBR: a primary of type 0x1C;
 *         GPT: the falcon boot type, with the FALCON-BIOS EF02 partition at
 *         LBA 34 made for stage 2 when the disk has none), FAT32 with room
 *         in its reserved sectors, its boot record, STATION.INI and the
 *         other files; stage 2 before the first partition, stage 3 and the
 *         marker where the record says; stage 1 into LBA 0's boot code.
 *   UEFI  \EFI\falcon\ on the machine's ESP: the station loader signed
 *         with the MOK as falconx64.efi, the public cert, STATION.INI and a
 *         signed memtest86+; the jumper as \EFI\BOOT\BOOTX64.EFI, another
 *         system's there saved beside the backup first; the ESP's boot
 *         record; a firmware entry named falcon, first in BootOrder, when
 *         the disk is this machine's. The entry names the jumper, which
 *         starts the loader, and the firmware verifies both against db,
 *         which must hold the cert. With --shim <dir>, shim and MokManager from dir go
 *         before it and the loader is shim's next stage (grubx64.efi),
 *         with no jumper: the entry names shim, and the cert is enrolled
 *         with MokManager instead.
 *
 * Both are written by default where the disk allows them (an ESP for UEFI;
 * room before the first partition for BIOS's stage 2). The station's
 * install ID is new; the shelf it is pointed at is stamped with it.
 */

#include <mkstick/station.h>

#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <common/datadir.h>
#include <common/log.h>
#include <common/jmem.h>
#include <common/proc.h>
#include <common/util.h>
#include <mkstick/bioscore.h>
#include <mkstick/cleanup.h>
#include <mkstick/keys.h>
#include <mkstick/mkstick.h>
#include <mkstick/pass.h>
#include <mkstick/shimname.h>

#include <crypto/crypto.h>
#include <fs/fs.h>
#include <ident/ident.h>
#include <luks/luks.h>
#include <media/shelf.h>
#include <media/stationini.h>
#include <part/part.h>
#include <part/table.h>

#include <falcon-bios/src/start/biosdefs.h>

#define DEFAULT_STAGE1        FALCON_DATADIR "/bin/falcon-bios-stage1.bin"
#define DEFAULT_STAGE2        FALCON_DATADIR "/bin/falcon-bios-stage2.bin"
#define DEFAULT_STAGE3        FALCON_DATADIR "/bin/falcon-bios-station-stage3.bin"
#define DEFAULT_LEGACY_STAGE3 FALCON_DATADIR "/bin/falcon-bios-station-legacy-stage3.bin"
#define DEFAULT_LOADER        FALCON_DATADIR "/bin/falcon-boot-station.efi"
#define DEFAULT_ESP_JUMP      FALCON_DATADIR "/bin/falcon-esp-jump.efi"
#define DEFAULT_SHIM_DIR      FALCON_DATADIR "/shim/bins"
#define DEFAULT_MODS          FALCON_DATADIR "/bin/mod"

#define STAGE_MAX  ((size_t)4 * 1024 * 1024)
#define INI_MAX    8192u
#define SHELF_KEY_MAX ((size_t)64 * 1024)

static const cli_opt install_opts[] = {
    { "bios", CLI_OPT_FLAG, 0, NULL,
      "install the BIOS station (default: both, where the disk allows)" },
    { "uefi", CLI_OPT_FLAG, 0, NULL,
      "install the UEFI station (default: both, where the disk allows)" },
    { "size", CLI_OPT_VALUE, 0, "<MiB>",
      "the station partition's size (default 64)" },
    { "shelf", CLI_OPT_VALUE, 0, "<n|disk:n|partition>",
      "the shelf the station boots Linux from; stamped with its install ID" },
    { "esp", CLI_OPT_VALUE, 0, "<n>",
      "the ESP's partition number (default: the disk's ESP)" },
    { "memtest", CLI_OPT_VALUE, 0, "<file>",
      "memtest86+ (x86 boot protocol build) for the BIOS station" },
    { "memtest-efi", CLI_OPT_VALUE, 0, "<file>",
      "memtest86+'s .efi for the UEFI station, signed with the MOK" },
    { "legacy", CLI_OPT_FLAG, 0, NULL,
      "station-legacy: its stage 3, jump's drivers, and --uki in /uki" },
    { "uki", CLI_OPT_VALUE, 1, "<file>",
      "with --legacy: a UKI for the station partition's /uki" },
    { "mods", CLI_OPT_VALUE, 0, "<dir>",
      "with --legacy: the drivers' directory (default " DEFAULT_MODS ")" },
    { "key", CLI_OPT_VALUE, 0, "<file>",
      "the MOK private key (default: the shelf's)" },
    { "cert", CLI_OPT_VALUE, 0, "<file>",
      "the MOK certificate, PEM (default: the shelf's)" },
    { "passphrase-file", CLI_OPT_VALUE, 0, "<file>",
      "unlock a LUKS2 shelf with this file's first line" },
    { "also", CLI_OPT_VALUE, 1, "<disk>",
      "another disk or image of the same machine, scanned for systems" },
    { "no-scan", CLI_OPT_FLAG, 0, NULL,
      "scan only the disk and --also, not every disk of this machine (a "
      "disk being prepared for another machine)" },
    { "backup", CLI_OPT_VALUE, 0, "<file>",
      "where everything this changes is saved first (default "
      "falcon-station-backup-<disk>.bin)" },
    { "dry-run", CLI_OPT_FLAG, 0, NULL,
      "report the plan and STATION.INI, write nothing" },
    { "force", CLI_OPT_FLAG, 0, NULL, "overwrite an existing backup file" },
    { "stage1", CLI_OPT_VALUE, 0, "<file>", "default " DEFAULT_STAGE1 },
    { "stage2", CLI_OPT_VALUE, 0, "<file>", "default " DEFAULT_STAGE2 },
    { "stage3", CLI_OPT_VALUE, 0, "<file>", "default " DEFAULT_STAGE3 },
    { "loader", CLI_OPT_VALUE, 0, "<file>", "default " DEFAULT_LOADER },
    { "shim", CLI_OPT_VALUE, 0, "<dir>",
      "UEFI through shim: shimx64.efi and its MokManager from dir ("
      DEFAULT_SHIM_DIR " is where fetch-shims.sh puts them); without it "
      "the firmware entry names the jumper at \\EFI\\BOOT\\BOOTX64.EFI, "
      "which the firmware's db must trust" }
};

/* ---- the shelf ---- */

typedef struct {
    int      set;
    char     disk[MKS_PATH_MAX];   /* what is read: a disk, image or node */
    uint64_t off;
    uint64_t len;
    int      disk_is_block;
    int      whole_node;           /* `disk` is the partition itself */
    int      luks;
    int      has_rec;
    ident_rec rec;
    uint32_t number;               /* on a disk; 0 for a node */
} shelf_ref;

static cli_status shelf_resolve(const char *spec, const stn_scan *s,
                                shelf_ref *out)
{
    static mks_records r;
    const char *colon = strrchr(spec, ':');
    char disk[MKS_PATH_MAX];
    unsigned long n = 0;
    char *end = NULL;
    mks_disk d;
    int i;
    cli_status rc;

    memset(out, 0, sizeof *out);
    out->set = 1;
    int on_target;
    int on_disk = 0;

    n = strtoul(spec, &end, 10);
    on_target = (end != spec && *end == '\0');
    if (!on_target && colon != NULL && colon[1] != '\0') {
        n = strtoul(colon + 1, &end, 10);
        on_disk = (end != colon + 1 && *end == '\0');
    }
    if (on_target) {
        (void)snprintf(disk, sizeof disk, "%s", s->disks[0].d.path);
    } else if (on_disk) {
        (void)snprintf(disk, sizeof disk, "%.*s", (int)(colon - spec), spec);
    } else {
        /* A partition node, read and stamped as itself. */
        rc = disk_stat(&d, spec);
        if (rc != CLI_OK)
            return rc;
        (void)snprintf(out->disk, sizeof out->disk, "%s", d.path);
        out->len = d.size;
        out->disk_is_block = d.is_block;
        out->whole_node = 1;
        return CLI_OK;
    }
    rc = disk_stat(&d, disk);
    if (rc != CLI_OK)
        return rc;
    rc = records_read(&r, d.path, d.sector, d.size);
    if (rc != CLI_OK)
        return rc;
    i = (r.rc == PART_OK) ? stn_part_index(&r, (uint32_t)n) : -1;
    if (i < 0)
        return set_error(CLI_ERR_NOTFOUND, "--shelf %s: %s has no partition "
                         "%lu", spec, d.path, n);
    (void)snprintf(out->disk, sizeof out->disk, "%s", d.path);
    out->off = r.parts[i].start_lba * d.sector;
    out->len = (r.parts[i].end_lba - r.parts[i].start_lba + 1u) * d.sector;
    out->disk_is_block = d.is_block;
    out->number = (uint32_t)n;
    return CLI_OK;
}

/* A shelf's record, read with the loaders' code; and whether it is LUKS2. */
static void shelf_probe(shelf_ref *sh)
{
    static const uint8_t magic[6] = { 'L', 'U', 'K', 'S', 0xba, 0xbe };
    static char json[LUKS_JSON_CAP];
    static char text[IDENT_TEXT_MAX + 1u];
    uint8_t first[512];
    luks_vol vol;
    disk_io io;
    size_t got = 0;

    sh->has_rec = 0;
    if (disk_io_open(&io, sh->disk, sh->off, sh->len) != CLI_OK) {
        cli_error_clear();
        return;
    }
    if (disk_io_read(&io, 0, sizeof first, first) == 0 &&
        memcmp(first, magic, sizeof magic) == 0) {
        uint32_t ver = 0;
        const char *role = NULL;
        const char *id = NULL;
        size_t rl = 0;
        size_t il = 0;

        sh->luks = 1;
        if (luks_open(&vol, disk_io_read, &io, json, sizeof json) ==
                LUKS_OK &&
            luks_falcon_token(&vol, &ver, &role, &rl, &id, &il) == LUKS_OK &&
            ident_from_strings(ver, role, rl, id, il, &sh->rec) == IDENT_OK)
            sh->has_rec = 1;
        disk_io_close(&io);
        return;
    }
    disk_io_close(&io);
    if (stn_read_file(sh->disk, sh->off, sh->len, IDENT_FILE, text,
                      sizeof text, &got) == FS_OK &&
        ident_decode_text(text, got, &sh->rec) == IDENT_OK)
        sh->has_rec = 1;
}

/* The shelf's contents, through core's LUKS2 and ext4 as the loaders read
 * it: the UKIs it offers, and the MOK pair when `keydir` is given. A LUKS2
 * shelf needs the passphrase. */
typedef struct {
    disk_io          io;
    luks_vol         vol;
    luks_master_key  mk;
    luks_dev         dev;
    fs_vol           fs;
    char             json[LUKS_JSON_CAP];
    shelf_list       list;
    fs_dir           scratch;
    fs_file          file;
    uint8_t          buf[SHELF_KEY_MAX];
} shelf_read_ctx;

static cli_status shelf_copy(shelf_read_ctx *c, const char *path,
                             const char *dst, mode_t mode)
{
    fs_dirent ent;
    cli_status rc;

    uint64_t size;

    if (fs_lookup(&c->fs, path, &c->scratch, &ent) != FS_OK || ent.is_dir ||
        fs_file_open(&c->fs, &c->file, &ent) != FS_OK)
        return set_error(CLI_ERR_NOTFOUND, "the shelf has no %s", path);
    size = fs_file_size(&c->file);
    if (size == 0u || size > sizeof c->buf)
        return set_error(CLI_ERR_INVALID, "%s on the shelf is not a key or "
                         "certificate", path);
    if (fs_file_read(&c->file, 0, size, c->buf) != FS_OK)
        return set_error(CLI_ERR_IO, "reading %s off the shelf", path);
    rc = util_write_file(dst, c->buf, (size_t)size, mode);
    crypto_wipe(c->buf, sizeof c->buf);
    return rc;
}

/* An argon2id keyslot's memory: the install runs on a machine with a
 * libc, so it is malloc's. */
static void *kdf_get(void *ctx, uint32_t kib)
{
    (void)ctx;
    return malloc((size_t)kib * 1024u);
}

static void kdf_put(void *ctx, void *mem, uint32_t kib)
{
    (void)ctx;
    (void)kib;
    free(mem);
}

static const luks_kdf_mem kdf_mem = { kdf_get, kdf_put, NULL, NULL, NULL };

static cli_status shelf_read(const shelf_ref *sh, const mks_pass *pass,
                             stn_scan *s, const char *keydir)
{
    static shelf_read_ctx c;
    const fs_ops *ops;
    fs_jinfo jinfo;
    fs_read_fn read = disk_io_read;
    void *ctx = &c.io;
    uint32_t slot = 0;
    char dst[MKS_PATH_MAX];
    size_t u;
    cli_status rc = CLI_OK;

    memset(&c, 0, sizeof c);
    rc = disk_io_open(&c.io, sh->disk, sh->off, sh->len);
    if (rc != CLI_OK)
        return rc;
    if (sh->luks) {
        if (luks_open(&c.vol, disk_io_read, &c.io, c.json, sizeof c.json) !=
            LUKS_OK) {
            rc = set_error(CLI_ERR_INVALID, "the shelf's LUKS2 header does "
                           "not read");
            goto out;
        }
        if (luks_unlock(&c.vol, pass->buf, pass->len, &kdf_mem, &c.mk,
                        &slot) != LUKS_OK) {
            rc = set_error(CLI_ERR_PERM, "the passphrase does not unlock "
                           "the shelf");
            goto out;
        }
        if (luks_dev_open(&c.dev, &c.vol, &c.mk) != LUKS_OK) {
            rc = set_error(CLI_ERR_INVALID, "the shelf's cipher is not one "
                           "the loaders read");
            goto out;
        }
        read = luks_read;
        ctx = &c.dev;
    }
    ops = fs_identify(read, ctx, FS_HINT_SHELF);
    if (ops == NULL ||
        fs_mount_journal(&c.fs, ops, read, ctx, &cli_jmem, &jinfo) != FS_OK) {
        rc = set_error(CLI_ERR_INVALID, "the shelf holds no filesystem the "
                       "loaders read");
        goto out;
    }
    if (shelf_collect(&c.list, &c.fs) > 0) {
        for (u = 0; u < c.list.n && s->n_found < STN_MAX_FOUND; u++) {
            stn_found *f = &s->found[s->n_found];
            uint8_t name[FS_NAME_MAX + 1u];
            size_t n;

            if (shelf_is_appliance(&c.list.uki[u].ent))
                continue;
            n = fs_name_to_bytes(c.list.uki[u].ent.name, name,
                                 sizeof name - 1u);
            name[n] = '\0';
            memset(f, 0, sizeof *f);
            f->kind = STN_F_UKI;
            f->locked = (uint8_t)(sh->luks != 0);
            if (stn_name_uki((const char *)name, f->name, sizeof f->name) !=
                0) {
                log_warn("shelf: %s has a name the INI cannot use as a "
                         "default", (const char *)name);
                f->name[0] = '\0';
            }
            (void)snprintf(f->label, sizeof f->label, "%.63s",
                           (const char *)name);
            s->n_found++;
        }
    }
    if (keydir != NULL) {
        rc = util_join(dst, sizeof dst, keydir, "falcon-mok.key");
        if (rc == CLI_OK)
            rc = shelf_copy(&c, "/" MKS_P3_KEY_NAME, dst, 0600);
        if (rc == CLI_OK)
            rc = util_join(dst, sizeof dst, keydir, "falcon-mok.crt");
        if (rc == CLI_OK)
            rc = shelf_copy(&c, "/" MKS_P3_CERT_NAME, dst, 0644);
    }
out:
    shelf_wipe(&c.list);
    if (sh->luks)
        luks_dev_close(&c.dev);
    crypto_wipe(&c.mk, sizeof c.mk);
    disk_io_close(&c.io);
    return rc;
}

/* A name the tools can open the shelf by: the partition itself, the
 * kernel's partition node of a disk, an image with the partition's offset
 * (debugfs), or a loop device over the partition inside an image
 * (cryptsetup, which needs root for it). */
static cli_status shelf_node(const shelf_ref *sh, char *node, size_t cap)
{
    char out[MKS_PATH_MAX];
    char real[MKS_PATH_MAX];
    char off[32];
    char len[32];
    size_t n = 0;
    cli_status rc;

    if (sh->whole_node) {
        (void)snprintf(node, cap, "%s", sh->disk);
        return CLI_OK;
    }
    /* The kernel names partitions from its own node (nvme0n1p2); a by-id
     * link's are "-part2", so the name is built from the resolved path. */
    if (sh->disk_is_block) {
        if (realpath(sh->disk, real) == NULL)
            (void)snprintf(real, sizeof real, "%s", sh->disk);
        return disk_partition_name(real, sh->number, node, cap);
    }
    /* e2fsprogs opens a filesystem at an offset of a file itself;
     * cryptsetup does not, and gets a loop device. */
    if (!sh->luks) {
        if (snprintf(node, cap, "%s?offset=%llu", sh->disk,
                     (unsigned long long)sh->off) >= (int)cap)
            return set_error(CLI_ERR_RANGE, "the shelf's path is too long");
        return CLI_OK;
    }
    (void)snprintf(off, sizeof off, "%llu", (unsigned long long)sh->off);
    (void)snprintf(len, sizeof len, "%llu", (unsigned long long)sh->len);
    rc = proc_capture_l(out, sizeof out, &n, "losetup", "--show", "-f", "-o",
                        off, "--sizelimit", len, sh->disk, PROC_END);
    if (rc != CLI_OK)
        return rc;
    while (n > 0u && (out[n - 1u] == '\n' || out[n - 1u] == '\r'))
        out[--n] = '\0';
    rc = cleanup_push_loop(out);
    if (rc != CLI_OK)
        return rc;
    (void)snprintf(node, cap, "%s", out);
    return CLI_OK;
}

static cli_status shelf_stamp(const shelf_ref *sh,
                              const uint8_t id[IDENT_ID_LEN])
{
    char node[MKS_PATH_MAX];
    char hex[33];
    ident_rec rec;
    int looped;
    cli_status rc;

    looped = !sh->whole_node && !sh->disk_is_block && sh->luks;
    rc = shelf_node(sh, node, sizeof node);
    if (rc != CLI_OK)
        return rc;
    records_id_hex(id, hex);

    if (sh->luks) {
        char json[192];
        char out[4096];
        int n;
        unsigned t;

        /* A shelf carries one falcon token: an earlier one goes first. */
        for (t = 0; t < 32u; t++) {
            char tid[8];
            size_t len = 0;

            (void)snprintf(tid, sizeof tid, "%u", t);
            if (proc_capture_l(out, sizeof out, &len, "cryptsetup", "token",
                               "export", "--token-id", tid, node,
                               PROC_END) != CLI_OK) {
                cli_error_clear();
                continue;
            }
            out[len < sizeof out ? len : sizeof out - 1u] = '\0';
            if (strstr(out, "\"type\":\"falcon\"") == NULL)
                continue;
            rc = proc_runl_quiet("cryptsetup", "token", "remove",
                                 "--token-id", tid, node, PROC_END);
            if (rc != CLI_OK)
                goto out;
        }
        n = snprintf(json, sizeof json,
                     "{\"type\":\"falcon\",\"keyslots\":[],\"version\":%u,"
                     "\"role\":\"shelf\",\"install_id\":\"%s\"}",
                     (unsigned)IDENT_VERSION, hex);
        rc = proc_input_l(json, (size_t)n, "cryptsetup", "token", "import",
                          node, PROC_END);
    } else {
        char tmp[] = "/tmp/falcon-shelf.XXXXXX";
        char text[IDENT_TEXT_MAX];
        char cmd[128];
        size_t n;
        int fd;

        memset(&rec, 0, sizeof rec);
        rec.version = IDENT_VERSION;
        rec.role = IDENT_ROLE_SHELF;
        memcpy(rec.install_id, id, IDENT_ID_LEN);
        n = ident_encode_text(&rec, text, sizeof text);
        fd = mkstemp(tmp);
        if (fd < 0 || write(fd, text, n) != (ssize_t)n) {
            if (fd >= 0)
                (void)close(fd);
            rc = set_error_errno(CLI_ERR_IO, "writing %s", tmp);
            goto out;
        }
        (void)close(fd);
        /* debugfs writes the file into the unmounted ext4 without a mount:
         * rm first, since write refuses a name that exists. */
        if (sh->has_rec)
            (void)proc_runl_quiet("debugfs", "-w", "-R", "rm /.falcon", node,
                                  PROC_END);
        (void)snprintf(cmd, sizeof cmd, "write %s .falcon", tmp);
        rc = proc_runl_quiet("debugfs", "-w", "-R", cmd, node, PROC_END);
        (void)unlink(tmp);
    }
out:
    if (looped)
        (void)cleanup_pop();
    if (rc != CLI_OK)
        return rc;

    /* Read back as a loader reads it: the stamp is only done when it
     * decodes to this install. */
    {
        shelf_ref check = *sh;

        shelf_probe(&check);
        if (!check.has_rec ||
            memcmp(check.rec.install_id, id, IDENT_ID_LEN) != 0 ||
            check.rec.role != IDENT_ROLE_SHELF)
            return set_error(CLI_ERR_IO, "the shelf's record did not read "
                             "back as install %s", hex);
    }
    log_info("shelf: stamped with install ID %s (%s)", hex,
             sh->luks ? "LUKS2 token" : IDENT_FILE);
    return CLI_OK;
}

/* ---- the plan ---- */

typedef struct {
    /* what */
    int bios;
    int uefi;
    int no_shim;                /* UEFI: the entry names the jumper */
    /* BIOS */
    stn_region region;
    unsigned   slot;            /* MBR slot, or GPT entry number */
    uint64_t   first_real;      /* the first partition not stage 2's */
    uint64_t   ef02_start;      /* GPT: stage 2's partition */
    uint64_t   ef02_end;
    int        make_ef02;
    unsigned   ef02_slot;
    uint32_t   reserved;
    bios_plan  bplan;
    /* UEFI */
    int        esp;             /* index in the target's records */
    bios_fat   esp_fat;
    /* the INIs */
    char       ini_bios[INI_MAX];
    size_t     ini_bios_len;
    char       ini_uefi[INI_MAX];
    size_t     ini_uefi_len;
} stn_plan;

static int is_bios_boot(const part_table *t, const part_entry *p)
{
    return t->id.kind == PART_KIND_GPT &&
           gpt_guid_eq(p->type_guid, stn_guid_bios_boot);
}

static unsigned gpt_free_number(const mks_records *r, unsigned skip)
{
    unsigned n;

    for (n = 1; n <= 128u; n++) {
        if (n != skip && stn_part_index(r, n) < 0)
            return n;
    }
    return 0;
}

static cli_status plan_bios(const stn_disk *td, uint64_t want, size_t l2,
                            size_t l3, stn_plan *p, int required)
{
    const mks_records *r = &td->r;
    uint32_t sec = td->d.sector;
    uint64_t disk_sectors = td->d.size / sec;
    uint64_t lo;
    uint64_t hi;
    uint64_t largest = 0;
    uint64_t largest4k = 0;
    bios_disk bd;
    uint32_t i;
    stn_status st;
    bioscore_status bs;
    char need[32];
    char have[32];

    p->first_real = 0;
    for (i = 0; i < r->table.count; i++) {
        const part_entry *e = &r->parts[i];

        if (is_bios_boot(&r->table, e)) {
            p->ef02_start = e->start_lba;
            p->ef02_end = e->end_lba;
            continue;
        }
        if (p->first_real == 0u || e->start_lba < p->first_real)
            p->first_real = e->start_lba;
    }

    if (r->table.id.kind == PART_KIND_MBR) {
        p->slot = stn_mbr_free_slot(td->lba0);
        if (p->slot == 0u)
            return set_error(CLI_ERR_RANGE, "%s: the MBR's four primary "
                             "slots are all taken; the station partition "
                             "needs one", td->d.path);
        lo = stn_first_allowed(&r->table, (uint64_t)1024u * 1024u / sec);
        hi = disk_sectors - 1u;
        if (hi > 0xFFFFFFFFu)
            hi = 0xFFFFFFFFu;
    } else {
        lo = stn_first_allowed(&r->table, r->table.first_usable);
        if (p->first_real != 0u && lo < p->first_real)
            lo = p->first_real;
        hi = r->table.last_usable;
    }

    st = stn_find_free(&r->table, lo, hi, want,
                       stn_align_for(STN_ALIGN_MIB, sec), &p->region,
                       &largest);
    if (st == STN_ERR_NOSPACE)
        st = stn_find_free(&r->table, lo, hi, want,
                           stn_align_for(STN_ALIGN_4K, sec), &p->region,
                           &largest4k);
    if (st != STN_OK) {
        (void)util_format_size(want * sec, need, sizeof need);
        (void)util_format_size((largest4k > largest ? largest4k : largest) *
                                   sec, have, sizeof have);
        return set_error(CLI_ERR_RANGE,
                         "%s has no free space for the station partition: "
                         "it needs %s (%llu sectors) outside every "
                         "partition, and the largest free run is %s. Shrink "
                         "a filesystem and its partition first (ntfsresize, "
                         "resize2fs); this tool resizes nothing",
                         td->d.path, need, (unsigned long long)want, have);
    }

    if (r->table.id.kind == PART_KIND_GPT) {
        if (p->ef02_start == 0u) {
            uint64_t gap_end = (p->first_real != 0u ? p->first_real
                                                    : p->region.start) - 1u;

            if (gap_end < (uint64_t)STAGE2_LBA ||
                gap_end - STAGE2_LBA + 1u < (l2 + sec - 1u) / sec) {
                if (!required)
                    return set_error(CLI_ERR_RANGE, "no room before the "
                                     "first partition");
                return set_error(CLI_ERR_RANGE,
                                 "%s has no room for stage 2 between LBA %u "
                                 "and its first partition (LBA %llu); the "
                                 "BIOS station needs %zu sectors there",
                                 td->d.path, (unsigned)STAGE2_LBA,
                                 (unsigned long long)p->first_real,
                                 (l2 + sec - 1u) / sec);
            }
            p->make_ef02 = 1;
            p->ef02_start = STAGE2_LBA;
            p->ef02_end = gap_end;
            p->ef02_slot = gpt_free_number(r, 0);
        }
        p->slot = gpt_free_number(r, p->ef02_slot);
        if (p->slot == 0u || (p->make_ef02 && p->ef02_slot == 0u))
            return set_error(CLI_ERR_RANGE, "%s: the GPT has no free entry",
                             td->d.path);
    }

    p->reserved = bioscore_reserved_for(BOOTPART_STAGE3_SECTOR, l3);
    if (p->reserved == 0u)
        return set_error(CLI_ERR_RANGE, "stage 3 is too large for a FAT "
                         "reserved area");

    /* The plan the stick's own `bios` makes, over the partition as it
     * will be: its FAT is described from the mkfs.fat line below, and
     * checked against the real one once it exists. */
    memset(&bd, 0, sizeof bd);
    bd.sector_bytes = td->d.sector;
    bd.kind = r->table.id.kind;
    bd.first_usable = (bd.kind == PART_KIND_GPT) ? r->table.first_usable : 1u;
    bd.first_part_lba = p->first_real;
    if (bd.first_part_lba == 0u || p->region.start < bd.first_part_lba)
        bd.first_part_lba = p->region.start;
    bd.boot_start = p->region.start;
    bd.boot_sectors = p->region.sectors;
    bd.fat.sector = td->d.sector;
    bd.fat.reserved = p->reserved;
    bd.fat.fsinfo = 1;
    bd.fat.backup = 6;
    bd.fat.fat32 = 1;
    bd.stage3_sector = BOOTPART_STAGE3_SECTOR;
    bd.marker_sector = BOOTPART_MARKER_SECTOR;
    bs = bioscore_plan(&bd, bd.kind == PART_KIND_GPT ? p->ef02_start
                                                     : STAGE2_LBA,
                       STAGE2_LBA_MBR, STAGE2_SECTORS, l2, l3, &p->bplan);
    if (bs != BIOSCORE_OK)
        return set_error(CLI_ERR_RANGE, "%s: %s", td->d.path,
                         bioscore_strerror(bs));
    if (bd.kind == PART_KIND_GPT &&
        p->bplan.stage2_lba + p->bplan.stage2_sectors - 1u > p->ef02_end)
        return set_error(CLI_ERR_RANGE, "stage 2 does not fit the %s "
                         "partition (LBA %llu..%llu)", BIOS_BOOT_NAME,
                         (unsigned long long)p->ef02_start,
                         (unsigned long long)p->ef02_end);
    return CLI_OK;
}

/* The disks the INI names that have no signature get one, as Windows NT
 * gives one on its first boot: random, nonzero, unlike every other disk's. */
static cli_status assign_signatures(stn_scan *s, int uefi_only)
{
    size_t k;
    size_t i;

    for (k = 0; k < s->n_disks; k++) {
        stn_disk *sd = &s->disks[k];
        int named = 0;

        if (sd->r.table.id.kind != PART_KIND_MBR || sd->r.table.id.mbr_sig
            != 0u)
            continue;
        for (i = 0; i < s->n_found; i++) {
            const stn_found *f = &s->found[i];

            if (f->kind != STN_F_UKI && f->kind != STN_F_FWENTRY &&
                (!uefi_only || f->kind == STN_F_EFIWIN) && f->disk_ix == k)
                named = 1;
        }
        if (!named)
            continue;
        for (;;) {
            uint32_t sig = 0;
            size_t j;
            int clash = 0;

            if (getrandom(&sig, sizeof sig, 0) != (ssize_t)sizeof sig)
                return set_error_errno(CLI_ERR_IO, "getrandom");
            for (j = 0; j < s->n_disks; j++) {
                if (s->disks[j].r.table.id.kind == PART_KIND_MBR &&
                    s->disks[j].r.table.id.mbr_sig == sig)
                    clash = 1;
                if (s->disks[j].needs_sig && s->disks[j].new_sig == sig)
                    clash = 1;
            }
            if (sig != 0u && !clash) {
                sd->new_sig = sig;
                break;
            }
        }
        sd->needs_sig = 1;
        for (i = 0; i < s->n_found; i++) {
            stn_found *f = &s->found[i];

            if (f->kind != STN_F_UKI && f->kind != STN_F_FWENTRY &&
                f->disk_ix == k) {
                f->disk.mbr_sig = sd->new_sig;
                (void)stn_name_disk(&f->disk, f->part, f->name,
                                    sizeof f->name);
                stn_found_label(f);
            }
        }
        sd->r.table.id.mbr_sig = sd->new_sig;
    }
    return CLI_OK;
}

static cli_status make_ini(const stn_found *all, size_t n, int uefi,
                           char *buf, size_t cap, size_t *len)
{
    static stationini ini;
    static stationini check;
    stn_found pick[STN_MAX_FOUND];
    size_t m = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        int take = uefi ? (all[i].kind == STN_F_UKI ||
                           all[i].kind == STN_F_FWENTRY ||
                           all[i].kind == STN_F_EFIWIN)
                        : (all[i].kind == STN_F_UKI ||
                           all[i].kind == STN_F_PART ||
                           all[i].kind == STN_F_DISK);

        if (take)
            pick[m++] = all[i];
    }
    /* On UEFI the firmware's own Windows entry is the default when there
     * is one; the ESP's bootmgfw.efi is how an image names it. */
    if (uefi) {
        int fw = 0;

        for (i = 0; i < m; i++)
            fw |= (pick[i].kind == STN_F_FWENTRY);
        if (fw) {
            size_t k = 0;

            for (i = 0; i < m; i++) {
                if (pick[i].kind != STN_F_EFIWIN)
                    pick[k++] = pick[i];
            }
            m = k;
        }
    }
    if (stn_ini_build(pick, m, STN_TIMEOUT, &ini) != STN_OK)
        return set_error(CLI_ERR_INTERNAL, "STATION.INI did not build");
    *len = stationini_format(&ini, buf, cap);
    /* Nothing found to name: no default and no section, which formats to
     * no text at all. The file is still written, so status and a person
     * find it where the next section goes. */
    if (*len == 0u && ini.n_entries == 0u && ini.default_name[0] == '\0') {
        static const char none[] =
            "# No default: the install found no system to name.\n";

        if (cap < sizeof none)
            return set_error(CLI_ERR_INTERNAL, "STATION.INI did not format");
        memcpy(buf, none, sizeof none);
        *len = sizeof none - 1u;
    }
    if (*len == 0u)
        return set_error(CLI_ERR_INTERNAL, "STATION.INI did not format");
    /* What the loader will make of it, before it is written. */
    if (stationini_parse(buf, *len, &check) != 0)
        return set_error(CLI_ERR_INTERNAL, "the STATION.INI written does not "
                         "parse: line %u: %s", check.err_line, check.why);
    return CLI_OK;
}

/* ---- the backup ---- */

static cli_status backup_add(stn_backup *b, const char *disk,
                             stn_run_kind kind, uint64_t lba,
                             uint64_t sectors)
{
    int d = stn_backup_disk(b, disk);

    if (d < 0)
        return set_error(CLI_ERR_RANGE, "too many disks for one backup");
    if (stn_backup_run(b, (uint32_t)d, kind, lba, sectors) != STN_OK)
        return set_error(CLI_ERR_RANGE, "too many runs for one backup");
    return CLI_OK;
}

static cli_status backup_write(const stn_backup *b, const char *path,
                               int force)
{
    static uint8_t hdr[STN_BK_HDR];
    uint32_t i;
    int fd;
    cli_status rc = CLI_OK;

    if (!force && util_is_file(path))
        return set_error(CLI_ERR_EXISTS, "%s exists; it may hold what a disk "
                         "was before an earlier install. Name another file, "
                         "or --force", path);
    if (stn_backup_encode(b, hdr) != STN_OK)
        return set_error(CLI_ERR_INTERNAL, "the backup header did not encode");
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return set_error_errno(CLI_ERR_IO, "creating %s", path);
    if (write(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr)
        rc = set_error_errno(CLI_ERR_IO, "writing %s", path);
    for (i = 0; i < b->n_runs && rc == CLI_OK; i++) {
        size_t bytes = (size_t)(b->runs[i].sectors * b->sector);
        uint8_t *buf = malloc(bytes);
        mks_dev dev;

        if (buf == NULL) {
            rc = set_error(CLI_ERR_NOMEM, "no memory for the backup");
            break;
        }
        dev.sector = b->sector;
        dev.fd = open(b->disks[b->runs[i].disk], O_RDONLY | O_CLOEXEC);
        if (dev.fd < 0)
            rc = set_error_errno(CLI_ERR_IO, "opening %s",
                                 b->disks[b->runs[i].disk]);
        else
            rc = dev_read(&dev, b->runs[i].lba, b->runs[i].sectors, buf);
        if (dev.fd >= 0)
            (void)close(dev.fd);
        if (rc == CLI_OK && write(fd, buf, bytes) != (ssize_t)bytes)
            rc = set_error_errno(CLI_ERR_IO, "writing %s", path);
        free(buf);
    }
    if (rc == CLI_OK && fsync(fd) != 0)
        rc = set_error_errno(CLI_ERR_IO, "flushing %s", path);
    if (close(fd) != 0 && rc == CLI_OK)
        rc = set_error_errno(CLI_ERR_IO, "closing %s", path);
    return rc;
}

/* The header again, for what is only known once written (the firmware
 * entry's number). */
static cli_status backup_rewrite_header(const stn_backup *b, const char *path)
{
    static uint8_t hdr[STN_BK_HDR];
    int fd;
    cli_status rc = CLI_OK;

    if (stn_backup_encode(b, hdr) != STN_OK)
        return set_error(CLI_ERR_INTERNAL, "the backup header did not encode");
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s", path);
    if (pwrite(fd, hdr, sizeof hdr, 0) != (ssize_t)sizeof hdr ||
        fsync(fd) != 0)
        rc = set_error_errno(CLI_ERR_IO, "writing %s", path);
    (void)close(fd);
    return rc;
}

/* ---- writing ---- */

static cli_status format_station(const stn_disk *td, const stn_plan *p)
{
    char ssz[16];
    char res[16];
    char start[24];
    char kib[24];

    (void)snprintf(ssz, sizeof ssz, "%u", (unsigned)td->d.sector);
    (void)snprintf(res, sizeof res, "%u", (unsigned)p->reserved);
    (void)snprintf(start, sizeof start, "%llu",
                   (unsigned long long)p->region.start);
    (void)snprintf(kib, sizeof kib, "%llu",
                   (unsigned long long)(p->region.sectors * td->d.sector /
                                        1024u));
    /* One sector a cluster keeps a partition this small FAT32 (65525
     * clusters at least); -h is what DOS-era code reads the partition's
     * place from; --mbr=n keeps mkfs.fat off LBA 0; -I because a whole
     * disk with partitions is what the target is. -S is the disk's sector:
     * the FAT's must match it (the BIOS reads the reserved sectors in the
     * disk's), and --offset and -h count in it, where an image file would
     * otherwise be taken for 512. */
    return proc_runl_quiet("mkfs.fat", "-I", "-F", "32", "-S", ssz, "-s",
                           "1", "-R", res, "-n", STN_FAT_LABEL, "-h", start,
                           "--offset", start, "--mbr=n", td->d.path, kib,
                           PROC_END);
}

static cli_status put_bios_files(const cli_args *a, const stn_disk *td,
                                 const stn_plan *p)
{
    uint64_t off = p->region.start * td->d.sector;
    const char *memtest = cli_value(a, "memtest", NULL);
    cli_status rc;
    size_t i;

    rc = stn_mtools_put_text(td->d.path, off, p->ini_bios, p->ini_bios_len,
                             STATIONINI_FILE);
    if (rc == CLI_OK && memtest != NULL)
        rc = stn_mtools_put(td->d.path, off, memtest, STN_BIOS_MEMTEST);
    if (rc != CLI_OK || !cli_flag(a, "legacy"))
        return rc;

    /* station-legacy: jump's drivers under the names the floppy gives
     * them, gzipped as the floppy carries them when the build made that. */
    {
        static const char *const mods[] = {
            "ATA.MOD", "EHCI.MOD", "UHCI.MOD", "OHCI.MOD", "PCC.MOD"
        };
        const char *dir = cli_value(a, "mods", DEFAULT_MODS);
        char src[MKS_PATH_MAX];

        for (i = 0; i < sizeof mods / sizeof mods[0] && rc == CLI_OK; i++) {
            (void)snprintf(src, sizeof src, "%s/%s.gz", dir, mods[i]);
            if (!util_is_file(src))
                (void)snprintf(src, sizeof src, "%s/%s", dir, mods[i]);
            if (!util_is_file(src)) {
                log_warn("no %s in %s; not copied", mods[i], dir);
                continue;
            }
            rc = stn_mtools_put(td->d.path, off, src, mods[i]);
        }
    }
    if (rc == CLI_OK && cli_count(a, "uki") > 0u)
        rc = stn_mtools_mkdir(td->d.path, off, "uki");
    for (i = 0; i < cli_count(a, "uki") && rc == CLI_OK; i++) {
        char dst[MKS_PATH_MAX];
        const char *u = cli_at(a, "uki", i);

        (void)snprintf(dst, sizeof dst, "uki/%s", util_basename(u));
        rc = stn_mtools_put(td->d.path, off, u, dst);
    }
    return rc;
}

static cli_status write_bios(const cli_args *a, const stn_disk *td,
                             stn_plan *p, const uint8_t *b2, size_t l2,
                             const uint8_t *b3, size_t l3,
                             const uint8_t id[IDENT_ID_LEN])
{
    uint8_t bs[BIOS_SECTOR_MAX];
    bios_fat fat;
    ident_rec rec;
    mks_dev dev;
    cli_status rc;

    if (td->r.table.id.kind == PART_KIND_GPT) {
        char n1[64], t1[80], c1[64], n2[64], t2[80], c2[64];

        (void)snprintf(n2, sizeof n2, "%u:%llu:%llu", p->slot,
                       (unsigned long long)p->region.start,
                       (unsigned long long)(p->region.start +
                                            p->region.sectors - 1u));
        (void)snprintf(t2, sizeof t2, "%u:%s", p->slot, MKS_TYPE_BOOT);
        (void)snprintf(c2, sizeof c2, "%u:%s", p->slot, STN_GPT_NAME);
        if (p->make_ef02) {
            (void)snprintf(n1, sizeof n1, "%u:%llu:%llu", p->ef02_slot,
                           (unsigned long long)p->ef02_start,
                           (unsigned long long)p->ef02_end);
            (void)snprintf(t1, sizeof t1, "%u:%s", p->ef02_slot,
                           BIOS_BOOT_TYPE);
            (void)snprintf(c1, sizeof c1, "%u:%s", p->ef02_slot,
                           BIOS_BOOT_NAME);
            rc = proc_runl_quiet("sgdisk", "-a", "1", "-n", n1, "-t", t1,
                                 "-c", c1, "-n", n2, "-t", t2, "-c", c2,
                                 td->d.path, PROC_END);
        } else {
            rc = proc_runl_quiet("sgdisk", "-a", "1", "-n", n2, "-t", t2,
                                 "-c", c2, td->d.path, PROC_END);
        }
        if (rc != CLI_OK)
            return rc;
        log_info("GPT: partition %u, %s%s", p->slot, STN_GPT_NAME,
                 p->make_ef02 ? ", and the FALCON-BIOS partition" : "");
    }

    rc = format_station(td, p);
    if (rc != CLI_OK)
        return rc;
    dev.sector = td->d.sector;
    dev.fd = open(td->d.path, O_RDWR | O_CLOEXEC);
    if (dev.fd < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s", td->d.path);
    rc = dev_read(&dev, p->region.start, 1, bs);
    if (rc == CLI_OK && (bioscore_fat_bpb(bs, &fat) != BIOSCORE_OK ||
                         fat.sector != td->d.sector ||
                         fat.reserved < p->reserved || !fat.fat32 ||
                         fat.fsinfo == BOOTPART_MARKER_SECTOR ||
                         fat.backup == BOOTPART_MARKER_SECTOR))
        rc = set_error(CLI_ERR_INVALID, "mkfs.fat made a FAT the plan does "
                       "not fit (%u-byte sectors, reserved %u; want %u, %u)",
                       (unsigned)fat.sector, (unsigned)fat.reserved,
                       (unsigned)td->d.sector, (unsigned)p->reserved);
    (void)close(dev.fd);
    if (rc != CLI_OK)
        return rc;
    log_info("station partition: FAT32, %u-byte sectors, %u reserved",
             (unsigned)fat.sector, (unsigned)fat.reserved);

    rc = put_bios_files(a, td, p);
    if (rc != CLI_OK)
        return rc;

    memset(&rec, 0, sizeof rec);
    rec.version = IDENT_VERSION;
    rec.role = IDENT_ROLE_BOOT;
    rec.stage3_sector = BOOTPART_STAGE3_SECTOR;
    rec.marker_sector = BOOTPART_MARKER_SECTOR;
    memcpy(rec.install_id, id, IDENT_ID_LEN);
    rc = records_write_boot(td->d.path, p->region.start * td->d.sector, &rec);
    if (rc != CLI_OK)
        return rc;

    dev.fd = open(td->d.path, O_RDWR | O_CLOEXEC);
    if (dev.fd < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s", td->d.path);
    rc = dev_write_stage(&dev, p->bplan.stage2_lba, b2, l2,
                         p->bplan.stage2_sectors);
    if (rc == CLI_OK)
        rc = dev_write_stage(&dev, p->bplan.stage3_lba, b3, l3,
                             p->bplan.stage3_sectors);
    if (rc == CLI_OK)
        rc = dev_zero(&dev, p->bplan.marker_lba, 1);
    if (rc == CLI_OK && fsync(dev.fd) != 0)
        rc = set_error_errno(CLI_ERR_IO, "flushing %s", td->d.path);
    (void)close(dev.fd);
    if (rc == CLI_OK)
        log_info("stage 2 at LBA %llu, stage 3 at LBA %llu, marker at LBA "
                 "%llu", (unsigned long long)p->bplan.stage2_lba,
                 (unsigned long long)p->bplan.stage3_lba,
                 (unsigned long long)p->bplan.marker_lba);
    return rc;
}

/* Another system's \EFI\BOOT\BOOTX64.EFI, copied beside the backup file
 * before the jumper replaces it, for `station remove` to put back. */
static cli_status save_foreign_jump(const stn_disk *td, const stn_plan *p,
                                    const char *backup)
{
    const part_entry *e = &td->r.parts[p->esp];
    uint64_t off = e->start_lba * td->d.sector;
    uint64_t len = (e->end_lba - e->start_lba + 1u) * td->d.sector;
    char dst[MKS_PATH_MAX];
    int present = 0;
    int ours = 0;
    cli_status rc;

    rc = stn_esp_jump_state(td->d.path, off, len, &present, &ours);
    if (rc != CLI_OK || stn_jump_plan(0, present, ours) != STN_JUMP_TAKE)
        return rc;
    if (snprintf(dst, sizeof dst, "%s%s", backup, STN_JUMP_BACKUP_EXT) >=
        (int)sizeof dst)
        return set_error(CLI_ERR_RANGE, "--backup is too long");
    rc = stn_mtools_get(td->d.path, off, STN_ESP_BOOT_DIR "/" STN_ESP_JUMP,
                        dst);
    if (rc != CLI_OK)
        return rc;
    log_info("\\EFI\\BOOT\\%s was another system's; saved to %s",
             STN_ESP_JUMP, dst);
    return CLI_OK;
}

static cli_status write_uefi(const stn_disk *td, const stn_plan *p,
                             const mok_keys *keys, const char *workdir,
                             const uint8_t id[IDENT_ID_LEN],
                             const cli_args *a)
{
    const part_entry *e = &td->r.parts[p->esp];
    uint64_t off = e->start_lba * td->d.sector;
    const char *shim_dir = cli_value(a, "shim", NULL);
    const char *loader = cli_value(a, "loader", DEFAULT_LOADER);
    const char *memtest = cli_value(a, "memtest-efi", NULL);
    char shim[MKS_PATH_MAX] = "";
    char mm[MKS_PATH_MAX] = "";
    char mm_name[SHIM_NEXT_STAGE_MAX] = "";
    char next[SHIM_NEXT_STAGE_MAX] = "";
    char signed_loader[MKS_PATH_MAX];
    char signed_mt[MKS_PATH_MAX];
    char signed_jump[MKS_PATH_MAX];
    char dst[MKS_PATH_MAX];
    ident_rec rec;
    cli_status rc = CLI_OK;

    if (p->no_shim) {
        (void)snprintf(next, sizeof next, "%s", STN_LOADER_NOSHIM);
    } else {
        rc = shim_find(shim_dir, "x64", shim, sizeof shim);
        if (rc == CLI_OK)
            rc = shim_next_stage_name(shim, next, sizeof next);
        if (rc == CLI_OK)
            rc = shim_mokmanager_name(shim, mm_name, sizeof mm_name);
        if (rc == CLI_OK)
            rc = shim_find_mokmanager(shim, "x64", mm_name, mm, sizeof mm);
    }
    if (rc == CLI_OK)
        rc = util_join(signed_loader, sizeof signed_loader, workdir, next);
    if (rc == CLI_OK)
        rc = mok_sign(keys, loader, signed_loader);
    if (rc == CLI_OK && memtest != NULL) {
        rc = util_join(signed_mt, sizeof signed_mt, workdir,
                       STN_ESP_MEMTEST);
        if (rc == CLI_OK)
            rc = mok_sign(keys, memtest, signed_mt);
    }
    if (rc == CLI_OK && p->no_shim) {
        rc = util_join(signed_jump, sizeof signed_jump, workdir,
                       STN_ESP_JUMP);
        if (rc == CLI_OK)
            rc = mok_sign(keys, DEFAULT_ESP_JUMP, signed_jump);
    }
    if (rc != CLI_OK)
        return rc;

    rc = stn_mtools_mkdir(td->d.path, off, STN_ESP_DIR);
    if (rc == CLI_OK && !p->no_shim) {
        (void)snprintf(dst, sizeof dst, "%s/%s", STN_ESP_DIR, STN_ESP_SHIM);
        rc = stn_mtools_put(td->d.path, off, shim, dst);
    }
    if (rc == CLI_OK && !p->no_shim) {
        (void)snprintf(dst, sizeof dst, "%s/%s", STN_ESP_DIR, mm_name);
        rc = stn_mtools_put(td->d.path, off, mm, dst);
    }
    if (rc == CLI_OK) {
        (void)snprintf(dst, sizeof dst, "%s/%s", STN_ESP_DIR, next);
        rc = stn_mtools_put(td->d.path, off, signed_loader, dst);
    }
    if (rc == CLI_OK) {
        (void)snprintf(dst, sizeof dst, "%s/%s", STN_ESP_DIR, STN_ESP_CERT);
        rc = stn_mtools_put(td->d.path, off, keys->cer, dst);
    }
    if (rc == CLI_OK) {
        (void)snprintf(dst, sizeof dst, "%s/%s", STN_ESP_DIR,
                       STATIONINI_FILE);
        rc = stn_mtools_put_text(td->d.path, off, p->ini_uefi,
                                 p->ini_uefi_len, dst);
    }
    if (rc == CLI_OK && memtest != NULL) {
        (void)snprintf(dst, sizeof dst, "%s/%s", STN_ESP_DIR,
                       STN_ESP_MEMTEST);
        rc = stn_mtools_put(td->d.path, off, signed_mt, dst);
    }
    if (rc != CLI_OK)
        return rc;
    if (p->no_shim)
        log_info("ESP (partition %u): \\EFI\\falcon\\ %s, %s, %s%s",
                 e->number, next, STN_ESP_CERT, STATIONINI_FILE,
                 memtest != NULL ? ", " STN_ESP_MEMTEST : "");
    else
        log_info("ESP (partition %u): \\EFI\\falcon\\ %s, %s, %s, %s, %s%s",
                 e->number, STN_ESP_SHIM, mm_name, next, STN_ESP_CERT,
                 STATIONINI_FILE, memtest != NULL ? ", " STN_ESP_MEMTEST : "");

    if (p->no_shim) {
        rc = stn_mtools_mkdir(td->d.path, off, STN_ESP_BOOT_DIR);
        if (rc == CLI_OK)
            rc = stn_mtools_put(td->d.path, off, signed_jump,
                                STN_ESP_BOOT_DIR "/" STN_ESP_JUMP);
        if (rc != CLI_OK)
            return rc;
        log_info("ESP (partition %u): \\EFI\\BOOT\\%s, the jumper to %s",
                 e->number, STN_ESP_JUMP, next);
    }

    memset(&rec, 0, sizeof rec);
    rec.version = IDENT_VERSION;
    rec.role = IDENT_ROLE_BOOT;
    memcpy(rec.install_id, id, IDENT_ID_LEN);
    return records_write_boot(td->d.path, off, &rec);
}

/* The plan's line for \EFI\BOOT\BOOTX64.EFI, before anything is written. */
static void jump_summary(const stn_disk *td, const stn_plan *p)
{
    const part_entry *e = &td->r.parts[p->esp];
    int present = 0;
    int ours = 0;

    if (!p->no_shim)
        return;
    if (stn_esp_jump_state(td->d.path, e->start_lba * td->d.sector,
                           (e->end_lba - e->start_lba + 1u) * td->d.sector,
                           &present, &ours) != CLI_OK) {
        cli_error_clear();
        return;
    }
    if (stn_jump_plan(0, present, ours) == STN_JUMP_TAKE)
        log_info("          \\EFI\\BOOT\\%s: the jumper, the other system's "
                 "saved beside the backup first", STN_ESP_JUMP);
    else
        log_info("          \\EFI\\BOOT\\%s: the jumper, which the entry "
                 "names", STN_ESP_JUMP);
}

/* Without shim the entry names the jumper and goes first in BootOrder
 * (stn_fw_jump_entry); with shim it names shimx64.efi. */
static cli_status firmware_entry(const stn_disk *td, const stn_plan *p,
                                 int *made)
{
    static stn_scan fw;
    char loader[64];
    char part[16];
    int ours = -1;
    cli_status rc;

    *made = -1;
    if (p->no_shim)
        (void)snprintf(loader, sizeof loader, "%s", STN_ESP_JUMP_PATH);
    else
        (void)snprintf(loader, sizeof loader, "\\EFI\\falcon\\%s",
                       STN_ESP_SHIM);
    if (!stn_is_machine(&td->d)) {
        log_info("firmware entry: none (an image; on its machine run "
                 "efibootmgr --create --disk <disk> --part %u --label %s "
                 "--loader '%s')",
                 td->r.parts[p->esp].number, STN_FW_LABEL, loader);
        return CLI_OK;
    }
    if (!util_is_dir("/sys/firmware/efi")) {
        log_info("firmware entry: none (this machine did not boot with "
                 "UEFI; the disk's machine gets it with efibootmgr)");
        return CLI_OK;
    }
    if (p->no_shim)
        return stn_fw_jump_entry(td->d.path, td->r.parts[p->esp].number,
                                 made);
    stn_scan_firmware(&fw, &ours);
    if (ours >= 0) {
        log_info("firmware entry: Boot%04X is already %s", (unsigned)ours,
                 STN_FW_LABEL);
        return CLI_OK;
    }
    (void)snprintf(part, sizeof part, "%u", td->r.parts[p->esp].number);
    rc = proc_runl_quiet("efibootmgr", "--create", "--disk", td->d.path,
                         "--part", part, "--label", STN_FW_LABEL, "--loader",
                         loader, PROC_END);
    if (rc != CLI_OK)
        return rc;
    stn_scan_firmware(&fw, &ours);
    *made = ours;
    log_info("firmware entry: Boot%04X %s", (unsigned)ours, STN_FW_LABEL);
    return CLI_OK;
}

static cli_status write_lba0(const stn_disk *td, const stn_plan *p,
                             const uint8_t *stage1)
{
    uint8_t lba0[BIOS_SECTOR_MAX];
    mks_dev dev;
    unsigned heads;
    unsigned spt;
    cli_status rc;

    dev.sector = td->d.sector;
    dev.fd = open(td->d.path, O_RDWR | O_CLOEXEC);
    if (dev.fd < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s", td->d.path);
    /* Read again: sgdisk rewrote the protective MBR on a GPT disk. */
    rc = dev_read(&dev, 0, 1, lba0);
    if (rc != CLI_OK)
        goto out;
    if (p->bios && td->r.table.id.kind == PART_KIND_MBR) {
        stn_mbr_geometry(lba0, &heads, &spt);
        if (stn_mbr_put(lba0, p->slot, STN_MBR_TYPE, p->region.start,
                        p->region.sectors, heads, spt) != STN_OK) {
            rc = set_error(CLI_ERR_INTERNAL, "the MBR entry did not fit");
            goto out;
        }
    }
    if (td->needs_sig)
        stn_mbr_set_sig(lba0, td->new_sig);
    if (p->bios &&
        bioscore_splice_mbr(lba0, stage1, STAGE1_S2_LBA_OFF,
                            STAGE1_S2_SECTORS_OFF, p->bplan.stage2_lba,
                            p->bplan.stage2_sectors, lba0) != BIOSCORE_OK) {
        rc = set_error(CLI_ERR_INVALID, "LBA 0 of %s does not end 0x55AA",
                       td->d.path);
        goto out;
    }
    if (!p->bios && !td->needs_sig)
        goto out;
    rc = dev_write(&dev, 0, lba0, dev.sector);
    if (rc == CLI_OK && fsync(dev.fd) != 0)
        rc = set_error_errno(CLI_ERR_IO, "flushing %s", td->d.path);
    if (rc == CLI_OK && p->bios)
        log_info("LBA 0: stage 1%s%s", td->r.table.id.kind == PART_KIND_MBR
                                           ? ", the station partition's "
                                             "entry" : "",
                 td->needs_sig ? ", a disk signature" : "");
out:
    (void)close(dev.fd);
    return rc;
}

static cli_status write_signature(const stn_disk *sd)
{
    uint8_t lba0[BIOS_SECTOR_MAX];
    mks_dev dev;
    cli_status rc;

    dev.sector = sd->d.sector;
    dev.fd = open(sd->d.path, O_RDWR | O_CLOEXEC);
    if (dev.fd < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s", sd->d.path);
    rc = dev_read(&dev, 0, 1, lba0);
    if (rc == CLI_OK) {
        stn_mbr_set_sig(lba0, sd->new_sig);
        rc = dev_write(&dev, 0, lba0, dev.sector);
    }
    if (rc == CLI_OK && fsync(dev.fd) != 0)
        rc = set_error_errno(CLI_ERR_IO, "flushing %s", sd->d.path);
    (void)close(dev.fd);
    if (rc == CLI_OK)
        log_info("%s: disk signature %08x", sd->d.path,
                 (unsigned)sd->new_sig);
    return rc;
}

/* The kernel's view of the new table: the whole table again when nothing
 * on the disk is in use, else the station partition alone (the running
 * machine's own disk). Neither is needed for the next boot. */
static void reread_table(const stn_disk *td, const stn_plan *p)
{
    char nr[16];
    int fd = open(td->d.path, O_RDONLY | O_CLOEXEC);
    int done = 0;

    if (fd >= 0) {
        done = (ioctl(fd, BLKRRPART) == 0);
        (void)close(fd);
    }
    if (done || !p->bios || !proc_exists("partx"))
        return;
    (void)snprintf(nr, sizeof nr, "%u", p->slot);
    if (proc_runl_quiet("partx", "-a", "--nr", nr, td->d.path, PROC_END) !=
        CLI_OK) {
        log_info("note:     the kernel lists partition %u after the next "
                 "boot", p->slot);
        cli_error_clear();
    }
}

/* ---- the command ---- */

static void print_ini(const char *where, const char *text, size_t len)
{
    const char *p = text;
    const char *end = text + len;

    log_info("%s:", where);
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = (nl != NULL) ? (size_t)(nl - p) : (size_t)(end - p);

        if (n > 0u && p[n - 1u] == '\r')
            n--;
        log_info("    %.*s", (int)n, p);
        p = (nl != NULL) ? nl + 1 : end;
    }
}

static void reach_warn(const char *what, uint64_t end)
{
    switch (stn_reach_of(end)) {
    case STN_REACH_PAST_LBA28:
        log_warn("%s ends past 128 GiB: a BIOS with 28-bit LBA does not "
                 "reach it", what);
        break;
    case STN_REACH_PAST_CHS:
        log_warn("%s ends past 8 GB: a BIOS without INT 13h extensions "
                 "does not reach it", what);
        break;
    case STN_REACH_OK:
        break;
    }
}

static cli_status install_run(const cli_args *a)
{
    static stn_scan s;
    static stn_plan p;
    static stn_backup bk;
    const char *also[CLI_MAX_VALUES] = { NULL };
    size_t n_also = cli_count(a, "also");
    const char *p1 = cli_value(a, "stage1", DEFAULT_STAGE1);
    const char *p2 = cli_value(a, "stage2", DEFAULT_STAGE2);
    const char *p3 = cli_value(a, "stage3", cli_flag(a, "legacy")
                                                ? DEFAULT_LEGACY_STAGE3
                                                : DEFAULT_STAGE3);
    const char *backup = cli_value(a, "backup", NULL);
    const char *size_text = cli_value(a, "size", NULL);
    const char *key = cli_value(a, "key", NULL);
    const char *cert = cli_value(a, "cert", NULL);
    int dry = cli_flag(a, "dry-run");
    int ask_bios = cli_flag(a, "bios");
    int ask_uefi = cli_flag(a, "uefi");
    unsigned long size_mib = 0;
    uint64_t want;
    char derived[MKS_PATH_MAX];
    char tmpdir[] = "/tmp/falcon-station.XXXXXX";
    char keydir[MKS_PATH_MAX];
    int made_tmp = 0;
    void *b1 = NULL, *b2 = NULL, *b3 = NULL;
    size_t l1 = 0, l2 = 0, l3 = 0;
    uint8_t install_id[IDENT_ID_LEN];
    char idhex[33];
    shelf_ref shelf;
    mks_pass pass;
    mok_keys keys;
    stn_disk *td;
    int fw_made = -1;
    size_t i;
    cli_status rc;

    memset(&p, 0, sizeof p);
    memset(&shelf, 0, sizeof shelf);
    memset(&pass, 0, sizeof pass);
    memset(&keys, 0, sizeof keys);
    p.esp = -1;
    (void)setenv("MTOOLS_SKIP_CHECK", "1", 1);

    rc = mkstick_guid_selfcheck();
    if (rc != CLI_OK)
        return rc;
    if (size_text != NULL) {
        char *end = NULL;

        size_mib = strtoul(size_text, &end, 10);
        if (end == size_text || *end != '\0' || size_mib == 0u ||
            size_mib > STN_MAX_MIB)
            return set_error(CLI_ERR_USAGE, "--size %s: a number of MiB up "
                             "to %u", size_text, STN_MAX_MIB);
    }
    if ((key == NULL) != (cert == NULL))
        return set_error(CLI_ERR_USAGE, "--key and --cert go together");
    if (cli_count(a, "uki") > 0u && !cli_flag(a, "legacy"))
        return set_error(CLI_ERR_USAGE, "--uki is station-legacy's (--legacy):"
                         " the station boots UKIs off the shelf");
    for (i = 0; i < cli_count(a, "uki"); i++) {
        if (!util_is_file(cli_at(a, "uki", i)))
            return set_error(CLI_ERR_NOTFOUND, "no UKI at %s",
                             cli_at(a, "uki", i));
    }

    for (i = 0; i < n_also; i++)
        also[i] = cli_at(a, "also", i);
    rc = stn_scan_disks(&s, a->pos[0], also, n_also, cli_flag(a, "no-scan"));
    if (rc != CLI_OK)
        return rc;
    td = &s.disks[0];
    disk_describe(&td->d);
    if (stn_min_mib(td->d.sector) == 0u)
        return set_error(CLI_ERR_INVALID, "%s has %u-byte sectors; the "
                         "installer writes disks of 512- and 4096-byte "
                         "sectors", td->d.path, td->d.sector);
    /* The station partition's FAT32 needs 65525 clusters of one sector
     * each, which is eight times the bytes on a 4Kn disk. */
    if (size_mib == 0u)
        size_mib = stn_default_mib(td->d.sector);
    if (size_mib < stn_min_mib(td->d.sector))
        return set_error(CLI_ERR_USAGE, "--size %s: at least %u MiB on a "
                         "disk of %u-byte sectors (FAT32's 65525 clusters)",
                         size_text, stn_min_mib(td->d.sector),
                         td->d.sector);
    want = (uint64_t)size_mib * (1024u * 1024u / td->d.sector);
    if (td->r.rc != PART_OK || td->r.table.count == 0u)
        return set_error(CLI_ERR_INVALID, "%s has no partition table; the "
                         "station goes onto a machine's disk beside its "
                         "systems", td->d.path);
    if (records_bios_boot(&td->r) >= 0 || records_boot(&td->r) >= 0) {
        int b = records_boot(&td->r);

        records_id_hex(td->r.recs[b].rec.install_id, idhex);
        return set_error(CLI_ERR_EXISTS, "partition %u of %s already carries "
                         "falcon install %s; `station remove` it first (and "
                         "delete the station partition a remove leaves)",
                         td->r.parts[b].number, td->d.path, idhex);
    }

    /* ---- which firmware ---- */
    {
        const char *esp_text = cli_value(a, "esp", NULL);

        if (esp_text != NULL) {
            char *end = NULL;
            unsigned long n = strtoul(esp_text, &end, 10);

            p.esp = (end != esp_text && *end == '\0')
                        ? stn_part_index(&td->r, (uint32_t)n) : -1;
            if (p.esp < 0)
                return set_error(CLI_ERR_NOTFOUND, "--esp %s: %s has no "
                                 "such partition", esp_text, td->d.path);
        } else {
            p.esp = stn_find_esp(&td->r);
        }
    }
    if (ask_uefi && p.esp < 0)
        return set_error(CLI_ERR_NOTFOUND, "%s has no ESP for the UEFI "
                         "station (--esp names one)", td->d.path);
    p.uefi = ask_uefi || (!ask_bios && p.esp >= 0);
    p.bios = ask_bios || !ask_uefi;
    p.no_shim = cli_value(a, "shim", NULL) == NULL;
    if (!p.no_shim && !p.uefi)
        return set_error(CLI_ERR_USAGE, "--shim is for the UEFI station, "
                         "and %s is not getting one", td->d.path);

    /* ---- BIOS: the stages and the plan ---- */
    if (p.bios) {
        rc = util_read_file(p1, &b1, &l1, STAGE_MAX);
        if (rc == CLI_OK)
            rc = util_read_file(p2, &b2, &l2, STAGE_MAX);
        if (rc == CLI_OK)
            rc = util_read_file(p3, &b3, &l3, STAGE_MAX);
        if (rc != CLI_OK)
            goto out;
        if (bioscore_check_stage1((const uint8_t *)b1, l1) != BIOSCORE_OK) {
            rc = set_error(CLI_ERR_INVALID, "%s is not a stage 1", p1);
            goto out;
        }
        rc = plan_bios(td, want, l2, l3, &p, ask_bios);
        if (rc != CLI_OK && !ask_bios && p.uefi) {
            log_info("BIOS:     not installed (%s)", cli_error_msg());
            cli_error_clear();
            p.bios = 0;
            rc = CLI_OK;
        }
        if (rc != CLI_OK)
            goto out;
    }

    /* ---- UEFI: the ESP ---- */
    if (p.uefi) {
        uint8_t bs[512];
        disk_io io;
        char where[MKS_PATH_MAX];

        if (stn_part_mounted(&td->d, td->r.parts[p.esp].number, where,
                             sizeof where)) {
            rc = set_error(CLI_ERR_BUSY, "the ESP (partition %u) is mounted "
                           "at %s; unmount it first",
                           td->r.parts[p.esp].number, where);
            goto out;
        }
        rc = disk_io_open(&io, td->d.path,
                          td->r.parts[p.esp].start_lba * td->d.sector,
                          512u);
        if (rc != CLI_OK)
            goto out;
        if (disk_io_read(&io, 0, sizeof bs, bs) != 0 ||
            bioscore_fat_bpb(bs, &p.esp_fat) != BIOSCORE_OK) {
            disk_io_close(&io);
            rc = set_error(CLI_ERR_INVALID, "partition %u of %s is not a FAT "
                           "ESP", td->r.parts[p.esp].number, td->d.path);
            goto out;
        }
        disk_io_close(&io);
    }

    /* ---- the shelf ---- */
    if (cli_value(a, "shelf", NULL) != NULL) {
        rc = shelf_resolve(cli_value(a, "shelf", NULL), &s, &shelf);
        if (rc != CLI_OK)
            goto out;
        shelf_probe(&shelf);
        log_info("shelf:    %s%s%s, %s", shelf.disk,
                 shelf.number != 0u ? " partition " : "",
                 shelf.number != 0u ? cli_value(a, "shelf", "") : "",
                 shelf.luks ? "LUKS2" : "plaintext");
        reach_warn("the shelf", shelf.off + shelf.len);
    } else {
        size_t k;
        uint32_t j;

        for (k = 0; k < s.n_disks; k++) {
            const mks_records *r = &s.disks[k].r;

            for (j = 0; j < r->table.count; j++) {
                if ((r->recs[j].state == REC_OK &&
                     r->recs[j].rec.role == IDENT_ROLE_SHELF) ||
                    (r->table.id.kind == PART_KIND_GPT &&
                     gpt_guid_eq(r->parts[j].type_guid, falcon_guid_shelf)))
                    log_info("note:     %s partition %u looks like a shelf; "
                             "--shelf %s:%u makes it this station's",
                             s.disks[k].d.path, r->parts[j].number,
                             s.disks[k].d.path, r->parts[j].number);
            }
        }
        log_info("shelf:    none (no --shelf): the station offers no Linux");
    }

    /* The passphrase, when something behind it is needed: the MOK pair for
     * UEFI, or a LUKS2 shelf's UKIs when a file gives it. */
    if (shelf.set && shelf.luks &&
        ((p.uefi && key == NULL) ||
         cli_value(a, "passphrase-file", NULL) != NULL)) {
        rc = pass_acquire(&pass, cli_value(a, "passphrase-file", NULL));
        if (rc != CLI_OK)
            goto out;
    }
    if (mkdtemp(tmpdir) == NULL) {
        rc = set_error_errno(CLI_ERR_IO, "creating a private work directory");
        goto out;
    }
    made_tmp = 1;
    rc = util_join(keydir, sizeof keydir, tmpdir, "keys");
    if (rc == CLI_OK)
        rc = util_mkdir_p(keydir, 0700);
    if (rc == CLI_OK)
        rc = cleanup_push_shreddir(keydir);
    if (rc != CLI_OK)
        goto out;
    if (p.uefi && key == NULL && !shelf.set) {
        rc = set_error(CLI_ERR_USAGE, "the UEFI station's loader is signed "
                       "with the MOK: --key and --cert, or --shelf to read "
                       "the shelf's");
        goto out;
    }
    if (shelf.set && (!shelf.luks || pass.len > 0u)) {
        rc = shelf_read(&shelf, &pass, &s,
                        (p.uefi && key == NULL) ? keydir : NULL);
        if (rc != CLI_OK)
            goto out;
    } else if (shelf.set) {
        log_info("note:     the shelf is locked; its UKIs are rows at boot "
                 "(--passphrase-file lets the INI name one as default)");
    }
    if (p.uefi) {
        rc = mok_paths(&keys, keydir);
        if (rc == CLI_OK && key != NULL) {
            (void)snprintf(keys.key, sizeof keys.key, "%s", key);
            (void)snprintf(keys.crt, sizeof keys.crt, "%s", cert);
        }
        if (rc == CLI_OK)
            rc = proc_runl_quiet("openssl", "x509", "-in", keys.crt,
                                 "-outform", "DER", "-out", keys.cer,
                                 PROC_END);
        if (rc != CLI_OK)
            goto out;
    }

    /* ---- what the menus will offer ---- */
    {
        int ours = -1;

        stn_scan_found(&s, p.uefi);
        /* The firmware's entries are this machine's; an image's machine
         * is not this one. */
        if (p.uefi && stn_is_machine(&td->d))
            stn_scan_firmware(&s, &ours);
        if (ours >= 0)
            log_info("note:     firmware entry Boot%04X is already %s",
                     (unsigned)ours, STN_FW_LABEL);
    }
    rc = assign_signatures(&s, !p.bios);
    if (rc != CLI_OK)
        goto out;
    for (i = 0; i < s.n_found; i++)
        log_info("found:    %-22s %s", s.found[i].name[0] != '\0'
                                           ? s.found[i].name : "-",
                 s.found[i].label);
    if (p.bios) {
        rc = make_ini(s.found, s.n_found, 0, p.ini_bios, sizeof p.ini_bios,
                      &p.ini_bios_len);
        if (rc != CLI_OK)
            goto out;
    }
    if (p.uefi) {
        rc = make_ini(s.found, s.n_found, 1, p.ini_uefi, sizeof p.ini_uefi,
                      &p.ini_uefi_len);
        if (rc != CLI_OK)
            goto out;
    }

    /* ---- the plan, said ---- */
    rc = records_new_id(install_id);
    if (rc != CLI_OK)
        goto out;
    records_id_hex(install_id, idhex);
    cli_log_blank();
    log_info("install ID: %s", idhex);
    if (p.bios) {
        char size[32];

        log_info("BIOS:     %s station partition, %s partition %u at LBA "
                 "%llu..%llu (%s), FAT32 with %u reserved sectors",
                 cli_flag(a, "legacy") ? "station-legacy," : "the",
                 td->r.table.id.kind == PART_KIND_MBR ? "primary"
                                                      : "GPT",
                 p.slot, (unsigned long long)p.region.start,
                 (unsigned long long)(p.region.start + p.region.sectors - 1u),
                 util_format_size(p.region.sectors * td->d.sector, size,
                                  sizeof size),
                 (unsigned)p.reserved);
        if (p.make_ef02)
            log_info("          %s partition %u at LBA %llu..%llu",
                     BIOS_BOOT_NAME, p.ef02_slot,
                     (unsigned long long)p.ef02_start,
                     (unsigned long long)p.ef02_end);
        log_info("          stage 1 in LBA 0, stage 2 at LBA %llu (%llu "
                 "sectors), stage 3 at LBA %llu (%llu sectors), marker at "
                 "LBA %llu", (unsigned long long)p.bplan.stage2_lba,
                 (unsigned long long)p.bplan.stage2_sectors,
                 (unsigned long long)p.bplan.stage3_lba,
                 (unsigned long long)p.bplan.stage3_sectors,
                 (unsigned long long)p.bplan.marker_lba);
        reach_warn("the station partition",
                   (p.region.start + p.region.sectors) * td->d.sector);
        {
            char hdr[64];

            (void)snprintf(hdr, sizeof hdr, "/%s", STATIONINI_FILE);
            print_ini(hdr, p.ini_bios, p.ini_bios_len);
        }
    }
    if (p.uefi) {
        log_info("UEFI:     \\EFI\\falcon\\ on the ESP (partition %u)%s",
                 td->r.parts[p.esp].number,
                 p.no_shim ? ", no shim: the loader is " STN_LOADER_NOSHIM
                             " and the entry names the jumper"
                           : "");
        jump_summary(td, &p);
        print_ini("\\EFI\\falcon\\" STATIONINI_FILE, p.ini_uefi,
                  p.ini_uefi_len);
    }
    for (i = 1; i < s.n_disks; i++) {
        if (s.disks[i].needs_sig)
            log_info("signature: %s gets %08x", s.disks[i].d.path,
                     (unsigned)s.disks[i].new_sig);
    }
    if (td->needs_sig)
        log_info("signature: %s gets %08x", td->d.path,
                 (unsigned)td->new_sig);

    if (dry) {
        log_info("--dry-run: nothing written");
        goto out;
    }
    if (td->d.is_block && geteuid() != 0) {
        rc = set_error(CLI_ERR_PERM, "writing %s needs root", td->d.path);
        goto out;
    }
    {
        static const char *const tools[] = {
            "mkfs.fat", "mcopy", "mmd", "mdir", "mdeltree"
        };

        for (i = 0; i < sizeof tools / sizeof tools[0]; i++) {
            if (!proc_exists(tools[i])) {
                rc = set_error(CLI_ERR_NOTFOUND, "%s is not on PATH",
                               tools[i]);
                goto out;
            }
        }
        if ((p.bios && td->r.table.id.kind == PART_KIND_GPT &&
             !proc_exists("sgdisk")) ||
            (p.uefi && (!proc_exists("sbsign") || !proc_exists("openssl")))) {
            rc = set_error(CLI_ERR_NOTFOUND, "sgdisk, sbsign or openssl is "
                           "not on PATH");
            goto out;
        }
    }

    /* ---- the backup, before anything ---- */
    if (backup == NULL) {
        (void)snprintf(derived, sizeof derived,
                       "falcon-station-backup-%s.bin",
                       util_basename(td->d.path));
        backup = derived;
    }
    memset(&bk, 0, sizeof bk);
    bk.sector = td->d.sector;
    bk.fw_entry = 0xFFFFFFFFu;
    memcpy(bk.install_id, install_id, IDENT_ID_LEN);
    (void)stn_backup_disk(&bk, td->d.path);
    rc = backup_add(&bk, td->d.path, STN_RUN_LBA0, 0, 1);
    if (rc == CLI_OK && p.bios) {
        uint64_t first = p.first_real != 0u ? p.first_real : p.region.start;
        uint64_t gap_lo = 1u;
        uint64_t disk_sectors = td->d.size / td->d.sector;

        bk.flags |= STN_FLAG_BIOS | STN_FLAG_MADE_PART;
        bk.station_part = p.slot;
        if (td->r.table.id.kind == PART_KIND_GPT) {
            gap_lo = td->r.table.first_usable;
            rc = backup_add(&bk, td->d.path, STN_RUN_GPT, 1, gap_lo - 1u);
            if (rc == CLI_OK)
                rc = backup_add(&bk, td->d.path, STN_RUN_GPT_BACKUP,
                                td->r.table.last_usable + 1u,
                                disk_sectors - td->r.table.last_usable - 1u);
            if (p.make_ef02) {
                bk.flags |= STN_FLAG_MADE_EF02;
                bk.ef02_part = p.ef02_slot;
            }
        }
        if (rc == CLI_OK && first > gap_lo)
            rc = backup_add(&bk, td->d.path, STN_RUN_GAP, gap_lo,
                            first - gap_lo > STN_BK_RUN_MAX
                                ? p.bplan.stage2_lba + p.bplan.stage2_sectors
                                      - gap_lo
                                : first - gap_lo);
        if (rc == CLI_OK) {
            uint64_t n = p.reserved + STN_REGION_BACKUP_BYTES / td->d.sector;

            rc = backup_add(&bk, td->d.path, STN_RUN_REGION, p.region.start,
                            p.region.sectors < n ? p.region.sectors : n);
        }
    }
    if (rc == CLI_OK && p.uefi) {
        bk.flags |= STN_FLAG_UEFI;
        bk.esp_part = td->r.parts[p.esp].number;
        rc = backup_add(&bk, td->d.path, STN_RUN_ESP_BOOT,
                        td->r.parts[p.esp].start_lba, 1);
        if (rc == CLI_OK && p.esp_fat.backup != 0u)
            rc = backup_add(&bk, td->d.path, STN_RUN_ESP_BOOT,
                            td->r.parts[p.esp].start_lba + p.esp_fat.backup,
                            1);
    }
    for (i = 1; i < s.n_disks && rc == CLI_OK; i++) {
        if (s.disks[i].needs_sig)
            rc = backup_add(&bk, s.disks[i].d.path, STN_RUN_SIG, 0, 1);
    }
    if (rc == CLI_OK)
        rc = backup_write(&bk, backup, cli_flag(a, "force"));
    if (rc != CLI_OK)
        goto out;
    log_info("backup:   %s (%u runs)", backup, (unsigned)bk.n_runs);
    if (p.uefi && p.no_shim) {
        rc = save_foreign_jump(td, &p, backup);
        if (rc != CLI_OK)
            goto out;
    }

    /* ---- writes: the shelf, BIOS, UEFI, other disks, LBA 0 last ---- */
    if (shelf.set) {
        rc = shelf_stamp(&shelf, install_id);
        if (rc != CLI_OK)
            goto out;
    }
    if (p.bios) {
        rc = write_bios(a, td, &p, (const uint8_t *)b2, l2,
                        (const uint8_t *)b3, l3, install_id);
        if (rc != CLI_OK)
            goto out;
    }
    if (p.uefi) {
        rc = write_uefi(td, &p, &keys, tmpdir, install_id, a);
        if (rc == CLI_OK)
            rc = firmware_entry(td, &p, &fw_made);
        if (rc == CLI_OK && fw_made >= 0) {
            bk.flags |= STN_FLAG_FW_ENTRY;
            bk.fw_entry = (uint32_t)fw_made;
            rc = backup_rewrite_header(&bk, backup);
        }
        if (rc != CLI_OK)
            goto out;
    }
    for (i = 1; i < s.n_disks && rc == CLI_OK; i++) {
        if (s.disks[i].needs_sig)
            rc = write_signature(&s.disks[i]);
    }
    if (rc == CLI_OK)
        rc = write_lba0(td, &p, (const uint8_t *)b1);
    if (rc != CLI_OK)
        goto out;
    if (td->d.is_block)
        reread_table(td, &p);

    cli_log_blank();
    log_info("%s boots the station (install ID %s).", td->d.path, idhex);
    log_info("`falcon-boot-mkstick station remove --backup %s` undoes it.",
             backup);
    if (p.uefi && p.no_shim)
        log_info("Secure Boot on: the firmware's db must hold "
                 "\\EFI\\falcon\\%s; enroll it from firmware setup.",
                 STN_ESP_CERT);
    else if (p.uefi)
        log_info("Secure Boot on: enroll \\EFI\\falcon\\%s with MokManager "
                 "on the first boot (mokutil --import).", STN_ESP_CERT);

out:
    pass_wipe(&pass);
    while (cleanup_depth() > 0u) {
        cli_status crc = cleanup_pop();

        if (crc != CLI_OK && rc == CLI_OK)
            rc = crc;
    }
    if (made_tmp) {
        char w[MKS_PATH_MAX];
        size_t k;
        static const char *const leftovers[] = {
            "grubx64.efi", "grub.efi", STN_LOADER_NOSHIM, STN_ESP_MEMTEST,
            STN_ESP_JUMP
        };

        for (k = 0; k < sizeof leftovers / sizeof leftovers[0]; k++) {
            if (util_join(w, sizeof w, tmpdir, leftovers[k]) == CLI_OK)
                (void)unlink(w);
        }
        (void)rmdir(tmpdir);
    }
    free(b1);
    free(b2);
    free(b3);
    return rc;
}

const cli_cmd stn_cmd_install = {
    "install",
    "put the station on a machine's disk, in space that is already free",
    "<disk>",
    install_opts,
    sizeof install_opts / sizeof install_opts[0],
    1, 1,
    "BIOS: a station partition in free space (MBR: a primary, type 0x1C,\n"
    "hidden FAT32; GPT: the falcon boot type, and a FALCON-BIOS partition\n"
    "at LBA 34 for stage 2 when there is none), with the boot record, stage\n"
    "3 and the marker in its reserved sectors and STATION.INI (MEMTEST.BIN,\n"
    "and on --legacy the drivers and /uki) as files; stage 2 before the\n"
    "first partition; stage 1 into LBA 0's boot code. Nothing is resized:\n"
    "without the free space --size needs it refuses and says how much.\n"
    "\n"
    "UEFI: \\EFI\\falcon\\ on the ESP (the loader signed with the MOK as\n"
    "falconx64.efi, the cert, STATION.INI, memtest.efi), the jumper signed\n"
    "with the MOK as \\EFI\\BOOT\\BOOTX64.EFI, the ESP's boot record, and a\n"
    "firmware entry named falcon, first in BootOrder, when the disk is this\n"
    "machine's. The MOK pair is --key/--cert or the shelf's. The entry names\n"
    "the jumper, which starts the loader; with no entry the firmware's\n"
    "removable-media path is the jumper too. The firmware's db must hold the\n"
    "cert. Another system's BOOTX64.EFI is saved beside --backup first.\n"
    "--shim <dir> puts shim and MokManager from dir before the loader, for a\n"
    "firmware whose db is not yours: the entry names shim, the cert is\n"
    "enrolled with MokManager, and there is no jumper.\n"
    "\n"
    "Both by default, where the disk allows. STATION.INI is written from\n"
    "what is on the machine's disks: the Windows or DOS found is the default\n"
    "(else a UKI on the shelf), with a five-second countdown. A disk it names\n"
    "with no signature gets one. The shelf (--shelf) is stamped with the\n"
    "station's install ID.\n"
    "\n"
    "Everything it changes is saved to --backup first, LBA 0 is written\n"
    "last, and `station remove --backup` undoes it.\n",
    install_run
};
