#include <mkstick/disk.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <common/log.h>
#include <common/proc.h>
#include <common/util.h>
#include <mkstick/cleanup.h>

cli_status disk_partition_name(const char *base, unsigned n,
                               char *out, size_t cap)
{
    size_t len = strlen(base);
    const char *sep = "";
    int written;

    if (len == 0)
        return set_error(CLI_ERR_INTERNAL, "empty base device name");

    /* The kernel's own rule: a name ending in a digit gets a 'p' so that
     * nvme0n1 + 1 is nvme0n1p1 and not the unparseable nvme0n11. */
    if (isdigit((unsigned char)base[len - 1]) != 0)
        sep = "p";

    written = snprintf(out, cap, "%s%s%u", base, sep, n);
    if (written < 0 || (size_t)written >= cap)
        return set_error(CLI_ERR_RANGE, "partition name for %s does not fit",
                         base);
    return CLI_OK;
}

cli_status disk_stat(mks_disk *d, const char *path)
{
    struct stat st;
    int fd;

    memset(d, 0, sizeof *d);
    if (strlen(path) >= MKS_PATH_MAX)
        return set_error(CLI_ERR_RANGE, "target path too long");
    (void)snprintf(d->path, sizeof d->path, "%s", path);

    if (stat(path, &st) != 0)
        return set_error_errno(CLI_ERR_NOTFOUND, "stat %s", path);

    d->sector = 512;
    if (S_ISBLK(st.st_mode)) {
        uint64_t bytes = 0;
        int sector = 512;

        d->is_block = 1;
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return set_error_errno(CLI_ERR_IO, "opening %s", path);
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
            (void)close(fd);
            return set_error_errno(CLI_ERR_IO, "BLKGETSIZE64 on %s", path);
        }
        if (ioctl(fd, BLKSSZGET, &sector) == 0 && sector > 0)
            d->sector = (uint32_t)sector;
        (void)close(fd);
        d->size = bytes;
        {
            char real[MKS_PATH_MAX];
            char sys[MKS_PATH_MAX + 32];
            const char *base;

            if (realpath(path, real) == NULL)
                (void)snprintf(real, sizeof real, "%s", path);
            base = strrchr(real, '/');
            base = (base != NULL) ? base + 1 : real;
            (void)snprintf(sys, sizeof sys, "/sys/class/block/%s/loop",
                           base);
            d->is_loop = util_is_dir(sys);
        }
    } else if (S_ISREG(st.st_mode)) {
        d->is_block = 0;
        d->size = (uint64_t)st.st_size;
    } else {
        return set_error(CLI_ERR_INVALID,
                         "%s is neither a regular file nor a block device",
                         path);
    }
    return CLI_OK;
}

/* Does the mount source `src` name this disk or one of its partitions?
 * Partition names are the disk name plus digits, optionally after a 'p', so
 * the test is "same prefix, and the tail is only that". */
static int names_this_disk(const char *src, const char *disk)
{
    size_t len = strlen(disk);
    const char *tail;

    if (strncmp(src, disk, len) != 0)
        return 0;
    tail = src + len;
    if (*tail == '\0')
        return 1;
    if (*tail == 'p')
        tail++;
    if (*tail == '\0')
        return 0;
    while (*tail != '\0') {
        if (isdigit((unsigned char)*tail) == 0)
            return 0;
        tail++;
    }
    return 1;
}

cli_status disk_check_not_mounted(const mks_disk *d)
{
    FILE *f;
    char line[8192];
    char real[MKS_PATH_MAX];
    const char *target = d->path;
    int busy = 0;

    if (!d->is_block)
        return CLI_OK;

    /* /dev/disk/by-id/... resolves to /dev/sdX, which is what /proc/mounts
     * actually carries. Compare the resolved names or the check is one
     * symlink away from useless. */
    if (realpath(d->path, real) != NULL)
        target = real;

    f = fopen("/proc/mounts", "re");
    if (f == NULL)
        return set_error_errno(CLI_ERR_IO, "opening /proc/mounts");

    while (fgets(line, sizeof line, f) != NULL) {
        char src[MKS_PATH_MAX];
        char dst[MKS_PATH_MAX];

        if (sscanf(line, "%4095s %4095s", src, dst) != 2)
            continue;
        if (names_this_disk(src, target)) {
            log_error("%s is mounted at %s", src, dst);
            busy = 1;
        }
    }
    (void)fclose(f);

    if (busy)
        return set_error(CLI_ERR_BUSY,
                         "%s (or a partition of it) is mounted; unmount it "
                         "before writing to the device", target);
    return CLI_OK;
}

/* The sysfs model string, when the kernel has one. Absent for loop devices
 * and for some USB bridges, so its absence is not an error. */
static void read_model(const mks_disk *d, char *out, size_t cap)
{
    char real[MKS_PATH_MAX];
    char sysfs[MKS_PATH_MAX];
    const char *name;
    FILE *f;

    out[0] = '\0';
    if (realpath(d->path, real) == NULL)
        return;
    name = util_basename(real);
    if (snprintf(sysfs, sizeof sysfs, "/sys/block/%s/device/model",
                 name) < 0)
        return;

    f = fopen(sysfs, "re");
    if (f == NULL)
        return;
    if (fgets(out, (int)cap, f) != NULL) {
        size_t len = strlen(out);

        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == ' '))
            out[--len] = '\0';
    }
    (void)fclose(f);
}

