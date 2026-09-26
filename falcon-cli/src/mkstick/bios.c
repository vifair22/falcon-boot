/* falcon-boot mkstick: `bios`, installing falcon-bios onto a stick that
 * already exists (docs/notes/bios-route.md Section 7.3).
 *
 * The whole point of this command is that it does not remake anything. A
 * falcon stick is four partitions starting at LBA 2048, and the sectors below
 * that - the 440 bytes of boot code in LBA 0, and the run between the end of
 * the GPT entry array and the first partition - are already unallocated on
 * every stick ever written. Adding the BIOS route is therefore a write to
 * space nobody owns, not the breaking layout change Section 5.4 assumed, and
 * a stick keeps its partitions, its filesystems, its LUKS volumes and its
 * PARTUUIDs across it.
 *
 * It is opt-in, and that is the decision rather than a stub. Installing stage
 * 1 makes a stick bootable on machines that ignore it today, and a BIOS
 * machine has no Secure Boot, no shim and no db - there is nothing there to
 * verify with, and the BIOS route has no blessed path for that reason. That
 * is a change in what a stick can reach, so somebody asks for it.
 *
 * ORDER OF WRITES IS LOAD BEARING. The stages go down first and LBA 0 last,
 * so an install interrupted at any point leaves a disk whose boot sector
 * either is not ours or points at stages that are already there. The reverse
 * order has a window where stage 1 sends the machine to whatever the gap used
 * to hold.
 */

#include <mkstick/mkstick.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <part/part.h>

#include <common/log.h>
#include <common/proc.h>
#include <common/util.h>
#include <mkstick/bioscore.h>
#include <mkstick/disk.h>

/* The loader's own addresses, read from the header that fixes them rather
 * than written a second time here. falcon-bios/build.mk reads the same file
 * with sed to place the rig image; this is the third reader and the only one
 * that ever touches real media. */
#include <falcon-bios/src/start/biosdefs.h>

#define DEFAULT_STAGE1 "build/bin/falcon-bios-stage1.bin"
#define DEFAULT_STAGE2 "build/bin/falcon-bios-stage2.bin"
#define DEFAULT_STAGE3 "build/bin/falcon-bios-stage3.bin"

/* Big enough for the largest stage, checked against the plan before use.
 * size_t on the first term so the product is computed at the width it is
 * consumed at rather than in int and widened afterwards. */
#define STAGE_MAX ((size_t)4 * 1024 * 1024)

typedef struct {
    int      fd;
    uint32_t sector;
} bios_dev;

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
      "undo an install: restore --backup, or clear the sectors" },
    { "reserve", CLI_OPT_FLAG, 0, NULL,
      "also claim the gap as a GPT BIOS boot partition (slot "
      "5, type EF02); rewrites the partition table" },
    { "dry-run", CLI_OPT_FLAG, 0, NULL,
      "report the plan and write nothing" },
    { "force", CLI_OPT_FLAG, 0, NULL,
      "overwrite an existing backup file, and allow --reserve on a disk "
      "with mounted partitions" }
};

/* ---- byte I/O over the target ---- */

static cli_status dev_read(const bios_dev *dev, uint64_t lba, uint64_t sectors,
                           uint8_t *buf)
{
    uint64_t want = sectors * dev->sector;
    uint64_t done = 0;

    /* A zero-length read would return success having left the caller's
     * buffer untouched, which is the one failure here that looks like data.
     * Every call site asks for a fixed, nonzero count, so this is a bug
     * report rather than a condition.
     *
     * Written as a statement and a literal return rather than the repo's
     * usual `return set_error(...)` because set_error is variadic and
     * clang-tidy does not track what it hands back: through the idiomatic
     * form the analyzer walks straight on into the caller with the buffer
     * still uninitialised and reports it reading garbage. */
    if (want == 0u) {
        (void)set_error(CLI_ERR_INTERNAL, "a read of no sectors at LBA %llu",
                        (unsigned long long)lba);
        return CLI_ERR_INTERNAL;
    }

    while (done < want) {
        ssize_t n = pread(dev->fd, buf + done, (size_t)(want - done),
                          (off_t)(lba * dev->sector + done));

        if (n < 0)
            return set_error_errno(CLI_ERR_IO, "reading LBA %llu",
                                   (unsigned long long)lba);
        if (n == 0)
            return set_error(CLI_ERR_IO, "short read at LBA %llu",
                             (unsigned long long)lba);
        done += (uint64_t)n;
    }
    return CLI_OK;
}

