/* falcon-boot mkstick: `station`, installing falcon as a machine's own boot
 * loader (docs/DESIGN.md Section 13.9). See station.h for the split.
 *
 * `station install` is stationinst.c. Here: the dispatch, the scan of the
 * machine's disks that install and status both run, the FAT reads and
 * mtools writes, `remove` and `status`.
 *
 * The scan is the installer's half of DESIGN Section 13.3: the loaders run
 * the same rules at boot. Both read the partition tables with falcon-core's
 * reader and a boot sector's names with the same rule, so what `status`
 * lists is what the menu shows; where they cannot agree (the UEFI loader
 * reads the firmware's entries, which a disk image has none of) status says
 * which half it could not see.
 */

#include <mkstick/station.h>

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include <common/log.h>
#include <common/proc.h>
#include <common/util.h>
#include <mkstick/bioscore.h>
#include <mkstick/mkstick.h>

#include <fs/fs.h>
#include <ident/ident.h>
#include <media/nextboot.h>
#include <media/shelf.h>
#include <media/stationini.h>
#include <media/strows.h>
#include <media/vbr.h>
#include <part/part.h>
#include <part/table.h>

#include <falcon-bios/src/start/biosdefs.h>

/* ---- partitions ---- */

int stn_is_machine(const mks_disk *d)
{
    return d->is_block && !d->is_loop;
}

int stn_part_index(const mks_records *r, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < r->table.count; i++) {
        if (r->parts[i].number == n)
            return (int)i;
    }
    return -1;
}

int stn_bios_boot(const mks_records *r)
{
    uint32_t i;

    for (i = 0; i < r->table.count; i++) {
        if (records_boot_candidate(&r->table, &r->parts[i]) &&
            r->recs[i].state == REC_OK &&
            r->recs[i].rec.role == IDENT_ROLE_BOOT &&
            r->recs[i].rec.stage3_sector != 0u)
            return (int)i;
    }
    return -1;
}

int stn_find_esp(const mks_records *r)
{
    uint32_t i;

    for (i = 0; i < r->table.count; i++) {
        const part_entry *p = &r->parts[i];

        if (r->table.id.kind == PART_KIND_GPT
                ? gpt_guid_eq(p->type_guid, gpt_guid_esp)
                : (p->mbr_type == 0xEFu))
            return (int)i;
    }
    return -1;
}

int stn_part_mounted(const mks_disk *d, uint32_t number, char *where,
                     size_t cap)
{
    char real[MKS_PATH_MAX];
    char node[MKS_PATH_MAX];
    char line[8192];
    FILE *f;
    int hit = 0;

    if (!d->is_block)
        return 0;
    if (realpath(d->path, real) == NULL)
        (void)snprintf(real, sizeof real, "%s", d->path);
    if (disk_partition_name(real, number, node, sizeof node) != CLI_OK) {
        cli_error_clear();
        return 0;
    }
    f = fopen("/proc/mounts", "re");
    if (f == NULL)
        return 0;
    while (fgets(line, sizeof line, f) != NULL) {
        char src[MKS_PATH_MAX];
        char dst[MKS_PATH_MAX];
        char rsrc[MKS_PATH_MAX];

        if (sscanf(line, "%4095s %4095s", src, dst) != 2)
            continue;
        if (realpath(src, rsrc) == NULL)
            (void)snprintf(rsrc, sizeof rsrc, "%s", src);
        if (strcmp(rsrc, node) == 0) {
            if (where != NULL)
                (void)snprintf(where, cap, "%s", dst);
            hit = 1;
            break;
        }
    }
    (void)fclose(f);
    return hit;
}

/* ---- files over core's drivers ---- */

int stn_read_file(const char *disk, uint64_t off, uint64_t len,
                  const char *path, char *buf, size_t cap, size_t *got)
{
    static fs_vol vol;
    static fs_dir scratch;
    static fs_file file;
    fs_dirent ent;
    const fs_ops *ops;
    uint64_t size = 0;
    disk_io io;
    int rc;

    *got = 0;
    if (disk_io_open(&io, disk, off, len) != CLI_OK) {
        cli_error_clear();
        return FS_ERR_IO;
    }
    ops = fs_identify(disk_io_read, &io, FS_HINT_NONE);
    if (ops == NULL) {
        disk_io_close(&io);
        return FS_ERR_BAD_SUPER;
    }
    rc = fs_mount(&vol, ops, disk_io_read, &io);
    if (rc == FS_OK)
        rc = fs_lookup(&vol, path, &scratch, &ent);
    if (rc == FS_OK && ent.is_dir)
        rc = FS_ERR_RANGE;
    if (rc == FS_OK)
        rc = fs_file_open(&vol, &file, &ent);
    /* The size is the open file's: a directory entry need not carry it. */
    if (rc == FS_OK) {
        size = fs_file_size(&file);
        if (size >= cap)
            rc = FS_ERR_RANGE;
    }
    if (rc == FS_OK && size > 0u)
        rc = fs_file_read(&file, 0, size, buf);
    if (rc == FS_OK) {
        buf[size] = '\0';
        *got = (size_t)size;
    }
    disk_io_close(&io);
    return rc;
}

/* ---- mtools ---- */

static void mtools_image(char *out, size_t cap, const char *disk,
                         uint64_t off)
{
    (void)snprintf(out, cap, "%s@@%llu", disk, (unsigned long long)off);
}

static int mtools_exists(const char *img, const char *path)
{
    char out[4096];
    size_t len = 0;
    cli_status rc;

    rc = proc_capture_l(out, sizeof out, &len, "mdir", "-b", "-i", img,
                        path, PROC_END);
    if (rc != CLI_OK && rc != CLI_ERR_RANGE) {
        cli_error_clear();
        return 0;
    }
    cli_error_clear();
    return 1;
}

cli_status stn_mtools_mkdir(const char *disk, uint64_t off, const char *dir)
{
    char img[MKS_PATH_MAX + 32];
    char path[MKS_PATH_MAX];
    size_t i;
    cli_status rc;

    mtools_image(img, sizeof img, disk, off);
    /* One component at a time: mmd has no -p. */
    for (i = 0; dir[i] != '\0'; i++) {
        if (dir[i + 1] != '/' && dir[i + 1] != '\0')
            continue;
        (void)snprintf(path, sizeof path, "::/%.*s", (int)(i + 1u), dir);
        if (mtools_exists(img, path))
            continue;
        rc = proc_runl_quiet("mmd", "-i", img, path, PROC_END);
        if (rc != CLI_OK)
            return rc;
    }
    return CLI_OK;
}

cli_status stn_mtools_put(const char *disk, uint64_t off, const char *src,
                          const char *dst)
{
    char img[MKS_PATH_MAX + 32];
    char path[MKS_PATH_MAX];

    mtools_image(img, sizeof img, disk, off);
    (void)snprintf(path, sizeof path, "::/%s", dst);
    return proc_runl_quiet("mcopy", "-o", "-i", img, src, path, PROC_END);
}

