/* falcon-boot mkstick: the next-boot marker sector on a disk. See nextio.h.
 */

#include <mkstick/nextio.h>

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <common/log.h>
#include <mkstick/nextcore.h>
#include <mkstick/records.h>

int nextio_probe(const char *path, uint32_t sector, uint64_t size,
                 next_target *out)
{
    static mks_records r;
    int boot;

    if (path == NULL || out == NULL || !nextboot_sector_size_ok(sector))
        return 0;
    if (records_read(&r, path, sector, size) != CLI_OK) {
        cli_error_clear();
        return 0;
    }
    boot = records_boot(&r);
    if (boot < 0 || r.recs[boot].rec.marker_sector == 0u)
        return 0;
    memset(out, 0, sizeof *out);
    (void)snprintf(out->path, sizeof out->path, "%s", path);
    out->sector = sector;
    out->marker_lba = r.parts[boot].start_lba +
                      r.recs[boot].rec.marker_sector;
    return 1;
}

int nextio_probe_path(const char *path, next_target *out)
{
    mks_disk d;

    if (disk_stat(&d, path) != CLI_OK) {
        cli_error_clear();
        return 0;
    }
    return nextio_probe(d.path, d.sector, d.size, out);
}

cli_status nextio_resolve(const char *disk, const char *const *cands,
                          size_t ncands, next_target *out)
{
    next_target first;
    next_target t;
    size_t found = 0;
    size_t i;

    /* A named disk is the answer or an error. Looking on past it would
     * write a marker to a disk nobody named, on a machine where the named
     * one was simply mistyped. */
    if (disk != NULL) {
        if (!nextio_probe_path(disk, out))
            return set_error(CLI_ERR_NOTFOUND,
                             "%s carries no falcon boot partition with a "
                             "marker sector", disk);
        return CLI_OK;
    }

    memset(&first, 0, sizeof first);
    for (i = 0; i < ncands; i++) {
        if (!nextio_probe_path(cands[i], &t))
            continue;
        if (found == 0u)
            first = t;
        else if (found == 1u)
            log_error("a boot partition is on %s", first.path);
        if (found != 0u)
            log_error("a boot partition is on %s", t.path);
        found++;
    }
    if (found == 0u)
        return set_error(CLI_ERR_NOTFOUND,
                         "no disk carries a falcon boot partition with a "
                         "marker sector%s; name one with --disk",
                         geteuid() != 0 ? " that this user can read" : "");
    if (found > 1u)
        return set_error(CLI_ERR_USAGE,
                         "%zu disks carry a falcon boot partition; name one "
                         "with --disk", found);
    *out = first;
    return CLI_OK;
}

static cli_status read_sector(int fd, const next_target *t, uint8_t *buf)
{
    off_t off = (off_t)(t->marker_lba * t->sector);

    if (pread(fd, buf, t->sector, off) != (ssize_t)t->sector)
        return set_error_errno(CLI_ERR_IO, "reading LBA %" PRIu64 " of %s",
                               t->marker_lba, t->path);
    return CLI_OK;
}

static int sector_ok(const next_target *t)
{
    return t != NULL && nextboot_sector_size_ok(t->sector) &&
           t->marker_lba <= (uint64_t)INT64_MAX / t->sector;
}

cli_status nextio_read(const next_target *t, next_queued *q)
{
    static uint8_t sec[NEXTBOOT_SECTOR_MAX];
    cli_status rc;
    int fd;

    if (q == NULL || !sector_ok(t))
        return set_error(CLI_ERR_INTERNAL, "no marker sector to read");
    memset(q, 0, sizeof *q);
    fd = open(t->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s", t->path);
    rc = read_sector(fd, t, sec);
    (void)close(fd);
    if (rc != CLI_OK)
        return rc;
    q->state = nextboot_sector_parse(sec, t->sector, &q->stamp, q->value,
                                     sizeof q->value);
    return CLI_OK;
}

cli_status nextio_write(const next_target *t, const char *value,
                        uint64_t now)
{
    static uint8_t cur[NEXTBOOT_SECTOR_MAX];
    static uint8_t sec[NEXTBOOT_SECTOR_MAX];
    static uint8_t back[NEXTBOOT_SECTOR_MAX];
    off_t off;
    cli_status rc;
    int fd;

    if (!sector_ok(t))
        return set_error(CLI_ERR_INTERNAL, "no marker sector to write");
    memset(sec, 0, t->sector);
    if (value != NULL &&
        nextboot_sector_write(sec, t->sector, now, value) != NEXTBOOT_OK)
        return set_error(CLI_ERR_INVALID,
                         "'%s' is not a next-boot value: printable ASCII, "
                         "a path on p2 or a reserved name such as %s",
                         value, NEXTBOOT_PERSONAL);

    fd = open(t->path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s for writing",
                               t->path);
    rc = read_sector(fd, t, cur);
    if (rc != CLI_OK) {
        (void)close(fd);
        return rc;
    }
    if (!nextcore_sector_ours(cur, t->sector)) {
        (void)close(fd);
        return set_error(CLI_ERR_INVALID,
                         "LBA %" PRIu64 " of %s is neither empty nor a "
                         "next-boot marker; left alone", t->marker_lba,
                         t->path);
    }
    off = (off_t)(t->marker_lba * t->sector);
    if (pwrite(fd, sec, t->sector, off) != (ssize_t)t->sector ||
        fsync(fd) != 0) {
        (void)close(fd);
        return set_error_errno(CLI_ERR_IO, "writing %s", t->path);
    }
    rc = read_sector(fd, t, back);
    (void)close(fd);
    if (rc != CLI_OK)
        return rc;
    if (memcmp(sec, back, t->sector) != 0)
        return set_error(CLI_ERR_IO, "the marker did not read back on %s",
                         t->path);
    return CLI_OK;
}