static cli_status dev_write(const bios_dev *dev, uint64_t lba,
                            const uint8_t *buf, uint64_t len)
{
    uint64_t done = 0;

    while (done < len) {
        ssize_t n = pwrite(dev->fd, buf + done, (size_t)(len - done),
                           (off_t)(lba * dev->sector + done));

        if (n <= 0)
            return set_error_errno(CLI_ERR_IO, "writing LBA %llu",
                                   (unsigned long long)lba);
        done += (uint64_t)n;
    }
    return CLI_OK;
}

/* One sector run set to zero. Used for the reserved tail of each stage: the
 * gap on a stick that has lived is rarely empty (this one held 444 KiB of a
 * previous loader's core image), and stage 1 reads the whole reserved run
 * whatever is in it. */
static cli_status dev_zero(const bios_dev *dev, uint64_t lba, uint64_t sectors)
{
    uint8_t buf[64u * 1024u];
    uint64_t left = sectors * dev->sector;
    uint64_t at = lba;
    cli_status rc;

    memset(buf, 0, sizeof buf);
    while (left > 0u) {
        uint64_t use = (left < sizeof buf) ? left : sizeof buf;

        rc = dev_write(dev, at, buf, use);
        if (rc != CLI_OK)
            return rc;
        at += use / dev->sector;
        left -= use;
    }
    return CLI_OK;
}

static int dev_gpt_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const bios_dev *dev = (const bios_dev *)ctx;
    uint64_t done = 0;

    while (done < len) {
        ssize_t n = pread(dev->fd, (uint8_t *)buf + done, (size_t)(len - done),
                          (off_t)(off + done));

        if (n <= 0)
            return -1;
        done += (uint64_t)n;
    }
    return 0;
}

/* Is there anything in LBA 0's boot code area? A GPT disk that has never
 * been booted by an MBR has zeros here; anything else is somebody's loader. */
static int mbr_has_code(const uint8_t *mbr)
{
    size_t i;

    for (i = 0; i < BIOS_MBR_CODE_LEN; i++) {
        if (mbr[i] != 0u)
            return 1;
    }
    return 0;
}

/* ---- the disk's own geometry ---- */

typedef struct {
    uint64_t first_usable;
    uint64_t first_part_lba;   /* 0 when the table has no partitions */
    int      have_ours;        /* a partition in our slot, wearing our name */
    int      mounted;
} bios_survey;

static cli_status survey(bios_dev *dev, const mks_disk *d,
                         bios_survey *out)
{
    gpt_part parts[128];
    gpt_table t;
    int rc;
    uint32_t i;

    memset(out, 0, sizeof *out);
    memset(&t, 0, sizeof t);
    t.parts = parts;
    t.cap = (uint32_t)(sizeof parts / sizeof parts[0]);

    rc = gpt_parse(&t, dev_gpt_read, dev, dev->sector);
    if (rc != GPT_OK)
        return set_error(CLI_ERR_INVALID,
                         "%s has no usable GPT (gpt_parse %d); falcon-bios "
                         "goes in the gap a GPT leaves, so this command has "
                         "nothing to measure against",
                         d->path, rc);
    /* The gap is bounded below by the GPT and above by the first partition.
     * A table with nothing in it has no upper bound to be safe against, and
     * is not a falcon stick either way. */
    if (t.count == 0u)
        return set_error(CLI_ERR_INVALID,
                         "%s has an empty partition table", d->path);

    out->first_usable = t.first_usable;
    for (i = 0; i < t.count; i++) {
        /* Our own reservation is not something to be protected from: it
         * covers the gap precisely because the stages go there, so a second
         * run with --reserve already applied must measure against the first
         * REAL partition and not against itself. */
        if (bioscore_is_reserve(&parts[i])) {
            out->have_ours = 1;
            continue;
        }
        if (out->first_part_lba == 0u || parts[i].start_lba < out->first_part_lba)
            out->first_part_lba = parts[i].start_lba;
    }
    if (out->first_part_lba == 0u)
        return set_error(CLI_ERR_INVALID,
                         "%s has no partition above the gap", d->path);
    out->mounted = (disk_check_not_mounted(d) != CLI_OK);
    cli_error_clear();
    return CLI_OK;
}