cli_status stn_mtools_put_text(const char *disk, uint64_t off,
                               const char *text, size_t len, const char *dst)
{
    char tmp[] = "/tmp/falcon-station.XXXXXX";
    cli_status rc;
    int fd = mkstemp(tmp);

    if (fd < 0)
        return set_error_errno(CLI_ERR_IO, "creating a temporary file");
    if (write(fd, text, len) != (ssize_t)len) {
        (void)close(fd);
        (void)unlink(tmp);
        return set_error_errno(CLI_ERR_IO, "writing %s", tmp);
    }
    (void)close(fd);
    rc = stn_mtools_put(disk, off, tmp, dst);
    (void)unlink(tmp);
    return rc;
}

cli_status stn_mtools_deltree(const char *disk, uint64_t off,
                              const char *dir)
{
    char img[MKS_PATH_MAX + 32];
    char path[MKS_PATH_MAX];

    mtools_image(img, sizeof img, disk, off);
    (void)snprintf(path, sizeof path, "::/%s", dir);
    if (!mtools_exists(img, path))
        return CLI_OK;
    return proc_runl_quiet("mdeltree", "-i", img, path, PROC_END);
}

/* ---- the scan ---- */

static cli_status read_disk(stn_disk *sd, const char *path)
{
    disk_io io;
    cli_status rc;

    memset(sd, 0, sizeof *sd);
    rc = disk_stat(&sd->d, path);
    if (rc != CLI_OK)
        return rc;
    rc = records_read(&sd->r, sd->d.path, sd->d.sector, sd->d.size);
    if (rc != CLI_OK)
        return rc;
    rc = disk_io_open(&io, sd->d.path, 0, sd->d.size);
    if (rc != CLI_OK)
        return rc;
    if (disk_io_read(&io, 0, sizeof sd->lba0, sd->lba0) != 0) {
        disk_io_close(&io);
        return set_error(CLI_ERR_IO, "reading LBA 0 of %s", sd->d.path);
    }
    disk_io_close(&io);
    return CLI_OK;
}

static int same_file(const char *a, const char *b)
{
    char ra[MKS_PATH_MAX];
    char rb[MKS_PATH_MAX];

    if (realpath(a, ra) == NULL || realpath(b, rb) == NULL)
        return strcmp(a, b) == 0;
    return strcmp(ra, rb) == 0;
}

static void add_disk(stn_scan *s, const char *path)
{
    size_t i;

    if (s->n_disks >= STN_MAX_DISKS)
        return;
    for (i = 0; i < s->n_disks; i++) {
        if (same_file(s->disks[i].d.path, path))
            return;
    }
    if (read_disk(&s->disks[s->n_disks], path) != CLI_OK) {
        log_debug("not scanning %s: %s", path, cli_error_msg());
        cli_error_clear();
        return;
    }
    s->n_disks++;
}

cli_status stn_scan_disks(stn_scan *s, const char *target,
                          const char *const *also, size_t n_also,
                          int only_named)
{
    static const char *const skip_prefix[] = {
        "loop", "ram", "zram", "sr", "fd", "dm-", "md", "nbd"
    };
    size_t i;
    cli_status rc;

    memset(s, 0, sizeof *s);
    s->shelf_disk = -1;
    s->shelf_part = -1;
    rc = read_disk(&s->disks[0], target);
    if (rc != CLI_OK)
        return rc;
    s->disks[0].is_target = 1;
    s->n_disks = 1;

    /* A disk on this machine is a disk the station's machine has; an
     * image's machine is what the command line names. */
    if (stn_is_machine(&s->disks[0].d) && !only_named) {
        DIR *dir = opendir("/sys/block");
        struct dirent *e;

        while (dir != NULL && (e = readdir(dir)) != NULL) {
            char path[MKS_PATH_MAX];
            int skip = (e->d_name[0] == '.');

            for (i = 0; i < sizeof skip_prefix / sizeof skip_prefix[0]; i++) {
                if (strncmp(e->d_name, skip_prefix[i],
                            strlen(skip_prefix[i])) == 0)
                    skip = 1;
            }
            if (skip)
                continue;
            (void)snprintf(path, sizeof path, "/dev/%s", e->d_name);
            add_disk(s, path);
        }
        if (dir != NULL)
            (void)closedir(dir);
    }
    for (i = 0; i < n_also; i++)
        add_disk(s, also[i]);
    return CLI_OK;
}

static stn_found *new_found(stn_scan *s)
{
    stn_found *f;

    if (s->n_found >= STN_MAX_FOUND)
        return NULL;
    f = &s->found[s->n_found++];
    memset(f, 0, sizeof *f);
    return f;
}

/* vbr_identify's reader: the partition's 512-byte sector `sector`. */
static int read_part_sector(void *ctx, uint32_t sector, uint8_t *buf)
{
    return disk_io_read((disk_io *)ctx, (uint64_t)sector * 512u, 512, buf);
}

void stn_scan_found(stn_scan *s, int uefi)
{
    static char probe[512];
    size_t k;

    for (k = 0; k < s->n_disks; k++) {
        stn_disk *sd = &s->disks[k];
        const mks_records *r = &sd->r;
        uint32_t i;
        int esp_windows = 0;

        if (r->rc != PART_OK)
            continue;
        for (i = 0; i < r->table.count; i++) {
            const part_entry *p = &r->parts[i];
            uint8_t sec[512];
            uint8_t scratch[512];
            disk_io io;
            vbr_loader os;
            stn_found *f;

            if (r->table.id.kind == PART_KIND_MBR &&
                (part_mbr_is_extended(p->mbr_type) || !p->primary))
                continue;
            /* falcon's own partitions, and anything encrypted, are not
             * another system's. */
            if (r->recs[i].state != REC_NONE || r->recs[i].luks)
                continue;
            if (disk_io_open(&io, sd->d.path, p->start_lba * sd->d.sector,
                             (p->end_lba - p->start_lba + 1u) *
                                 sd->d.sector) != CLI_OK) {
                cli_error_clear();
                continue;
            }
            if (disk_io_read(&io, 0, sizeof sec, sec) != 0) {
                disk_io_close(&io);
                continue;
            }
            /* What the BIOS station would call it, FAT32's reserved
             * sectors asked as it asks them. */
            os = vbr_identify(sec, read_part_sector, &io, scratch);
            disk_io_close(&io);

            if (uefi && !esp_windows &&
                (r->table.id.kind == PART_KIND_GPT
                     ? gpt_guid_eq(p->type_guid, gpt_guid_esp)
                     : p->mbr_type == 0xEFu)) {
                size_t got = 0;

                if (stn_read_file(sd->d.path, p->start_lba * sd->d.sector,
                                  (p->end_lba - p->start_lba + 1u) *
                                      sd->d.sector,
                                  "/EFI/Microsoft/Boot/bootmgfw.efi", probe,
                                  sizeof probe, &got) == FS_ERR_RANGE) {
                    /* Found, and larger than the probe: which is all that
                     * was asked. */
                    f = new_found(s);
                    if (f != NULL) {
                        f->kind = STN_F_EFIWIN;
                        f->os = VBR_LOADER_BOOTMGR;
                        f->disk = r->table.id;
                        f->disk_ix = (uint32_t)k;
                        f->part = p->number;
                        /* The name and label the UEFI station gives this
                         * ESP's row. */
                        (void)stn_name_disk(&f->disk, f->part, f->name,
                                            sizeof f->name);
                        stn_found_label(f);
                        esp_windows = 1;
                    }
                }
                continue;
            }

            if (os == VBR_LOADER_NONE)
                continue;
            f = new_found(s);
            if (f == NULL)
                return;
            f->kind = STN_F_PART;
            f->os = os;
            f->disk = r->table.id;
            f->disk_ix = (uint32_t)k;
            f->part = p->number;
        }
        if (!sd->is_target && stn_mbr_chainable(sd->lba0)) {
            stn_found *f = new_found(s);

            if (f == NULL)
                return;
            f->kind = STN_F_DISK;
            f->disk = r->table.id;
            f->disk_ix = (uint32_t)k;
        }
    }
    for (k = 0; k < s->n_found; k++) {
        stn_found *f = &s->found[k];

        if (f->kind == STN_F_PART || f->kind == STN_F_DISK) {
            (void)stn_name_disk(&f->disk, f->part, f->name, sizeof f->name);
            stn_found_label(f);
        }
    }
}

