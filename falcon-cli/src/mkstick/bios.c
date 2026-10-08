/* falcon-boot mkstick: `bios`, installing falcon-bios onto a disk that
 * already carries a `boot` partition (docs/DESIGN.md Section 3.3).
 *
 * It remakes nothing. It writes four things, all outside every file:
 *
 *   - stage 1 into LBA 0's boot code, with stage 2's place patched into it;
 *   - stage 2 after LBA 0 and before the first partition (LBA 34 on GPT,
 *     past the entry array; LBA 1 on MBR);
 *   - stage 3 into the `boot` partition's reserved sectors, at the sector
 *     that partition's identity record names;
 *   - the next-boot marker sector the record names, zeroed.
 *
 * The boot partition is found the way stage 2 finds it: the table, then the
 * first ESP- or FAT-typed partition whose boot sector carries a `boot`
 * record. `mkstick create` makes one on every stick (a FAT32 ESP formatted
 * with room in its reserved sectors); a disk without one is refused.
 *
 * It is opt-in. Installing stage 1 makes a disk bootable on machines that
 * ignore it today, and a BIOS machine has no Secure Boot, no shim and no db,
 * so the BIOS route has no blessed path. That is a change in what a stick can
 * reach, so somebody asks for it.
 *
 * ORDER OF WRITES IS LOAD BEARING. The stages go down first and LBA 0 last,
 * so an install interrupted at any point leaves a disk whose boot sector
 * either is not ours or points at stages that are already there.
 */

#include <mkstick/mkstick.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <part/part.h>

#include <common/datadir.h>
#include <common/log.h>
#include <common/proc.h>
#include <common/util.h>
#include <mkstick/bioscore.h>
#include <mkstick/devio.h>
#include <mkstick/disk.h>
#include <mkstick/records.h>

/* The loader's own numbers, read from the header that fixes them rather than
 * written a second time here. */
#include <falcon-bios/src/start/biosdefs.h>

#define DEFAULT_STAGE1 FALCON_DATADIR "/bin/falcon-bios-stage1.bin"
#define DEFAULT_STAGE2 FALCON_DATADIR "/bin/falcon-bios-stage2.bin"
#define DEFAULT_STAGE3 FALCON_DATADIR "/bin/falcon-bios-stage3.bin"

/* Big enough for the largest stage; the plan checks what actually fits. */
#define STAGE_MAX ((size_t)4 * 1024 * 1024)

static const cli_opt bios_opts[] = {
    { "stage1", CLI_OPT_VALUE, 0, "<file>",
      "the 512-byte MBR image (default " DEFAULT_STAGE1 ")" },
    { "stage2", CLI_OPT_VALUE, 0, "<file>",
      "the second stage (default " DEFAULT_STAGE2 ")" },
    { "stage3", CLI_OPT_VALUE, 0, "<file>",
      "the third stage (default " DEFAULT_STAGE3 ")" },
    { "backup", CLI_OPT_VALUE, 0, "<file>",
      "where to save the sectors being overwritten, and what --remove "
      "restores from" },
    { "no-backup", CLI_OPT_FLAG, 0, NULL,
      "do not save the sectors being overwritten" },
    { "remove", CLI_OPT_FLAG, 0, NULL,
      "undo an install: restore --backup, or clear what an install wrote" },
    { "reserve", CLI_OPT_FLAG, 0, NULL,
      "GPT only: also claim stage 2's gap as a BIOS boot partition (slot "
      "5, type EF02); rewrites the partition table" },
    { "dry-run", CLI_OPT_FLAG, 0, NULL,
      "report the plan and write nothing" },
    { "force", CLI_OPT_FLAG, 0, NULL,
      "overwrite an existing backup file, and allow --reserve on a disk "
      "with mounted partitions" }
};

static int mbr_has_code(const uint8_t *mbr)
{
    size_t i;

    for (i = 0; i < BIOS_MBR_CODE_LEN; i++) {
        if (mbr[i] != 0u)
            return 1;
    }
    return 0;
}

/* ---- the disk's own facts ---- */

typedef struct {
    int      boot;            /* index into recs of the boot partition */
    int      have_reserve;    /* the FALCON-BIOS reservation exists */
    uint32_t reserve_slot;    /* and its slot, which --remove deletes */
    int      mounted;
    bios_disk disk;
} bios_survey;