/* ---- backup ---- */

static cli_status backup_write(const bios_dev *dev, const char *path,
                               const bios_plan *p, int force)
{
    uint8_t hdr[BIOS_BACKUP_HDR];
    uint8_t *gap = NULL;
    uint8_t mbr[BIOS_SECTOR_BYTES];
    uint64_t sectors = p->end_lba - p->stage2_lba;
    size_t gap_bytes;
    int fd;
    cli_status rc;

    if (!force && util_is_file(path))
        return set_error(CLI_ERR_EXISTS,
                         "%s exists; it holds what was on this disk before an "
                         "earlier install, and a second run would replace it "
                         "with what that install wrote. Name another file, or "
                         "--force", path);

    (void)bioscore_backup_header(hdr, dev->sector, p->stage2_lba, sectors);

    rc = dev_read(dev, 0, 1, mbr);
    if (rc != CLI_OK)
        return rc;

    /* bioscore_plan reserves sectors for both stages and disk_stat refuses a
     * sector size below 512, so this cannot fire; it is here because the
     * alternative is a zero-length allocation whose return value may be NULL
     * or may be a pointer that must still be freed, and neither branch below
     * is written for that. */
    gap_bytes = (size_t)sectors * dev->sector;
    if (gap_bytes == 0u)
        return set_error(CLI_ERR_INTERNAL, "the plan covers no sectors");
    gap = malloc(gap_bytes);
    if (gap == NULL)
        return set_error(CLI_ERR_NOMEM, "no memory for the backup");
    rc = dev_read(dev, p->stage2_lba, sectors, gap);
    if (rc != CLI_OK) {
        free(gap);
        return rc;
    }

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        free(gap);
        return set_error_errno(CLI_ERR_IO, "creating %s", path);
    }
    if (write(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr ||
        write(fd, mbr, sizeof mbr) != (ssize_t)sizeof mbr ||
        write(fd, gap, gap_bytes) != (ssize_t)gap_bytes) {
        (void)close(fd);
        free(gap);
        return set_error_errno(CLI_ERR_IO, "writing %s", path);
    }
    free(gap);
    if (fsync(fd) != 0 || close(fd) != 0)
        return set_error_errno(CLI_ERR_IO, "flushing %s", path);

    log_info("saved %llu sector(s) to %s",
             (unsigned long long)(sectors + 1u), path);
    return CLI_OK;
}