void disk_describe(const mks_disk *d)
{
    char size[32];
    char model[256];

    read_model(d, model, sizeof model);
    log_info("target:   %s (%s)", d->path,
             d->is_block ? "block device" : "regular file");
    if (model[0] != '\0')
        log_info("model:    %s", model);
    log_info("size:     %s (%llu bytes), %u-byte sectors",
             util_format_size(d->size, size, sizeof size),
             (unsigned long long)d->size, d->sector);
}

cli_status disk_attach(mks_disk *d)
{
    unsigned i;
    cli_status rc;

    if (d->is_block) {
        /* The kernel's node, not a /dev/disk/by-id link: partitions are named
         * from it (sdb1, nvme0n1p1), and a link's own partition links are
         * named by udev (-part1) and only after it has run. */
        if (realpath(d->path, d->base) == NULL)
            return set_error_errno(CLI_ERR_NOTFOUND, "resolving %s", d->path);
    } else {
        char out[MKS_PATH_MAX];
        size_t len = 0;
        size_t end;

        rc = proc_capture_l(out, sizeof out, &len, "losetup",
                            "--show", "-f", "-P", d->path, PROC_END);
        if (rc != CLI_OK)
            return rc;
        end = len;
        while (end > 0 && (out[end - 1] == '\n' || out[end - 1] == '\r'))
            out[--end] = '\0';
        if (end == 0)
            return set_error(CLI_ERR_EXEC,
                             "losetup did not name a loop device for %s",
                             d->path);
        (void)snprintf(d->base, sizeof d->base, "%s", out);
        rc = cleanup_push_loop(d->base);
        if (rc != CLI_OK) {
            (void)proc_runl_quiet("losetup", "-d", d->base, PROC_END);
            return rc;
        }
        d->loop_attached = 1;
        log_debug("attached %s as %s", d->path, d->base);
    }

    for (i = 0; i < MKS_PARTS; i++) {
        rc = disk_partition_name(d->base, i + 1u, d->part[i],
                                 sizeof d->part[i]);
        if (rc != CLI_OK)
            return rc;
    }
    return CLI_OK;
}

cli_status disk_detach(mks_disk *d)
{
    cli_status rc;

    if (!d->loop_attached)
        return CLI_OK;
    rc = cleanup_pop();
    d->loop_attached = 0;
    return rc;
}

cli_status disk_settle(const mks_disk *d)
{
    unsigned tries;

    /* udevadm is advisory: on a machine without it (or in a container) the
     * poll below is what actually guarantees the nodes exist. */
    if (proc_exists("udevadm"))
        (void)proc_runl_quiet("udevadm", "settle", PROC_END);

    for (tries = 0; tries < 100u; tries++) {
        unsigned i;
        int all = 1;

        for (i = 0; i < MKS_PARTS; i++) {
            if (!util_is_block(d->part[i]))
                all = 0;
        }
        if (all)
            return CLI_OK;
        (void)usleep(50000);
    }
    return set_error(CLI_ERR_IO,
                     "partition nodes for %s did not appear (expected %s)",
                     d->base, d->part[0]);
}

cli_status disk_settle_node(const char *node)
{
    unsigned tries;

    if (proc_exists("udevadm"))
        (void)proc_runl_quiet("udevadm", "settle", PROC_END);
    for (tries = 0; tries < 100u; tries++) {
        if (util_is_block(node))
            return CLI_OK;
        (void)usleep(50000);
    }
    return set_error(CLI_ERR_IO, "partition node %s did not appear", node);
}

cli_status disk_io_open(disk_io *io, const char *path, uint64_t base,
                        uint64_t len)
{
    struct stat st;

    memset(io, 0, sizeof *io);
    io->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (io->fd < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s", path);
    /* A whole disk and each of its partition nodes keep separate buffer
     * caches, so a disk read right after mkfs wrote through /dev/sdX1 can
     * return the sectors from before the format (measured 2026-09-26: the
     * boot record's reserved-sector check saw mkfs's old 32 in place of
     * 2080). Flushing drops this device's cached sectors, so what follows
     * reads the medium. Needs CAP_SYS_ADMIN, which every command that
     * reads a block device already has; an image file has no such cache. */
    if (fstat(io->fd, &st) == 0 && S_ISBLK(st.st_mode))
        (void)ioctl(io->fd, BLKFLSBUF, 0);
    io->base = base;
    io->len = len;
    (void)snprintf(io->name, sizeof io->name, "%s", path);
    return CLI_OK;
}

void disk_io_close(disk_io *io)
{
    if (io->fd >= 0)
        (void)close(io->fd);
    io->fd = -1;
}

int disk_io_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    disk_io *io = (disk_io *)ctx;
    unsigned char *p = (unsigned char *)buf;
    uint64_t done = 0;

    if (io->len != 0 && (off > io->len || len > io->len - off))
        return -1;

    while (done < len) {
        ssize_t n = pread(io->fd, p + done, (size_t)(len - done),
                          (off_t)(io->base + off + done));

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        done += (uint64_t)n;
    }
    return 0;
}