static cli_status survey(mks_dev *dev, const mks_disk *d, mks_records *recs,
                         bios_survey *out)
{
    uint8_t bs[BIOS_SECTOR_MAX];
    const part_entry *bp;
    const ident_rec *rec;
    uint32_t i;
    cli_status rc;

    memset(out, 0, sizeof *out);
    out->boot = -1;

    rc = records_read(recs, d->path, d->sector, d->size);
    if (rc != CLI_OK)
        return rc;
    if (recs->rc != PART_OK || recs->table.count == 0u)
        return set_error(CLI_ERR_INVALID,
                         "%s has no usable partition table (part_read_table "
                         "%d)", d->path, recs->rc);

    out->boot = records_bios_boot(recs);
    if (out->boot < 0)
        return set_error(CLI_ERR_INVALID,
                         "no partition on %s carries a boot record naming a "
                         "stage 3 sector; `mkstick create` makes one, and `mkstick ident --write-boot` "
                         "stamps an existing FAT partition", d->path);

    out->disk.sector_bytes = d->sector;
    out->disk.kind = recs->table.id.kind;
    out->disk.first_usable = recs->table.first_usable;
    for (i = 0; i < recs->table.count; i++) {
        const part_entry *p = &recs->parts[i];

        /* Our own reservation covers stage 2's gap precisely because stage 2
         * goes there, so it is not what the gap is measured against. */
        if (bioscore_is_reserve(&recs->table, p)) {
            out->have_reserve = 1;
            out->reserve_slot = p->number;
            continue;
        }
        if (out->disk.first_part_lba == 0u ||
            p->start_lba < out->disk.first_part_lba)
            out->disk.first_part_lba = p->start_lba;
    }

    bp = &recs->parts[out->boot];
    rec = &recs->recs[out->boot].rec;
    out->disk.boot_start = bp->start_lba;
    out->disk.boot_sectors = bp->end_lba - bp->start_lba + 1u;
    out->disk.stage3_sector = rec->stage3_sector;
    out->disk.marker_sector = rec->marker_sector;

    rc = dev_read(dev, bp->start_lba, 1, bs);
    if (rc != CLI_OK)
        return rc;
    if (bioscore_fat_bpb(bs, &out->disk.fat) != BIOSCORE_OK)
        return set_error(CLI_ERR_INVALID,
                         "p%u of %s carries a boot record but is not a FAT "
                         "volume", bp->number, d->path);

    out->mounted = (disk_check_not_mounted(d) != CLI_OK);
    cli_error_clear();
    return CLI_OK;
}

/* ---- backup ---- */