static cli_status backup_restore(const bios_dev *dev, const char *path)
{
    uint8_t hdr[BIOS_BACKUP_HDR];
    uint8_t mbr[BIOS_SECTOR_BYTES];
    uint8_t *gap = NULL;
    uint32_t sector_bytes = 0;
    uint64_t gap_lba = 0, gap_sectors = 0;
    size_t bytes;
    bioscore_status bs;
    cli_status rc = CLI_OK;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return set_error_errno(CLI_ERR_NOTFOUND, "opening %s", path);
    if (read(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) {
        (void)close(fd);
        return set_error(CLI_ERR_INVALID, "%s is too short to be a backup",
                         path);
    }
    bs = bioscore_backup_parse(hdr, sizeof hdr, &sector_bytes, &gap_lba,
                               &gap_sectors);
    if (bs != BIOSCORE_OK) {
        (void)close(fd);
        return set_error(CLI_ERR_INVALID, "%s: %s", path,
                         bioscore_strerror(bs));
    }
    if (sector_bytes != dev->sector) {
        (void)close(fd);
        return set_error(CLI_ERR_INVALID,
                         "%s was taken from a disk with %u-byte sectors, not "
                         "%u", path, sector_bytes, dev->sector);
    }

    /* LBA 0 goes back whole, which is the one place a restore differs from an
     * install: an install splices because it must not disturb the protective
     * MBR, while a restore is putting that exact sector back. */
    if (read(fd, mbr, sizeof mbr) != (ssize_t)sizeof mbr) {
        (void)close(fd);
        return set_error(CLI_ERR_INVALID, "%s has no boot sector in it", path);
    }
    /* bioscore_backup_parse refuses a zero in either field, so this cannot
     * fire; it is here because a zero-length malloc may return NULL or a
     * pointer that must still be freed, and neither branch below is written
     * for that. */
    bytes = (size_t)gap_sectors * sector_bytes;
    if (bytes == 0u) {
        (void)close(fd);
        return set_error(CLI_ERR_INVALID, "%s covers no sectors", path);
    }
    gap = malloc(bytes);
    if (gap == NULL) {
        (void)close(fd);
        return set_error(CLI_ERR_NOMEM, "no memory to read %s", path);
    }
    if (read(fd, gap, bytes) != (ssize_t)bytes) {
        (void)close(fd);
        free(gap);
        return set_error(CLI_ERR_INVALID, "%s is shorter than its header says",
                         path);
    }
    (void)close(fd);

    rc = dev_write(dev, gap_lba, gap, bytes);
    free(gap);
    if (rc != CLI_OK)
        return rc;
    rc = dev_write(dev, 0, mbr, sizeof mbr);
    if (rc != CLI_OK)
        return rc;

    log_info("restored LBA 0 and %llu sector(s) from LBA %llu",
             (unsigned long long)gap_sectors, (unsigned long long)gap_lba);
    return CLI_OK;
}

/* ---- the GPT bookkeeping entry ---- */

static cli_status reserve(const mks_disk *d, const bios_plan *p,
                          const bios_survey *s, int force)
{
    char range[64];
    char slot_type[64];
    char slot_name[64 + sizeof BIOS_BOOT_NAME];
    cli_status rc;

    if (s->have_ours) {
        log_info("slot %u is already %s", (unsigned)BIOS_BOOT_SLOT,
                 BIOS_BOOT_NAME);
        return CLI_OK;
    }
    if (s->mounted && !force)
        return set_error(CLI_ERR_BUSY,
                         "%s has mounted partitions and --reserve rewrites "
                         "the partition table; the kernel will not reload it "
                         "until the next boot. Run it with the stick idle, or "
                         "--force", d->path);
    if (!proc_exists("sgdisk"))
        return set_error(CLI_ERR_NOTFOUND, "sgdisk is not on PATH");

    (void)snprintf(range, sizeof range, "%u:%llu:%llu",
                   (unsigned)BIOS_BOOT_SLOT,
                   (unsigned long long)p->stage2_lba,
                   (unsigned long long)(s->first_part_lba - 1u));
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
             (unsigned long long)(s->first_part_lba - 1u), BIOS_BOOT_NAME);
    return CLI_OK;
}

static cli_status unreserve(const mks_disk *d, const bios_survey *s, int force)
{
    char slot[16];
    cli_status rc;

    if (!s->have_ours)
        return CLI_OK;
    if (s->mounted && !force) {
        log_warn("leaving slot %u in place: %s has mounted partitions",
                 (unsigned)BIOS_BOOT_SLOT, d->path);
        return CLI_OK;
    }
    if (!proc_exists("sgdisk"))
        return set_error(CLI_ERR_NOTFOUND, "sgdisk is not on PATH");

    (void)snprintf(slot, sizeof slot, "%u", (unsigned)BIOS_BOOT_SLOT);
    rc = proc_runl_quiet("sgdisk", "-d", slot, d->path, PROC_END);
    if (rc != CLI_OK)
        return rc;
    log_info("slot %u removed", (unsigned)BIOS_BOOT_SLOT);
    return CLI_OK;
}

/* ---- the command ---- */

static void describe_plan(const bios_plan *p, const bios_survey *s,
                          size_t s2, size_t s3)
{
    log_info("GPT:      first usable LBA %llu, first partition at LBA %llu",
             (unsigned long long)s->first_usable,
             (unsigned long long)s->first_part_lba);
    log_info("gap:      LBA %llu..%llu, %llu sector(s)",
             (unsigned long long)s->first_usable,
             (unsigned long long)(s->first_part_lba - 1u),
             (unsigned long long)(s->first_part_lba - s->first_usable));
    log_info("stage 1:  LBA 0, bytes 0..%u (the partition table is kept)",
             (unsigned)(BIOS_MBR_CODE_LEN - 1u));
    log_info("stage 2:  LBA %llu, %zu byte(s) in %llu reserved sector(s)",
             (unsigned long long)p->stage2_lba, s2,
             (unsigned long long)p->stage2_sectors);
    log_info("stage 3:  LBA %llu, %zu byte(s) in %llu reserved sector(s)",
             (unsigned long long)p->stage3_lba, s3,
             (unsigned long long)p->stage3_sectors);
}