void stn_scan_firmware(stn_scan *s, int *falcon_entry)
{
    char out[16384];
    size_t len = 0;
    char *line;
    char *save = NULL;

    *falcon_entry = -1;
    if (!util_is_dir("/sys/firmware/efi") || !proc_exists("efibootmgr"))
        return;
    if (proc_capture_l(out, sizeof out, &len, "efibootmgr", PROC_END) !=
        CLI_OK) {
        cli_error_clear();
        return;
    }
    out[len < sizeof out ? len : sizeof out - 1u] = '\0';
    for (line = strtok_r(out, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        unsigned num;
        const char *desc;
        stn_found *f;

        char hex[5];
        char *end = NULL;

        if (strncmp(line, "Boot", 4) != 0 || strlen(line) < 10u)
            continue;
        memcpy(hex, line + 4, 4);
        hex[4] = '\0';
        num = (unsigned)strtoul(hex, &end, 16);
        if (end != hex + 4)
            continue;
        desc = line + 9;
        while (*desc == ' ' || *desc == '*')
            desc++;
        if (strncmp(desc, STN_FW_LABEL, strlen(STN_FW_LABEL)) == 0 &&
            (desc[strlen(STN_FW_LABEL)] == '\t' ||
             desc[strlen(STN_FW_LABEL)] == ' ' ||
             desc[strlen(STN_FW_LABEL)] == '\0')) {
            *falcon_entry = (int)num;
            continue;
        }
        if (strncmp(desc, "Windows Boot Manager", 20) != 0)
            continue;
        f = new_found(s);
        if (f == NULL)
            return;
        f->kind = STN_F_FWENTRY;
        f->os = VBR_LOADER_BOOTMGR;
        f->boot_num = (uint16_t)num;
        (void)snprintf(f->name, sizeof f->name, "nxt-%04x", num);
        (void)snprintf(f->label, sizeof f->label, "%s", STROWS_LABEL_BOOTMGR);
    }
}

/* ---- remove ---- */

static const cli_opt remove_opts[] = {
    { "backup", CLI_OPT_VALUE, 0, "<file>",
      "the file `station install` wrote (required)" },
    { "full", CLI_OPT_FLAG, 0, NULL,
      "also put back the partition tables, the station partition's first "
      "sectors and other disks' signatures" },
    { "force", CLI_OPT_FLAG, 0, NULL,
      "restore onto a disk that carries no record of this install" },
    { "dry-run", CLI_OPT_FLAG, 0, NULL, "report and write nothing" }
};

static int carries_install(const mks_records *r, const uint8_t *id)
{
    uint32_t i;

    for (i = 0; i < r->table.count; i++) {
        if (r->recs[i].state == REC_OK &&
            memcmp(r->recs[i].rec.install_id, id, IDENT_ID_LEN) == 0)
            return 1;
    }
    return 0;
}

static const char *run_name(uint32_t kind)
{
    switch ((stn_run_kind)kind) {
    case STN_RUN_LBA0:       return "LBA 0";
    case STN_RUN_GAP:        return "the boot area (stage 2)";
    case STN_RUN_GPT:        return "the primary GPT";
    case STN_RUN_GPT_BACKUP: return "the backup GPT";
    case STN_RUN_REGION:     return "the station partition's first sectors";
    case STN_RUN_ESP_BOOT:   return "the ESP's boot sector";
    case STN_RUN_SIG:        return "LBA 0 (the disk signature)";
    }
    return "?";
}

static cli_status write_run(const char *path, uint32_t sector,
                            const stn_run *run, const uint8_t *data,
                            int whole_lba0)
{
    mks_dev dev;
    cli_status rc;

    dev.sector = sector;
    dev.fd = open(path, O_RDWR | O_CLOEXEC);
    if (dev.fd < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s", path);
    if (run->kind == STN_RUN_LBA0 && !whole_lba0) {
        uint8_t cur[BIOS_SECTOR_MAX];

        /* The boot code only: the table as it is now keeps the station
         * partition, which is the user's to delete. */
        rc = dev_read(&dev, 0, 1, cur);
        if (rc == CLI_OK) {
            memcpy(cur, data, BIOS_MBR_CODE_LEN);
            rc = dev_write(&dev, 0, cur, sector);
        }
    } else {
        rc = dev_write(&dev, run->lba, data, run->sectors * sector);
    }
    if (rc == CLI_OK && fsync(dev.fd) != 0)
        rc = set_error_errno(CLI_ERR_IO, "flushing %s", path);
    (void)close(dev.fd);
    return rc;
}

static cli_status remove_run(const cli_args *a)
{
    static uint8_t hdr[STN_BK_HDR];
    static mks_records r;
    static stn_scan fw;
    const char *bpath = cli_value(a, "backup", NULL);
    const char *target;
    int full = cli_flag(a, "full");
    int dry = cli_flag(a, "dry-run");
    uint8_t *data[STN_BK_MAX_RUNS];
    stn_backup b;
    char idhex[33];
    mks_disk d;
    size_t i;
    int fd;
    cli_status rc = CLI_OK;

    memset(data, 0, sizeof data);
    if (bpath == NULL)
        return set_error(CLI_ERR_USAGE, "which backup? --backup <file>");
    fd = open(bpath, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return set_error_errno(CLI_ERR_NOTFOUND, "opening %s", bpath);
    if (read(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr ||
        stn_backup_decode(hdr, sizeof hdr, &b) != STN_OK) {
        (void)close(fd);
        return set_error(CLI_ERR_INVALID, "%s is not a station backup",
                         bpath);
    }
    for (i = 0; i < b.n_runs && rc == CLI_OK; i++) {
        size_t bytes = (size_t)(b.runs[i].sectors * b.sector);

        data[i] = malloc(bytes);
        if (data[i] == NULL)
            rc = set_error(CLI_ERR_NOMEM, "no memory for the backup");
        else if (read(fd, data[i], bytes) != (ssize_t)bytes)
            rc = set_error(CLI_ERR_INVALID,
                           "%s is shorter than its header says", bpath);
    }
    (void)close(fd);
    if (rc != CLI_OK)
        goto out;

    /* The target named now wins over the one recorded, since a disk's
     * name can change between boots; its records say whether it is the
     * same disk. */
    target = (a->npos > 0u) ? a->pos[0] : b.disks[0];
    rc = disk_stat(&d, target);
    if (rc != CLI_OK)
        goto out;
    if (d.sector != b.sector) {
        rc = set_error(CLI_ERR_INVALID, "%s has %u-byte sectors; the backup "
                       "was taken from %u-byte ones", d.path, d.sector,
                       b.sector);
        goto out;
    }
    rc = records_read(&r, d.path, d.sector, d.size);
    if (rc != CLI_OK)
        goto out;
    records_id_hex(b.install_id, idhex);
    disk_describe(&d);
    log_info("backup:   %s, install ID %s", bpath, idhex);
    if (!carries_install(&r, b.install_id)) {
        if (!cli_flag(a, "force")) {
            rc = set_error(CLI_ERR_INVALID,
                           "no partition of %s carries install ID %s: it is "
                           "not the disk this backup was taken from, or the "
                           "station is already removed (--force restores "
                           "anyway)", d.path, idhex);
            goto out;
        }
        log_warn("no partition of %s carries install ID %s; --force given",
                 d.path, idhex);
    }
    if (b.esp_part != 0u && stn_part_mounted(&d, b.esp_part, NULL, 0)) {
        rc = set_error(CLI_ERR_BUSY, "the ESP (partition %u of %s) is "
                       "mounted; unmount it first", b.esp_part, d.path);
        goto out;
    }

    for (i = 0; i < b.n_runs; i++) {
        const stn_run *run = &b.runs[i];

        log_info("%s %s: %s (LBA %llu, %llu sector(s))",
                 stn_run_restores(run, full) ? "restore" : "keep   ",
                 run->disk == 0u ? d.path : b.disks[run->disk],
                 run_name(run->kind), (unsigned long long)run->lba,
                 (unsigned long long)run->sectors);
    }
    if (dry) {
        log_info("--dry-run: nothing written");
        goto out;
    }

    /* The ESP's files first, then its boot sector: the record goes last
     * there as LBA 0 does on the disk. */
    if ((b.flags & STN_FLAG_UEFI) != 0u && b.esp_part != 0u) {
        int e = stn_part_index(&r, b.esp_part);

        if (e < 0) {
            log_warn("%s has no partition %u; the ESP's files stay",
                     d.path, b.esp_part);
        } else {
            rc = stn_mtools_deltree(d.path,
                                    r.parts[e].start_lba * d.sector,
                                    STN_ESP_DIR);
            if (rc != CLI_OK)
                goto out;
            log_info("deleted \\EFI\\falcon from the ESP (partition %u)",
                     b.esp_part);
        }
    }
    if ((b.flags & STN_FLAG_FW_ENTRY) != 0u && b.fw_entry <= 0xFFFFu) {
        int ours = -1;

        stn_scan_firmware(&fw, &ours);
        if (ours == (int)b.fw_entry) {
            char num[8];

            (void)snprintf(num, sizeof num, "%04X", (unsigned)b.fw_entry);
            rc = proc_runl_quiet("efibootmgr", "-b", num, "-B", PROC_END);
            if (rc != CLI_OK)
                goto out;
            log_info("removed firmware entry Boot%s (%s)", num,
                     STN_FW_LABEL);
        } else {
            log_warn("firmware entry Boot%04X is not this machine's falcon "
                     "entry; left alone", (unsigned)b.fw_entry);
        }
    }

    /* Every run but the target's LBA 0, then that. */
    for (i = 0; i < b.n_runs && rc == CLI_OK; i++) {
        const stn_run *run = &b.runs[i];
        const char *path = run->disk == 0u ? d.path : b.disks[run->disk];

        if (!stn_run_restores(run, full) ||
            (run->disk == 0u && run->kind == STN_RUN_LBA0))
            continue;
        rc = write_run(path, b.sector, run, data[i], 0);
    }
    for (i = 0; i < b.n_runs && rc == CLI_OK; i++) {
        const stn_run *run = &b.runs[i];

        if (run->disk == 0u && run->kind == STN_RUN_LBA0)
            rc = write_run(d.path, b.sector, run, data[i],
                           stn_run_whole_lba0(run, full));
    }
    if (rc != CLI_OK)
        goto out;

    /* The FALCON-BIOS partition held stage 2 and nothing else; without
     * --full, whose table restore drops it, it goes by itself. */
    if (!full && (b.flags & STN_FLAG_MADE_EF02) != 0u && b.ef02_part != 0u) {
        char slot[16];

        (void)snprintf(slot, sizeof slot, "%u", b.ef02_part);
        rc = proc_runl_quiet("sgdisk", "-d", slot, d.path, PROC_END);
        if (rc != CLI_OK)
            goto out;
        log_info("partition %u (%s) removed", b.ef02_part, BIOS_BOOT_NAME);
    }
    if (d.is_block && proc_exists("partx"))
        (void)proc_runl_quiet("partx", "-u", d.path, PROC_END);

    if (full)
        log_info("%s is as it was before the install", d.path);
    else if (b.station_part != 0u && (b.flags & STN_FLAG_BIOS) != 0u)
        log_info("the station partition (partition %u) is left in place; "
                 "delete it when it is no longer wanted", b.station_part);
    log_info("the station is removed from %s", d.path);

out:
    for (i = 0; i < STN_BK_MAX_RUNS; i++)
        free(data[i]);
    return rc;
}

static const cli_cmd stn_cmd_remove = {
    "remove",
    "undo `station install` from its backup",
    "[<disk>]",
    remove_opts,
    sizeof remove_opts / sizeof remove_opts[0],
    0, 1,
    "Puts back what the backup holds: LBA 0's boot code, the boot area stage\n"
    "2 was written into, and the ESP's boot sector; deletes \\EFI\\falcon from\n"
    "the ESP and the firmware entry named falcon it created; removes the\n"
    "FALCON-BIOS partition it made on a GPT disk. The station partition is\n"
    "left for the user to delete. --full also puts back the partition\n"
    "tables (so the station partition's entry goes), the station\n"
    "partition's first sectors and the signatures it wrote on other disks.\n"
    "\n"
    "The disk is the one the backup names, unless <disk> names it now; one\n"
    "of its partitions must carry the backup's install ID.\n",
    remove_run
};

/* ---- status ---- */

static const cli_opt status_opts[] = {
    { "also", CLI_OPT_VALUE, 1, "<disk>",
      "another disk or image of the same machine, to list its systems" },
    { "no-scan", CLI_OPT_FLAG, 0, NULL,
      "read only the disk and --also, not every disk of this machine" }
};

typedef struct {
    unsigned ok;
    unsigned fail;
} stn_tally;

static void verdict(stn_tally *t, int good, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void verdict(stn_tally *t, int good, const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (good)
        t->ok++;
    else
        t->fail++;
    if (good)
        log_info("  %-4s %s", "OK", line);
    else
        log_error("  %-4s %s", "FAIL", line);
}

static void reach_note(const char *what, uint64_t end)
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

/* A row's words as the loader draws them, for a terminal: UCS-2 to ASCII,
 * anything else as '?'. */
static void label_ascii(const uint16_t *in, char *out, size_t cap)
{
    size_t i;

    for (i = 0; in[i] != 0 && i + 1u < cap; i++) {
        if (in[i] >= 0x20u && in[i] < 0x7Fu)
            out[i] = (char)in[i];
        else
            out[i] = '?';
    }
    out[i] = '\0';
}

static void label_wide(uint16_t *out, size_t cap, const char *in)
{
    size_t i;

    for (i = 0; in[i] != '\0' && i + 1u < cap; i++)
        out[i] = (uint16_t)(unsigned char)in[i];
    out[i] = 0;
}

/* What station-legacy adds to the BIOS list (DESIGN Section 13.10;
 * apps/station-legacy/bios_main.c, legacy_rows): jump's routes, each when
 * its driver is in the station partition, and the partition's own /uki. A
 * route's row also needs its controller on the machine's PCI bus, which is
 * known only at boot, so the list shows every route whose driver is there. */
#define STN_LEGACY_UKI_MAX 16u

typedef struct {
    int    any;           /* a driver is there: --legacy installed it */
    int    cd;            /* ATA.MOD */
    int    cf;            /* PCC.MOD */
    int    usb;           /* EHCI.MOD, UHCI.MOD or OHCI.MOD */
    size_t n_uki;
    struct {
        char name[STATIONINI_NAME_MAX];
        char file[STATIONINI_PATH_MAX];
        char title[STATIONINI_LABEL_MAX];
    } uki[STN_LEGACY_UKI_MAX];
} stn_legacy;

static void add_route_row(strows *rows, const char *name, const char *label)
{
    strow *w = strows_add(rows, STROW_ROUTE);

    if (w == NULL)
        return;
    (void)snprintf(w->name, sizeof w->name, "%s", name);
    label_wide(w->label, STROWS_LABEL_MAX, label);
}

/* The list a station shows, built as the loader builds it (DESIGN Section
 * 13.3; apps/station/efi_main.c and bios_main.c, rows_find and
 * build_rows) from what was found, then STATION.INI applied, ordered and
 * defaulted by media/strows.c, the module both loaders use. The UEFI
 * firmware-setup row is left out: whether the firmware offers it is known
 * only at boot. */
static void print_rows(const stn_scan *s, const stationini *ini, int have_ini,
                       int memtest, strows_fw fw, int encrypted,
                       const stn_legacy *lg)
{
    static strows rows;
    size_t view[STROWS_MAX];
    size_t n;
    size_t sel = 0;
    size_t i;
    int has_default;
    strow *w;

    strows_init(&rows);
    if (encrypted) {
        w = strows_add(&rows, STROW_POS);
        if (w != NULL)
            label_wide(w->label, STROWS_LABEL_MAX, STROWS_LABEL_POS);
    }
    for (i = 0; i < s->n_found; i++) {
        const stn_found *f = &s->found[i];
        strow_kind kind;

        switch (f->kind) {
        case STN_F_UKI:     kind = STROW_UKI; break;
        case STN_F_PART:    kind = STROW_VBR; break;
        case STN_F_DISK:    kind = STROW_MBR; break;
        case STN_F_FWENTRY: kind = STROW_NXT; break;
        case STN_F_EFIWIN:  kind = STROW_ESP; break;
        default:            continue;
        }
        w = strows_add(&rows, kind);
        if (w == NULL)
            break;
        (void)snprintf(w->name, sizeof w->name, "%s", f->name);
        label_wide(w->label, STROWS_LABEL_MAX,
                   f->title[0] != '\0' ? f->title : f->label);
        if (kind == STROW_UKI)
            (void)snprintf(w->file, sizeof w->file, "%s", f->label);
        if (kind == STROW_ESP)
            (void)snprintf(w->file, sizeof w->file, "%s", STROWS_BOOTMGFW);
        if (kind == STROW_VBR || kind == STROW_MBR || kind == STROW_ESP) {
            w->has_part = 1;
            w->disk = f->disk;
            w->part = f->part;
        }
    }
    if (lg != NULL && lg->any) {
        /* jump's order, then the station partition's shelf. */
        if (lg->cd)
            add_route_row(&rows, "cd", STROWS_LABEL_CD);
        if (lg->cf)
            add_route_row(&rows, "cf", STROWS_LABEL_CF);
        if (lg->usb)
            add_route_row(&rows, "usb", STROWS_LABEL_USB);
    }
    for (i = 0; lg != NULL && i < lg->n_uki; i++) {
        w = strows_add(&rows, STROW_UKI);
        if (w == NULL)
            break;
        (void)snprintf(w->name, sizeof w->name, "%s", lg->uki[i].name);
        (void)snprintf(w->file, sizeof w->file, "%s", lg->uki[i].file);
        label_wide(w->label, STROWS_LABEL_MAX, lg->uki[i].title);
    }
    w = memtest ? strows_add(&rows, STROW_MEMTEST) : NULL;
    if (w != NULL) {
        (void)snprintf(w->name, sizeof w->name, "memtest");
        label_wide(w->label, STROWS_LABEL_MAX, STROWS_LABEL_MEMTEST);
        w->own_esp = (uint8_t)(fw == STROWS_UEFI);
    }
    if (have_ini)
        strows_apply(&rows, ini, fw);

    n = strows_arrange(&rows, have_ini ? ini : NULL, view, STROWS_MAX);
    has_default = have_ini && strows_default(&rows, ini, view, n, &sel);
    for (i = 0; i < n; i++) {
        const strow *r = &rows.row[view[i]];
        char label[STROWS_LABEL_MAX];

        label_ascii(r->label, label, sizeof label);
        log_info("  %-20s %s%s%s",
                 r->section[0] != '\0' ? r->section
                 : r->name[0] != '\0'  ? r->name : "-",
                 label, r->added ? "  (added by STATION.INI)" : "",
                 has_default && sel == i ? "  [default]" : "");
    }
    for (i = 0; i < rows.n; i++) {
        const strow *r = &rows.row[i];
        char label[STROWS_LABEL_MAX];

        if (!r->hidden)
            continue;
        label_ascii(r->label, label, sizeof label);
        log_info("  %-20s %s  [hidden]",
                 r->section[0] != '\0' ? r->section : r->name, label);
    }
}

static void show_ini(stn_tally *t, const char *where, const char *text,
                     size_t len, stationini *ini, int *have)
{
    *have = 0;
    if (stationini_parse(text, len, ini) != 0) {
        verdict(t, 0, "%s: line %u: %s (no default, no countdown)", where,
                ini->err_line, ini->why);
        return;
    }
    *have = 1;
    verdict(t, 1, "%s parses: %u entr%s, default %s, timeout %u s", where,
            ini->n_entries, ini->n_entries == 1u ? "y" : "ies",
            ini->default_name[0] != '\0' ? ini->default_name : "(none)",
            ini->timeout);
}

/* The station partition's /uki, as station-legacy lists it: shelf.c's
 * labels without the "Personal OS: " they start with, since these are not
 * the personal OS (falcon-bios station.c, add_own_uki). */
static void legacy_shelf(const char *disk, uint64_t off, uint64_t len,
                         stn_legacy *lg)
{
    static const char prefix[] = "Personal OS: ";
    static fs_vol vol;
    static shelf_list list;
    const fs_ops *ops;
    disk_io io;
    size_t u;

    if (disk_io_open(&io, disk, off, len) != CLI_OK) {
        cli_error_clear();
        return;
    }
    ops = fs_identify(disk_io_read, &io, FS_HINT_NONE);
    if (ops != NULL && fs_mount(&vol, ops, disk_io_read, &io) == FS_OK &&
        shelf_collect(&list, &vol) > 0) {
        shelf_label(&list, &vol);
        for (u = 0; u < list.n && lg->n_uki < STN_LEGACY_UKI_MAX; u++) {
            uint8_t name[FS_NAME_MAX + 1u];
            size_t n = fs_name_to_bytes(list.uki[u].ent.name, name,
                                        sizeof name - 1u);
            char title[STATIONINI_LABEL_MAX];
            size_t k = lg->n_uki++;
            const char *words = title;

            name[n] = '\0';
            if (stn_name_uki((const char *)name, lg->uki[k].name,
                             sizeof lg->uki[k].name) != 0)
                (void)snprintf(lg->uki[k].name, sizeof lg->uki[k].name,
                               "uki");
            (void)snprintf(lg->uki[k].file, sizeof lg->uki[k].file,
                           "%.127s", (const char *)name);
            label_ascii(list.uki[u].label, title, sizeof title);
            if (strncmp(title, prefix, sizeof prefix - 1u) == 0 &&
                title[sizeof prefix - 1u] != '\0')
                words = title + sizeof prefix - 1u;
            (void)snprintf(lg->uki[k].title, sizeof lg->uki[k].title, "%s",
                           words);
        }
        shelf_wipe(&list);
    }
    disk_io_close(&io);
}

static void status_bios(stn_tally *t, stn_scan *s, stationini *ini,
                        int *have_ini, int *memtest, stn_legacy *lg)
{
    static char text[16384];
    stn_disk *sd = &s->disks[0];
    const mks_records *r = &sd->r;
    int boot = stn_bios_boot(r);
    uint64_t s2_lba = 0;
    uint64_t s2_n = 0;
    uint8_t sec[512];
    disk_io io;
    size_t got = 0;
    const part_entry *bp;
    uint64_t off;
    uint64_t len;

    cli_log_blank();
    log_info("BIOS (DESIGN Sections 3.3, 13.2)");
    if (boot < 0) {
        log_info("  --   no station partition: BIOS is not installed");
        return;
    }
    bp = &r->parts[boot];
    off = bp->start_lba * sd->d.sector;
    len = (bp->end_lba - bp->start_lba + 1u) * sd->d.sector;
    verdict(t, 1, "station partition: partition %u, LBA %llu..%llu",
            bp->number, (unsigned long long)bp->start_lba,
            (unsigned long long)bp->end_lba);
    reach_note("the station partition", off + len);

    if (disk_io_open(&io, sd->d.path, 0, sd->d.size) != CLI_OK) {
        verdict(t, 0, "%s", cli_error_msg());
        return;
    }
    if (bioscore_stage1_fields(sd->lba0, STAGE1_S2_LBA_OFF,
                               STAGE1_S2_SECTORS_OFF, &s2_lba, &s2_n)) {
        verdict(t, 1, "stage 1 in LBA 0 names stage 2 at LBA %llu, %llu "
                "sector(s)", (unsigned long long)s2_lba,
                (unsigned long long)s2_n);
        verdict(t, disk_io_read(&io, s2_lba * sd->d.sector, sizeof sec,
                                sec) == 0 && memcmp(sec + 4, "BOS2", 4) == 0,
                "stage 2 is at LBA %llu", (unsigned long long)s2_lba);
    } else {
        verdict(t, 0, "LBA 0 carries no falcon stage 1");
    }
    verdict(t, disk_io_read(&io, (bp->start_lba +
                                  r->recs[boot].rec.stage3_sector) *
                                     sd->d.sector,
                            sizeof sec, sec) == 0 &&
                   memcmp(sec, "BOSZ", 4) == 0,
            "stage 3 is at sector %u of partition %u",
            (unsigned)r->recs[boot].rec.stage3_sector, bp->number);
    if (r->recs[boot].rec.marker_sector != 0u &&
        disk_io_read(&io, (bp->start_lba + r->recs[boot].rec.marker_sector) *
                              sd->d.sector,
                     sizeof sec, sec) == 0)
        log_info("  --   next-boot marker: %s",
                 nextboot_sector_is_marker(sec, sizeof sec) ? "set"
                                                            : "empty");
    disk_io_close(&io);

    *memtest = (stn_read_file(sd->d.path, off, len, "/" STN_BIOS_MEMTEST,
                              text, 16, &got) == FS_ERR_RANGE);
    if (*memtest)
        log_info("  --   %s present", STN_BIOS_MEMTEST);
    {
        static const char *const mods[] = {
            "ATA.MOD", "PCC.MOD", "EHCI.MOD", "UHCI.MOD", "OHCI.MOD"
        };
        char path[16];
        size_t i;

        for (i = 0; i < sizeof mods / sizeof mods[0]; i++) {
            int have;

            (void)snprintf(path, sizeof path, "/%s", mods[i]);
            have = stn_read_file(sd->d.path, off, len, path, text, 16,
                                 &got) == FS_ERR_RANGE;
            if (!have)
                continue;
            lg->any = 1;
            if (i == 0u)
                lg->cd = 1;
            else if (i == 1u)
                lg->cf = 1;
            else
                lg->usb = 1;
        }
        if (lg->any) {
            log_info("  --   station-legacy's drivers:%s%s%s",
                     lg->cd ? " CD (ATA.MOD)" : "",
                     lg->cf ? " CF (PCC.MOD)" : "",
                     lg->usb ? " USB" : "");
            legacy_shelf(sd->d.path, off, len, lg);
            log_info("  --   /uki: %zu image(s)", lg->n_uki);
        }
    }
    if (stn_read_file(sd->d.path, off, len, "/" STATIONINI_FILE, text,
                      sizeof text, &got) == FS_OK)
        show_ini(t, "/" STATIONINI_FILE, text, got, ini, have_ini);
    else
        log_info("  --   no /%s: the menu waits", STATIONINI_FILE);
}

static void status_uefi(stn_tally *t, stn_scan *s, stationini *ini,
                        int *have_ini, int *memtest)
{
    static char text[16384];
    static const char *const files[] = {
        STN_ESP_SHIM, "grubx64.efi", STN_ESP_CERT
    };
    stn_disk *sd = &s->disks[0];
    const mks_records *r = &sd->r;
    int e = stn_find_esp(r);
    const part_entry *p;
    uint64_t off;
    uint64_t len;
    size_t got = 0;
    size_t i;
    char path[128];

    cli_log_blank();
    log_info("UEFI (DESIGN Section 13.2)");
    if (e < 0) {
        log_info("  --   no ESP on this disk");
        return;
    }
    p = &r->parts[e];
    off = p->start_lba * sd->d.sector;
    len = (p->end_lba - p->start_lba + 1u) * sd->d.sector;
    (void)snprintf(path, sizeof path, "/%s/%s", STN_ESP_DIR, STN_ESP_SHIM);
    if (stn_read_file(sd->d.path, off, len, path, text, 16, &got) !=
        FS_ERR_RANGE) {
        log_info("  --   the ESP (partition %u) has no \\EFI\\falcon: UEFI "
                 "is not installed", p->number);
        return;
    }
    verdict(t, r->recs[e].state == REC_OK &&
                   r->recs[e].rec.role == IDENT_ROLE_BOOT,
            "the ESP (partition %u) carries the station's boot record",
            p->number);
    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        (void)snprintf(path, sizeof path, "/%s/%s", STN_ESP_DIR, files[i]);
        verdict(t, stn_read_file(sd->d.path, off, len, path, text, 16,
                                 &got) == FS_ERR_RANGE,
                "\\EFI\\falcon\\%s", files[i]);
    }
    (void)snprintf(path, sizeof path, "/%s/%s", STN_ESP_DIR,
                   STN_ESP_MEMTEST);
    *memtest = (stn_read_file(sd->d.path, off, len, path, text, 16, &got) ==
                FS_ERR_RANGE);
    if (*memtest)
        log_info("  --   \\EFI\\falcon\\%s present", STN_ESP_MEMTEST);
    (void)snprintf(path, sizeof path, "/%s/%s", STN_ESP_DIR,
                   STATIONINI_FILE);
    if (stn_read_file(sd->d.path, off, len, path, text, sizeof text,
                      &got) == FS_OK)
        show_ini(t, "\\EFI\\falcon\\" STATIONINI_FILE, text, got, ini,
                 have_ini);
    else
        log_info("  --   no \\EFI\\falcon\\%s: the menu waits",
                 STATIONINI_FILE);
}

/* A LUKS2 shelf's keyslots: each one's KDF and cost, what the loaders
 * cannot use, and a slot asking for more memory than this machine has,
 * which the loader refuses with a page saying so (docs/notes/argon2-totp.md
 * Section 1). mkstick makes no shelf; one made by hand with cryptsetup has
 * whatever cost cryptsetup benchmarked on the machine that made it. */
static void shelf_slots(const stn_disk *sd, const part_entry *p)
{
    static char json[LUKS_JSON_CAP];
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    uint64_t have_mib = (pages > 0 && psize > 0)
                            ? ((uint64_t)pages * (uint64_t)psize) >> 20
                            : 0;
    luks_vol vol;
    disk_io io;
    size_t n = 0;
    size_t usable = 0;
    size_t i;

    if (disk_io_open(&io, sd->d.path, p->start_lba * sd->d.sector,
                     (p->end_lba - p->start_lba + 1u) * sd->d.sector) !=
        CLI_OK) {
        cli_error_clear();
        return;
    }
    if (luks_open(&vol, disk_io_read, &io, json, sizeof json) != LUKS_OK) {
        log_warn("the shelf's LUKS2 header could not be read");
        disk_io_close(&io);
        return;
    }
    (void)luks_slot_count(&vol, &n);
    for (i = 0; i < n; i++) {
        luks_slot_info info;
        char kdf[96];

        if (luks_slot_at(&vol, i, &info) != LUKS_OK)
            continue;
        mks_slot_describe(&info, kdf, sizeof kdf);
        log_info("       keyslot %u: %s%s", info.id, kdf,
                 info.usable ? "" : "; the loaders cannot use it");
        if (info.usable)
            usable++;
        if (info.kdf == LUKS_KDF_ARGON2ID && have_mib != 0 &&
            (info.kdf_memory_kib + 1023u) / 1024u > have_mib)
            log_warn("keyslot %u needs %u MiB; this machine has %llu MiB "
                     "of memory", info.id,
                     (info.kdf_memory_kib + 1023u) / 1024u,
                     (unsigned long long)have_mib);
    }
    if (usable == 0)
        log_warn("no keyslot of the shelf is argon2id or pbkdf2: the "
                 "loaders cannot open it");
    disk_io_close(&io);
}

/* The shelves carrying the install ID, said; a plaintext one's UKIs added
 * to what was found. Returns nonzero when one is LUKS2: the loader's
 * "Personal OS (encrypted)" row. */
static int status_shelf(stn_scan *s, const uint8_t *id)
{
    size_t k;
    uint32_t i;
    int any = 0;
    int encrypted = 0;

    cli_log_blank();
    log_info("the shelf (DESIGN Section 13.2)");
    for (k = 0; k < s->n_disks; k++) {
        const stn_disk *sd = &s->disks[k];

        for (i = 0; i < sd->r.table.count; i++) {
            const mks_rec *rec = &sd->r.recs[i];
            const part_entry *p = &sd->r.parts[i];

            if (rec->state != REC_OK || rec->rec.role != IDENT_ROLE_SHELF ||
                memcmp(rec->rec.install_id, id, IDENT_ID_LEN) != 0)
                continue;
            any = 1;
            if (rec->luks)
                encrypted = 1;
            log_info("  OK   %s partition %u (%s)", sd->d.path, p->number,
                     rec->luks ? "LUKS2: its UKIs are rows once unlocked"
                               : "plaintext");
            reach_note("the shelf", (p->end_lba + 1u) * sd->d.sector);
            if (rec->luks)
                shelf_slots(sd, p);
            if (!rec->luks) {
                static fs_vol vol;
                static shelf_list list;
                disk_io io;
                const fs_ops *ops;
                size_t u;

                if (disk_io_open(&io, sd->d.path,
                                 p->start_lba * sd->d.sector,
                                 (p->end_lba - p->start_lba + 1u) *
                                     sd->d.sector) != CLI_OK) {
                    cli_error_clear();
                    continue;
                }
                ops = fs_identify(disk_io_read, &io, FS_HINT_SHELF);
                if (ops != NULL && fs_mount(&vol, ops, disk_io_read, &io) ==
                                       FS_OK &&
                    shelf_collect(&list, &vol) > 0) {
                    shelf_label(&list, &vol);
                    for (u = 0; u < list.n; u++) {
                        stn_found *f = new_found(s);
                        uint8_t name[FS_NAME_MAX + 1u];
                        size_t n = fs_name_to_bytes(list.uki[u].ent.name,
                                                    name, sizeof name - 1u);

                        name[n] = '\0';
                        if (f == NULL)
                            break;
                        f->kind = STN_F_UKI;
                        if (stn_name_uki((const char *)name, f->name,
                                         sizeof f->name) != 0)
                            (void)snprintf(f->name, sizeof f->name, "uki");
                        (void)snprintf(f->label, sizeof f->label, "%.63s",
                                       (const char *)name);
                        label_ascii(list.uki[u].label, f->title,
                                    sizeof f->title);
                    }
                    shelf_wipe(&list);
                }
                disk_io_close(&io);
            }
        }
    }
    if (!any)
        log_info("  --   no partition carries this station's shelf record");
    return encrypted;
}

static cli_status status_run(const cli_args *a)
{
    static stn_scan s;
    static stationini ini_bios;
    static stn_legacy legacy;
    static stationini ini_uefi;
    const char *also[CLI_MAX_VALUES] = { NULL };
    size_t n_also = cli_count(a, "also");
    stn_tally t;
    char dtext[PART_DISK_ID_TEXT];
    char idhex[33];
    const uint8_t *id = NULL;
    int have_bios = 0;
    int have_uefi = 0;
    int mem_bios = 0;
    int mem_uefi = 0;
    int encrypted;
    int boot;
    int esp;
    size_t i;
    uint32_t k;
    cli_status rc;

    memset(&t, 0, sizeof t);
    for (i = 0; i < n_also; i++)
        also[i] = cli_at(a, "also", i);
    rc = stn_scan_disks(&s, a->pos[0], also, n_also, cli_flag(a, "no-scan"));
    if (rc != CLI_OK)
        return rc;
    disk_describe(&s.disks[0].d);
    if (s.disks[0].r.rc != PART_OK)
        return set_error(CLI_ERR_INVALID, "%s has no usable partition table",
                         s.disks[0].d.path);
    part_disk_id_format(&s.disks[0].r.table.id, dtext);
    log_info("disk:     %s", dtext);

    boot = stn_bios_boot(&s.disks[0].r);
    esp = stn_find_esp(&s.disks[0].r);
    if (boot >= 0)
        id = s.disks[0].r.recs[boot].rec.install_id;
    else if (esp >= 0 && s.disks[0].r.recs[esp].state == REC_OK &&
             s.disks[0].r.recs[esp].rec.role == IDENT_ROLE_BOOT)
        id = s.disks[0].r.recs[esp].rec.install_id;
    if (id == NULL) {
        log_info("no partition of %s carries a station's boot record: "
                 "falcon is not installed here", s.disks[0].d.path);
        return CLI_OK;
    }
    records_id_hex(id, idhex);
    log_info("install:  %s", idhex);
    cli_log_blank();
    log_info("partitions");
    for (k = 0; k < s.disks[0].r.table.count; k++) {
        const mks_rec *rec = &s.disks[0].r.recs[k];
        const part_entry *p = &s.disks[0].r.parts[k];

        if (rec->state == REC_OK) {
            records_id_hex(rec->rec.install_id, idhex);
            log_info("  partition %-3u LBA %-10llu %s, install ID %s%s",
                     p->number, (unsigned long long)p->start_lba,
                     ident_role_name(rec->rec.role), idhex,
                     memcmp(rec->rec.install_id, id, IDENT_ID_LEN) == 0
                         ? "" : " (another install)");
        } else {
            log_info("  partition %-3u LBA %-10llu no record",
                     p->number, (unsigned long long)p->start_lba);
        }
    }

    status_bios(&t, &s, &ini_bios, &have_bios, &mem_bios, &legacy);
    status_uefi(&t, &s, &ini_uefi, &have_uefi, &mem_uefi);
    encrypted = status_shelf(&s, id);

    stn_scan_found(&s, 0);
    if (boot >= 0) {
        cli_log_blank();
        log_info("what the BIOS menu offers");
        print_rows(&s, &ini_bios, have_bios, mem_bios, STROWS_BIOS,
                   encrypted, &legacy);
        if (legacy.cd || legacy.cf || legacy.usb)
            log_info("  (CD, CF and USB are rows when the machine has the "
                     "controller)");
    }
    if (esp >= 0 && have_uefi) {
        int ours = -1;

        {
            size_t n = 0;

            /* The shelf's UKIs stay; the BIOS rows go. */
            for (i = 0; i < s.n_found; i++) {
                if (s.found[i].kind == STN_F_UKI)
                    s.found[n++] = s.found[i];
            }
            s.n_found = n;
        }
        if (stn_is_machine(&s.disks[0].d))
            stn_scan_firmware(&s, &ours);
        stn_scan_found(&s, 1);
        {
            size_t n = 0;

            for (i = 0; i < s.n_found; i++) {
                if (s.found[i].kind != STN_F_PART &&
                    s.found[i].kind != STN_F_DISK)
                    s.found[n++] = s.found[i];
            }
            s.n_found = n;
        }
        cli_log_blank();
        log_info("what the UEFI menu offers%s",
                 !stn_is_machine(&s.disks[0].d)
                     ? " (an image: the firmware's entries are its machine's)"
                 : !util_is_dir("/sys/firmware/efi")
                     ? " (this machine did not boot with UEFI: its firmware "
                       "entries are not listed)" : "");
        print_rows(&s, &ini_uefi, have_uefi, mem_uefi, STROWS_UEFI,
                   encrypted, NULL);
    }

    cli_log_blank();
    log_info("%u checks passed, %u failures", t.ok, t.fail);
    if (t.fail > 0u)
        return set_error(CLI_ERR_INVALID, "the station on %s is incomplete",
                         s.disks[0].d.path);
    return CLI_OK;
}

static const cli_cmd stn_cmd_status = {
    "status",
    "what station is installed on a disk, and what its menu offers",
    "<disk>",
    status_opts,
    sizeof status_opts / sizeof status_opts[0],
    1, 1,
    "Read-only. Reads the disk's table and records with the loaders' own\n"
    "code: the install ID, the BIOS stages where stage 1 and the station\n"
    "partition's record put them, \\EFI\\falcon on the ESP, each STATION.INI\n"
    "as the loaders parse it, the shelf carrying the install ID, and the\n"
    "rows each menu shows (found systems, what STATION.INI adds, relabels\n"
    "and hides, and the default). On a block device every disk of this\n"
    "machine is read; --also adds a disk or image.\n",
    status_run
};

/* ---- dispatch ---- */

static const cli_cmd *const station_cmds[] = {
    &stn_cmd_install,
    &stn_cmd_remove,
    &stn_cmd_status
};

static cli_status station_placeholder(const cli_args *a)
{
    (void)a;
    return set_error(CLI_ERR_USAGE, "station install, remove or status?");
}

const cli_cmd mks_cmd_station = {
    "station",
    "install falcon as a machine's boot loader: install, remove, status",
    "install|remove|status ...",
    NULL,
    0,
    0, 0,
    "  station install <disk>   put the station on a machine's disk\n"
    "  station remove  [<disk>] --backup <file>\n"
    "  station status  <disk>\n"
    "\n"
    "`falcon-boot-mkstick station <command> --help` for each.\n",
    station_placeholder
};

cli_status mks_station_main(int argc, char **argv)
{
    int i;

    for (i = 0; i < argc; i++) {
        if (argv[i][0] != '-')
            break;
    }
    if (i >= argc) {
        int help = 0;
        int k;

        for (k = 0; k < argc; k++) {
            if (strcmp(argv[k], "--help") == 0 || strcmp(argv[k], "-h") == 0)
                help = 1;
        }
        cli_help_set_topic(&mks_cmd_station);
        if (help) {
            cli_help_print();
            return CLI_OK;
        }
        return set_error(CLI_ERR_USAGE, "station install, remove or status?");
    }
    return cli_dispatch(station_cmds, sizeof station_cmds /
                                          sizeof station_cmds[0],
                        "falcon-boot-mkstick station", argc, argv);
}