static cli_status backup_write(const mks_dev *dev, const char *path,
                               const bios_run *runs, size_t n, int force)
{
    uint8_t hdr[BIOS_BACKUP_HDR];
    uint8_t *buf;
    uint64_t total = 0;
    size_t i;
    int fd;
    cli_status rc = CLI_OK;

    if (!force && util_is_file(path))
        return set_error(CLI_ERR_EXISTS,
                         "%s exists; it holds what was on this disk before an "
                         "earlier install, and a second run would replace it "
                         "with what that install wrote. Name another file, or "
                         "--force", path);
    if (bioscore_backup_header(hdr, dev->sector, runs, n) != BIOSCORE_OK)
        return set_error(CLI_ERR_INTERNAL, "no runs to back up");

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return set_error_errno(CLI_ERR_IO, "creating %s", path);
    if (write(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) {
        (void)close(fd);
        return set_error_errno(CLI_ERR_IO, "writing %s", path);
    }
    for (i = 0; i < n && rc == CLI_OK; i++) {
        size_t bytes = (size_t)(runs[i].sectors * dev->sector);

        buf = malloc(bytes);
        if (buf == NULL) {
            rc = set_error(CLI_ERR_NOMEM, "no memory for the backup");
            break;
        }
        rc = dev_read(dev, runs[i].lba, runs[i].sectors, buf);
        if (rc == CLI_OK && write(fd, buf, bytes) != (ssize_t)bytes)
            rc = set_error_errno(CLI_ERR_IO, "writing %s", path);
        free(buf);
        total += runs[i].sectors;
    }
    if (rc == CLI_OK && fsync(fd) != 0)
        rc = set_error_errno(CLI_ERR_IO, "flushing %s", path);
    if (close(fd) != 0 && rc == CLI_OK)
        rc = set_error_errno(CLI_ERR_IO, "closing %s", path);
    if (rc == CLI_OK)
        log_info("saved %llu sector(s) to %s", (unsigned long long)total, path);
    return rc;
}

static cli_status backup_restore(const mks_dev *dev, const char *path)
{
    uint8_t hdr[BIOS_BACKUP_HDR];
    bios_run runs[BIOS_BACKUP_MAX_RUNS];
    uint8_t *data[BIOS_BACKUP_MAX_RUNS];
    uint32_t sector_bytes = 0;
    size_t n = 0;
    size_t i;
    bioscore_status bs;
    cli_status rc = CLI_OK;
    int fd;

    memset(data, 0, sizeof data);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return set_error_errno(CLI_ERR_NOTFOUND, "opening %s", path);
    if (read(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) {
        (void)close(fd);
        return set_error(CLI_ERR_INVALID, "%s is too short to be a backup",
                         path);
    }
    bs = bioscore_backup_parse(hdr, sizeof hdr, &sector_bytes, runs,
                               BIOS_BACKUP_MAX_RUNS, &n);
    if (bs != BIOSCORE_OK) {
        (void)close(fd);
        return set_error(CLI_ERR_INVALID, "%s: %s", path,
                         bioscore_strerror(bs));
    }
    /* The parse bounds n by the array; said again here, where the loops
     * that index by it are, for the analyzer that cannot see into it. */
    if (n > BIOS_BACKUP_MAX_RUNS) {
        (void)close(fd);
        return set_error(CLI_ERR_INVALID, "%s: too many runs", path);
    }
    if (sector_bytes != dev->sector) {
        (void)close(fd);
        return set_error(CLI_ERR_INVALID,
                         "%s was taken from a disk with %u-byte sectors, not "
                         "%u", path, sector_bytes, dev->sector);
    }

    /* Everything is read before anything is written, so a short file fails
     * with the disk untouched. */
    for (i = 0; i < n; i++) {
        size_t bytes = (size_t)(runs[i].sectors * sector_bytes);

        data[i] = malloc(bytes);
        if (data[i] == NULL) {
            rc = set_error(CLI_ERR_NOMEM, "no memory to read %s", path);
            break;
        }
        if (read(fd, data[i], bytes) != (ssize_t)bytes) {
            rc = set_error(CLI_ERR_INVALID,
                           "%s is shorter than its header says", path);
            break;
        }
    }
    (void)close(fd);

    /* LBA 0 last, as an install writes it last. It goes back whole: a
     * restore is putting that exact sector back. */
    for (i = 0; i < n && rc == CLI_OK; i++) {
        if (runs[i].lba != 0u)
            rc = dev_write(dev, runs[i].lba, data[i],
                           runs[i].sectors * sector_bytes);
    }
    for (i = 0; i < n && rc == CLI_OK; i++) {
        if (runs[i].lba == 0u)
            rc = dev_write(dev, 0, data[i], runs[i].sectors * sector_bytes);
    }
    for (i = 0; i < BIOS_BACKUP_MAX_RUNS; i++)
        free(data[i]);
    if (rc == CLI_OK)
        log_info("restored %zu run(s) from %s", n, path);
    return rc;
}

/* ---- --remove with no backup: clear what an install wrote ---- */

static cli_status clear_install(const mks_dev *dev, const mks_disk *d,
                                const bios_survey *s, uint8_t *mbr)
{
    uint8_t sec[BIOS_SECTOR_MAX];
    uint64_t s2_lba = 0;
    uint64_t s2_n = 0;
    uint64_t s3_lba = s->disk.boot_start + s->disk.stage3_sector;
    cli_status rc;

    /* Stage 2's run is what LBA 0 says it is, believed only when it lies in
     * the gap and starts with the gate stage 1 checks. */
    if (bioscore_stage1_fields(mbr, STAGE1_S2_LBA_OFF, STAGE1_S2_SECTORS_OFF,
                               &s2_lba, &s2_n) &&
        s2_lba >= (s->disk.kind == PART_KIND_GPT ? s->disk.first_usable : 1u) &&
        s2_lba + s2_n <= s->disk.first_part_lba && s2_n <= STAGE2_SECTORS &&
        dev_read(dev, s2_lba, 1, sec) == CLI_OK &&
        memcmp(sec + 4, "BOS2", 4) == 0) {
        rc = dev_zero(dev, s2_lba, s2_n);
        if (rc != CLI_OK)
            return rc;
        log_info("cleared stage 2 at LBA %llu..%llu",
                 (unsigned long long)s2_lba,
                 (unsigned long long)(s2_lba + s2_n - 1u));
    }

    /* Stage 3's run is its own header's length, inside the reserved area. */
    rc = dev_read(dev, s3_lba, 1, sec);
    if (rc != CLI_OK)
        return rc;
    if (memcmp(sec, "BOSZ", 4) == 0) {
        uint32_t len = (uint32_t)sec[4] | ((uint32_t)sec[5] << 8) |
                       ((uint32_t)sec[6] << 16) | ((uint32_t)sec[7] << 24);
        uint64_t n = ((uint64_t)len + dev->sector - 1u) / dev->sector;
        uint64_t room = s->disk.fat.reserved - s->disk.stage3_sector;

        rc = dev_zero(dev, s3_lba, n < room ? n : room);
        if (rc != CLI_OK)
            return rc;
        log_info("cleared stage 3 from sector %u of the boot partition",
                 (unsigned)s->disk.stage3_sector);
    }
    if (s->disk.marker_sector != 0u) {
        rc = dev_zero(dev, s->disk.boot_start + s->disk.marker_sector, 1);
        if (rc != CLI_OK)
            return rc;
    }

    /* The boot code area goes to zero, which is what a disk no MBR loader
     * has touched looks like. The table and signature stay. */
    memset(mbr, 0, BIOS_MBR_CODE_LEN);
    rc = dev_write(dev, 0, mbr, dev->sector);
    if (rc == CLI_OK)
        log_info("cleared LBA 0's boot code on %s", d->path);
    return rc;
}

/* ---- the GPT bookkeeping entry ---- */

static cli_status reserve(const mks_disk *d, const bios_plan *p,
                          const bios_survey *s, int force)
{
    char range[64];
    char slot_type[64];
    char slot_name[64 + sizeof BIOS_BOOT_NAME];
    cli_status rc;

    if (s->have_reserve) {
        log_info("slot %u is already %s", (unsigned)s->reserve_slot,
                 BIOS_BOOT_NAME);
        return CLI_OK;
    }
    if (s->mounted && !force)
        return set_error(CLI_ERR_BUSY,
                         "%s has mounted partitions and --reserve rewrites "
                         "the partition table; the kernel will not reload it "
                         "until the next boot. Run it with the disk idle, or "
                         "--force", d->path);
    if (!proc_exists("sgdisk"))
        return set_error(CLI_ERR_NOTFOUND, "sgdisk is not on PATH");

    (void)snprintf(range, sizeof range, "%u:%llu:%llu",
                   (unsigned)BIOS_BOOT_SLOT,
                   (unsigned long long)p->stage2_lba,
                   (unsigned long long)(s->disk.first_part_lba - 1u));
    (void)snprintf(slot_type, sizeof slot_type, "%u:%s",
                   (unsigned)BIOS_BOOT_SLOT, BIOS_BOOT_TYPE);
    (void)snprintf(slot_name, sizeof slot_name, "%u:%s",
                   (unsigned)BIOS_BOOT_SLOT, BIOS_BOOT_NAME);

    /* -a 1 before -n, because sgdisk applies its options in order and its
     * default 2048-sector alignment would move the partition out of the very
     * gap it is meant to claim. */
    rc = proc_runl_quiet("sgdisk", "-a", "1", "-n", range, "-t", slot_type,
                         "-c", slot_name, d->path, PROC_END);
    if (rc != CLI_OK)
        return rc;
    log_info("slot %u now claims LBA %llu..%llu as %s",
             (unsigned)BIOS_BOOT_SLOT, (unsigned long long)p->stage2_lba,
             (unsigned long long)(s->disk.first_part_lba - 1u),
             BIOS_BOOT_NAME);
    return CLI_OK;
}

static cli_status unreserve(const mks_disk *d, const bios_survey *s, int force)
{
    char slot[16];
    cli_status rc;

    if (!s->have_reserve)
        return CLI_OK;
    if (s->mounted && !force) {
        log_warn("leaving slot %u in place: %s has mounted partitions",
                 (unsigned)s->reserve_slot, d->path);
        return CLI_OK;
    }
    if (!proc_exists("sgdisk"))
        return set_error(CLI_ERR_NOTFOUND, "sgdisk is not on PATH");

    /* The slot the reservation is in, not BIOS_BOOT_SLOT: a station's is
     * wherever install put it, and slot 5 there is the station partition. */
    (void)snprintf(slot, sizeof slot, "%u", (unsigned)s->reserve_slot);
    rc = proc_runl_quiet("sgdisk", "-d", slot, d->path, PROC_END);
    if (rc != CLI_OK)
        return rc;
    log_info("slot %u removed", (unsigned)s->reserve_slot);
    return CLI_OK;
}

/* ---- the command ---- */

static void describe_plan(const bios_plan *p, const bios_survey *s,
                          const mks_records *recs, size_t l2, size_t l3)
{
    const part_entry *bp;

    /* survey() refuses a disk with no boot partition before a plan exists. */
    if (s->boot < 0)
        return;
    bp = &recs->parts[s->boot];

    log_info("table:    %s, first partition at LBA %llu",
             s->disk.kind == PART_KIND_GPT ? "GPT" : "MBR",
             (unsigned long long)s->disk.first_part_lba);
    log_info("boot:     p%u at LBA %llu, %u reserved sectors", bp->number,
             (unsigned long long)bp->start_lba, (unsigned)s->disk.fat.reserved);
    log_info("stage 1:  LBA 0, bytes 0..%u (the partition table is kept)",
             (unsigned)(BIOS_MBR_CODE_LEN - 1u));
    log_info("stage 2:  LBA %llu..%llu, %zu byte(s)",
             (unsigned long long)p->stage2_lba,
             (unsigned long long)(p->stage2_lba + p->stage2_sectors - 1u), l2);
    log_info("stage 3:  LBA %llu..%llu (p%u sector %u), %zu byte(s)",
             (unsigned long long)p->stage3_lba,
             (unsigned long long)(p->stage3_lba + p->stage3_sectors - 1u),
             bp->number, (unsigned)s->disk.stage3_sector, l3);
    if (p->marker_lba != 0u)
        log_info("marker:   LBA %llu (p%u sector %u), zeroed",
                 (unsigned long long)p->marker_lba, bp->number,
                 (unsigned)s->disk.marker_sector);
}

cli_status mks_bios_install(const mks_bios_req *req)
{
    static mks_records recs;
    const char *target = req->target;
    const char *p1 = req->stage1 != NULL ? req->stage1 : DEFAULT_STAGE1;
    const char *p2 = req->stage2 != NULL ? req->stage2 : DEFAULT_STAGE2;
    const char *p3 = req->stage3 != NULL ? req->stage3 : DEFAULT_STAGE3;
    const char *backup = req->backup;
    char derived[MKS_PATH_MAX];
    void *b1 = NULL, *b2 = NULL, *b3 = NULL;
    size_t l1 = 0, l2 = 0, l3 = 0;
    uint8_t mbr[BIOS_SECTOR_MAX];
    bios_run runs[4];
    size_t nruns = 0;
    mks_disk d;
    mks_dev dev;
    bios_survey s;
    bios_plan plan;
    bioscore_status bs;
    cli_status rc;

    rc = mkstick_guid_selfcheck();
    if (rc != CLI_OK)
        return rc;

    /* --remove needs no stages: what it clears is read off the disk. */
    if (!req->remove) {
        rc = util_read_file(p1, &b1, &l1, STAGE_MAX);
        if (rc == CLI_OK)
            rc = util_read_file(p2, &b2, &l2, STAGE_MAX);
        if (rc == CLI_OK)
            rc = util_read_file(p3, &b3, &l3, STAGE_MAX);
        if (rc != CLI_OK)
            goto out;
        bs = bioscore_check_stage1((const uint8_t *)b1, l1);
        if (bs != BIOSCORE_OK) {
            rc = set_error(CLI_ERR_INVALID, "%s: %s", p1,
                           bioscore_strerror(bs));
            goto out;
        }
    }

    rc = disk_stat(&d, target);
    if (rc != CLI_OK)
        goto out;
    disk_describe(&d);
    if (d.sector != BIOS_SECTOR_BYTES && d.sector != BIOS_SECTOR_MAX) {
        rc = set_error(CLI_ERR_INVALID, "%s: %s", d.path,
                       bioscore_strerror(BIOSCORE_ERR_SECTOR));
        goto out;
    }

    dev.sector = d.sector;
    dev.fd = open(d.path, (req->dry_run ? O_RDONLY : O_RDWR) | O_CLOEXEC);
    if (dev.fd < 0) {
        rc = set_error_errno(CLI_ERR_IO, "opening %s", d.path);
        goto out;
    }

    rc = survey(&dev, &d, &recs, &s);
    if (rc != CLI_OK)
        goto close_out;
    rc = dev_read(&dev, 0, 1, mbr);
    if (rc != CLI_OK)
        goto close_out;

    /* The default name is the target's, so two disks cannot quietly share
     * one backup file. */
    if (backup == NULL && !req->no_backup) {
        (void)snprintf(derived, sizeof derived, "falcon-bios-backup-%s.bin",
                       util_basename(d.path));
        backup = derived;
    }

    if (req->remove) {
        if (req->dry_run) {
            log_info("--dry-run: nothing written");
            goto close_out;
        }
        rc = unreserve(&d, &s, req->force);
        if (rc != CLI_OK)
            goto close_out;
        if (backup != NULL && util_is_file(backup))
            rc = backup_restore(&dev, backup);
        else
            rc = clear_install(&dev, &d, &s, mbr);
        goto sync_out;
    }

    if (s.disk.kind == PART_KIND_MBR && req->reserve) {
        rc = set_error(CLI_ERR_USAGE,
                       "--reserve claims the gap with a GPT partition; %s is "
                       "MBR", d.path);
        goto close_out;
    }

    bs = bioscore_plan(&s.disk, STAGE2_LBA, STAGE2_LBA_MBR, STAGE2_SECTORS,
                       l2, l3, &plan);
    if (bs != BIOSCORE_OK) {
        rc = set_error(CLI_ERR_RANGE, "%s: %s", d.path, bioscore_strerror(bs));
        if (bs == BIOSCORE_ERR_RESERVED)
            log_error("stage 3 is %zu byte(s); the boot partition has %u "
                      "reserved sectors from sector %u (reformat it with "
                      "mkfs.fat -R %u, or remake the stick)", l3,
                      (unsigned)s.disk.fat.reserved,
                      (unsigned)s.disk.stage3_sector,
                      (unsigned)bioscore_reserved_for(s.disk.stage3_sector,
                                                      l3));
        goto close_out;
    }
    describe_plan(&plan, &s, &recs, l2, l3);
    if (s.mounted)
        log_info("note:     %s has mounted partitions; none of them cover "
                 "these sectors", d.path);

    /* Say what is being replaced. A disk that has carried another loader
     * still has its boot code here, and replacing that silently is the
     * difference between a tool and a surprise. */
    if (bioscore_is_installed(mbr, (const uint8_t *)b1, STAGE1_S2_LBA_OFF,
                              STAGE1_S2_SECTORS_OFF))
        log_info("note:     LBA 0 already carries this stage 1");
    else if (mbr_has_code(mbr))
        log_warn("LBA 0 carries boot code that is not ours; it is being "
                 "replaced");

    if (req->dry_run) {
        log_info("--dry-run: nothing written");
        goto close_out;
    }

    runs[nruns].lba = 0;
    runs[nruns++].sectors = 1;
    runs[nruns].lba = plan.stage2_lba;
    runs[nruns++].sectors = plan.stage2_sectors;
    runs[nruns].lba = plan.stage3_lba;
    runs[nruns++].sectors = plan.stage3_sectors;
    if (plan.marker_lba != 0u) {
        runs[nruns].lba = plan.marker_lba;
        runs[nruns++].sectors = 1;
    }
    if (backup != NULL) {
        rc = backup_write(&dev, backup, runs, nruns, req->force);
        if (rc != CLI_OK)
            goto close_out;
    }

    rc = dev_write_stage(&dev, plan.stage2_lba, (const uint8_t *)b2, l2,
                         plan.stage2_sectors);
    if (rc == CLI_OK)
        rc = dev_write_stage(&dev, plan.stage3_lba, (const uint8_t *)b3, l3,
                             plan.stage3_sectors);
    if (rc == CLI_OK && plan.marker_lba != 0u)
        rc = dev_zero(&dev, plan.marker_lba, 1);
    if (rc != CLI_OK)
        goto close_out;

    /* LBA 0 last. See the file header. */
    bs = bioscore_splice_mbr(mbr, (const uint8_t *)b1, STAGE1_S2_LBA_OFF,
                             STAGE1_S2_SECTORS_OFF, plan.stage2_lba,
                             plan.stage2_sectors, mbr);
    if (bs != BIOSCORE_OK) {
        rc = set_error(CLI_ERR_INVALID, "%s: %s", d.path,
                       bioscore_strerror(bs));
        goto close_out;
    }
    rc = dev_write(&dev, 0, mbr, dev.sector);
    if (rc != CLI_OK)
        goto close_out;

    if (req->reserve) {
        /* sgdisk opens the disk itself, so our writes have to be on the
         * platter before it reads the table it is about to rewrite. */
        if (fsync(dev.fd) != 0) {
            rc = set_error_errno(CLI_ERR_IO, "flushing %s", d.path);
            goto close_out;
        }
        rc = reserve(&d, &plan, &s, req->force);
        if (rc != CLI_OK)
            goto close_out;
    }
    log_info("%s boots falcon-bios on a BIOS machine", d.path);

sync_out:
    if (rc == CLI_OK && fsync(dev.fd) != 0)
        rc = set_error_errno(CLI_ERR_IO, "flushing %s", d.path);
close_out:
    (void)close(dev.fd);
out:
    free(b1);
    free(b2);
    free(b3);
    return rc;
}

static cli_status bios_cmd_run(const cli_args *a)
{
    mks_bios_req req;

    memset(&req, 0, sizeof req);
    req.target = a->pos[0];
    req.stage1 = cli_value(a, "stage1", DEFAULT_STAGE1);
    req.stage2 = cli_value(a, "stage2", DEFAULT_STAGE2);
    req.stage3 = cli_value(a, "stage3", DEFAULT_STAGE3);
    req.backup = cli_value(a, "backup", NULL);
    req.no_backup = cli_flag(a, "no-backup");
    req.remove = cli_flag(a, "remove");
    req.reserve = cli_flag(a, "reserve");
    req.dry_run = cli_flag(a, "dry-run");
    req.force = cli_flag(a, "force");
    return mks_bios_install(&req);
}

const cli_cmd mks_cmd_bios = {
    "bios",
    "install falcon-bios: LBA 0, stage 2's gap, the boot partition",
    "<target>",
    bios_opts,
    sizeof bios_opts / sizeof bios_opts[0],
    1, 1,
    "Writes stage 1 into the boot code of LBA 0 (the partition table is\n"
    "kept), stage 2 after LBA 0 and before the first partition (LBA 34 on\n"
    "GPT, LBA 1 on MBR), and stage 3 into the reserved sectors of the\n"
    "partition whose boot sector carries the `boot` identity record, at the\n"
    "sector the record names; the next-boot marker sector it names is\n"
    "zeroed. Partitions, filesystems and files are untouched.\n"
    "\n"
    "Every sector it writes is saved first, to --backup, and --remove puts\n"
    "them back; without a backup, --remove clears what an install wrote.\n"
    "--reserve additionally claims stage 2's gap as a GPT BIOS boot\n"
    "partition, so a later partitioning tool sees it as taken.\n"
    "\n"
    "A BIOS machine has no Secure Boot, no shim and no db, so the BIOS route\n"
    "has no blessed path.\n",
    bios_cmd_run
};