cli_status mks_bios_install(const mks_bios_req *req)
{
    const char *target = req->target;
    const char *p1 = req->stage1 != NULL ? req->stage1 : DEFAULT_STAGE1;
    const char *p2 = req->stage2 != NULL ? req->stage2 : DEFAULT_STAGE2;
    const char *p3 = req->stage3 != NULL ? req->stage3 : DEFAULT_STAGE3;
    const char *backup = req->backup;
    int removing = req->remove;
    int dry = req->dry_run;
    int force = req->force;
    char derived[MKS_PATH_MAX];
    void *b1 = NULL, *b2 = NULL, *b3 = NULL;
    size_t l1 = 0, l2 = 0, l3 = 0;
    uint8_t mbr[BIOS_SECTOR_BYTES];
    mks_disk d;
    bios_dev dev;
    bios_survey s;
    bios_plan plan;
    bioscore_status bs;
    cli_status rc;

    rc = mkstick_guid_selfcheck();
    if (rc != CLI_OK)
        return rc;

    bs = bioscore_plan(STAGE2_LBA, STAGE2_SECTORS, STAGE3_MAX_SECTORS, &plan);
    if (bs != BIOSCORE_OK)
        return set_error(CLI_ERR_INTERNAL,
                         "falcon-bios/src/start/biosdefs.h describes a layout that "
                         "cannot be written: %s", bioscore_strerror(bs));

    rc = util_read_file(p1, &b1, &l1, STAGE_MAX);
    if (rc != CLI_OK)
        goto out;
    rc = util_read_file(p2, &b2, &l2, STAGE_MAX);
    if (rc != CLI_OK)
        goto out;
    rc = util_read_file(p3, &b3, &l3, STAGE_MAX);
    if (rc != CLI_OK)
        goto out;

    bs = bioscore_check_images((const uint8_t *)b1, l1, l2, l3, &plan);
    if (bs != BIOSCORE_OK) {
        rc = set_error(CLI_ERR_INVALID, "%s", bioscore_strerror(bs));
        goto out;
    }

    rc = disk_stat(&d, target);
    if (rc != CLI_OK)
        goto out;
    disk_describe(&d);

    dev.sector = d.sector;
    dev.fd = open(d.path, O_RDWR | O_CLOEXEC);
    if (dev.fd < 0) {
        rc = set_error_errno(CLI_ERR_IO, "opening %s for writing", d.path);
        goto out;
    }

    rc = survey(&dev, &d, &s);
    if (rc != CLI_OK)
        goto close_out;

    bs = bioscore_check_gap(&plan, s.first_usable, s.first_part_lba);
    if (bs != BIOSCORE_OK) {
        rc = set_error(CLI_ERR_RANGE,
                       "%s: %s (the stages want LBA %llu..%llu; this disk has "
                       "LBA %llu..%llu free)", d.path, bioscore_strerror(bs),
                       (unsigned long long)plan.stage2_lba,
                       (unsigned long long)(plan.end_lba - 1u),
                       (unsigned long long)s.first_usable,
                       (unsigned long long)(s.first_part_lba - 1u));
        goto close_out;
    }

    rc = dev_read(&dev, 0, 1, mbr);
    if (rc != CLI_OK)
        goto close_out;

    describe_plan(&plan, &s, l2, l3);
    if (s.mounted)
        log_info("note:     %s has mounted partitions; none of them cover "
                 "these sectors", d.path);

    /* Say what is being replaced. A stick that has carried another loader
     * still has its boot code here - the author's had GRUB's, from a
     * previous life as a Ventoy stick - and replacing that silently is the
     * difference between a tool and a surprise. */
    if (bioscore_is_installed(mbr, (const uint8_t *)b1))
        log_info("note:     LBA 0 already carries this stage 1");
    else if (mbr_has_code(mbr))
        log_warn("LBA 0 carries boot code that is not ours; it is being "
                 "replaced");

    /* The default name is the target's, so two sticks cannot quietly share
     * one backup file. */
    if (backup == NULL && !req->no_backup) {
        (void)snprintf(derived, sizeof derived, "falcon-bios-backup-%s.bin",
                       util_basename(d.path));
        backup = derived;
    }

    if (dry) {
        log_info("--dry-run: nothing written");
        goto close_out;
    }

    if (removing) {
        rc = unreserve(&d, &s, force);
        if (rc != CLI_OK)
            goto close_out;
        if (backup != NULL && util_is_file(backup)) {
            rc = backup_restore(&dev, backup);
            goto sync_out;
        }
        /* No backup to put back: clear what an install owns. The boot code
         * area goes to zero rather than to anything else, because that is
         * what a GPT disk that has never been BIOS-booted looks like. */
        memset(mbr, 0, BIOS_MBR_CODE_LEN);
        rc = dev_write(&dev, 0, mbr, sizeof mbr);
        if (rc != CLI_OK)
            goto close_out;
        rc = dev_zero(&dev, plan.stage2_lba, plan.end_lba - plan.stage2_lba);
        if (rc == CLI_OK)
            log_info("cleared LBA 0's boot code and LBA %llu..%llu",
                     (unsigned long long)plan.stage2_lba,
                     (unsigned long long)(plan.end_lba - 1u));
        goto sync_out;
    }

    if (backup != NULL) {
        rc = backup_write(&dev, backup, &plan, force);
        if (rc != CLI_OK)
            goto close_out;
    }

    /* Zero first: the reserved run is read whole by the stage above it, and
     * on a stick that has carried another loader it is full of that loader. */
    rc = dev_zero(&dev, plan.stage2_lba, plan.end_lba - plan.stage2_lba);
    if (rc != CLI_OK)
        goto close_out;
    rc = dev_write(&dev, plan.stage2_lba, (const uint8_t *)b2, l2);
    if (rc != CLI_OK)
        goto close_out;
    rc = dev_write(&dev, plan.stage3_lba, (const uint8_t *)b3, l3);
    if (rc != CLI_OK)
        goto close_out;

    /* LBA 0 last. See the file header. Nothing written above touches this
     * sector, so the copy read before the writes is still what is there. */
    bs = bioscore_splice_mbr(mbr, (const uint8_t *)b1, mbr);
    if (bs != BIOSCORE_OK) {
        rc = set_error(CLI_ERR_INVALID, "%s: %s", d.path,
                       bioscore_strerror(bs));
        goto close_out;
    }
    rc = dev_write(&dev, 0, mbr, sizeof mbr);
    if (rc != CLI_OK)
        goto close_out;

    if (req->reserve) {
        if (fsync(dev.fd) != 0) {
            rc = set_error_errno(CLI_ERR_IO, "flushing %s", d.path);
            goto close_out;
        }
        /* sgdisk opens the disk itself, so our writes have to be on the
         * platter before it reads the table it is about to rewrite. */
        rc = reserve(&d, &plan, &s, force);
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

static cli_status bios_run(const cli_args *a)
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
    "install falcon-bios into the sectors a GPT stick leaves unallocated",
    "<target>",
    bios_opts,
    sizeof bios_opts / sizeof bios_opts[0],
    1, 1,
    "Writes three things and remakes nothing: stage 1 into the boot code at\n"
    "the head of LBA 0, and stages 2 and 3 into the sectors between the GPT\n"
    "entry array and the first partition. Partitions, filesystems, LUKS\n"
    "volumes and PARTUUIDs are untouched, so a stick keeps working exactly as\n"
    "it did under UEFI.\n"
    "\n"
    "Every sector it is about to write is saved first, to --backup, and\n"
    "--remove puts them back. --reserve additionally claims the gap as a GPT\n"
    "BIOS boot partition; nothing reads it, and it is there so a later\n"
    "partitioning tool sees the space as taken.\n"
    "\n"
    "A BIOS machine has no Secure Boot, no shim and no db, so the BIOS route\n"
    "has no blessed path. Installing this makes the stick bootable on\n"
    "machines that ignore it today.\n",
    bios_run
};
