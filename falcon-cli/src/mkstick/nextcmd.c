/* falcon-boot mkstick: `next-boot`, setting the next-boot marker from Linux
 * (docs/DESIGN.md Section 6.3.1).
 *
 * The marker says "on the next boot, start this entry instead of drawing the
 * menu", and both loaders read it and clear it before believing it. Where it
 * is kept depends on the firmware the machine booted with:
 *
 *   - UEFI (/sys/firmware/efi exists): the FalconNextBoot variable, written
 *     through efivarfs with the attributes the loader sets it with;
 *   - BIOS: one sector of the `boot` partition, at the offset its identity
 *     record names, dated with the RTC's reading so the loader honours it
 *     only for ten minutes.
 *
 * The value is checked with the loaders' own rule (nextboot_decode) before
 * anything is written, and the sector is written only when it is empty or
 * already a marker: a sector holding anything else is not the one the
 * record meant, and is left alone.
 *
 * This replaces archusb's `archusb-boot-next` writer, which wrote the BIOS
 * marker to LBA 2047 of the disk with dd: that sector is inside the first
 * partition on an MBR disk whose first partition starts at 63.
 */

#include <mkstick/mkstick.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <common/log.h>
#include <common/util.h>
#include <mkstick/disk.h>
#include <mkstick/nextcore.h>
#include <mkstick/records.h>

#include <media/nextboot.h>

#define NEXT_MAX_DISKS 16u

static const cli_opt next_opts[] = {
    { "disk", CLI_OPT_VALUE, 0, "<dev>",
      "the disk holding the boot partition, when more than one does" },
    { "sector", CLI_OPT_FLAG, 0, NULL,
      "write the BIOS marker sector even on a UEFI machine" },
    { "clear", CLI_OPT_FLAG, 0, NULL,
      "remove a pending marker instead of setting one" }
};

/* ---- UEFI: the variable ---- */

/* efivarfs marks variables immutable so an `rm -rf /` cannot brick a
 * machine; the flag has to come off before the file can be rewritten or
 * removed. A file that is not there has nothing to clear. */
static cli_status efivar_unlock(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    int flags = 0;

    if (fd < 0)
        return (errno == ENOENT) ? CLI_OK
                                 : set_error_errno(CLI_ERR_IO, "opening %s",
                                                   path);
    if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0 &&
        (flags & FS_IMMUTABLE_FL) != 0) {
        flags &= ~FS_IMMUTABLE_FL;
        if (ioctl(fd, FS_IOC_SETFLAGS, &flags) != 0) {
            (void)close(fd);
            return set_error_errno(CLI_ERR_PERM,
                                   "clearing the immutable flag on %s", path);
        }
    }
    (void)close(fd);
    return CLI_OK;
}

static cli_status set_variable(const char *value, int clear)
{
    char path[MKS_PATH_MAX];
    uint8_t buf[4u + NEXTBOOT_MAX + 1u];
    size_t len = 0;
    cli_status rc;
    int fd;

    (void)snprintf(path, sizeof path, "%s/%s", NEXTCORE_EFIVARS_DIR,
                   NEXTCORE_EFIVAR);
    if (!clear) {
        len = nextcore_efivar(value, buf, sizeof buf);
        if (len == 0u)
            return set_error(CLI_ERR_INVALID,
                             "'%s' is not a next-boot value: printable ASCII, "
                             "a path on p2 or a reserved name such as %s",
                             value, NEXTBOOT_PERSONAL);
    }

    rc = efivar_unlock(path);
    if (rc != CLI_OK)
        return rc;
    if (clear) {
        if (unlink(path) != 0 && errno != ENOENT)
            return set_error_errno(CLI_ERR_IO, "removing %s", path);
        log_info("FalconNextBoot cleared");
        return CLI_OK;
    }

    /* One write: efivarfs sets the variable from exactly one write call. */
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s", path);
    if (write(fd, buf, len) != (ssize_t)len) {
        (void)close(fd);
        return set_error_errno(CLI_ERR_IO, "writing %s", path);
    }
    if (close(fd) != 0)
        return set_error_errno(CLI_ERR_IO, "writing %s", path);
    log_info("FalconNextBoot = %s", value);
    return CLI_OK;
}

/* ---- BIOS: the sector ---- */

typedef struct {
    char     path[MKS_PATH_MAX];
    uint64_t marker_lba;
} next_target;

/* The boot partition on one disk, when it has one with a marker. */
static int probe_disk(const char *path, next_target *out)
{
    static mks_records r;
    mks_disk d;
    int boot;

    if (disk_stat(&d, path) != CLI_OK) {
        cli_error_clear();
        return 0;
    }
    if (d.sector != NEXTBOOT_SECTOR_BYTES)
        return 0;
    if (records_read(&r, d.path, d.sector, d.size) != CLI_OK) {
        cli_error_clear();
        return 0;
    }
    boot = records_boot(&r);
    if (boot < 0 || r.recs[boot].rec.marker_sector == 0u)
        return 0;
    (void)snprintf(out->path, sizeof out->path, "%s", d.path);
    out->marker_lba = r.parts[boot].start_lba +
                      r.recs[boot].rec.marker_sector;
    return 1;
}

/* Every whole disk the kernel lists, bar the ones that are never a boot
 * disk. Loop devices are left out too: an image is named with --disk. */
static cli_status find_target(next_target *out)
{
    static next_target found[NEXT_MAX_DISKS];
    static const char *const skip_prefix[] = {
        "loop", "ram", "zram", "sr", "fd", "dm-", "md", "nbd"
    };
    size_t n = 0;
    size_t i;
    DIR *dir;
    struct dirent *e;

    dir = opendir("/sys/block");
    if (dir == NULL)
        return set_error_errno(CLI_ERR_IO, "listing /sys/block");
    while ((e = readdir(dir)) != NULL && n < NEXT_MAX_DISKS) {
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
        if (probe_disk(path, &found[n]))
            n++;
    }
    (void)closedir(dir);

    if (n == 0u)
        return set_error(CLI_ERR_NOTFOUND,
                         "no disk carries a falcon boot partition with a "
                         "marker sector%s; name one with --disk",
                         geteuid() != 0 ? " that this user can read" : "");
    if (n > 1u) {
        for (i = 0; i < n; i++)
            log_error("a boot partition is on %s", found[i].path);
        return set_error(CLI_ERR_USAGE,
                         "%zu disks carry a falcon boot partition; name one "
                         "with --disk", n);
    }
    *out = found[0];
    return CLI_OK;
}

static cli_status read_rtc(uint64_t *now)
{
    char text[32];
    FILE *f = fopen(NEXTCORE_RTC_EPOCH, "re");
    size_t n;

    if (f == NULL)
        return set_error_errno(CLI_ERR_NOTFOUND,
                               "opening %s (the RTC reading the loader dates "
                               "the marker against)", NEXTCORE_RTC_EPOCH);
    n = fread(text, 1, sizeof text - 1u, f);
    (void)fclose(f);
    text[n] = '\0';
    if (nextcore_parse_epoch(text, n, now) != 0)
        return set_error(CLI_ERR_INVALID, "%s does not hold a number",
                         NEXTCORE_RTC_EPOCH);
    return CLI_OK;
}

static cli_status set_sector(const char *disk, const char *value, int clear)
{
    uint8_t cur[NEXTBOOT_SECTOR_BYTES];
    uint8_t sec[NEXTBOOT_SECTOR_BYTES];
    uint8_t back[NEXTBOOT_SECTOR_BYTES];
    next_target t;
    uint64_t now = 0;
    off_t off;
    cli_status rc;
    int fd;

    memset(sec, 0, sizeof sec);
    memset(&t, 0, sizeof t);
    if (!clear) {
        rc = read_rtc(&now);
        if (rc != CLI_OK)
            return rc;
        if (nextboot_sector_write(sec, sizeof sec, now, value) != NEXTBOOT_OK)
            return set_error(CLI_ERR_INVALID,
                             "'%s' is not a next-boot value: printable ASCII, "
                             "a path on p2 or a reserved name such as %s",
                             value, NEXTBOOT_PERSONAL);
    }

    if (disk != NULL) {
        if (!probe_disk(disk, &t))
            return set_error(CLI_ERR_NOTFOUND,
                             "%s carries no falcon boot partition with a "
                             "marker sector", disk);
    } else {
        rc = find_target(&t);
        if (rc != CLI_OK)
            return rc;
    }

    fd = open(t.path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s for writing", t.path);
    off = (off_t)(t.marker_lba * NEXTBOOT_SECTOR_BYTES);
    if (pread(fd, cur, sizeof cur, off) != (ssize_t)sizeof cur) {
        (void)close(fd);
        return set_error_errno(CLI_ERR_IO, "reading %s", t.path);
    }
    if (!nextcore_sector_ours(cur, sizeof cur)) {
        (void)close(fd);
        return set_error(CLI_ERR_INVALID,
                         "LBA %llu of %s is neither empty nor a next-boot "
                         "marker; left alone",
                         (unsigned long long)t.marker_lba, t.path);
    }
    if (pwrite(fd, sec, sizeof sec, off) != (ssize_t)sizeof sec ||
        fsync(fd) != 0 ||
        pread(fd, back, sizeof back, off) != (ssize_t)sizeof back) {
        (void)close(fd);
        return set_error_errno(CLI_ERR_IO, "writing %s", t.path);
    }
    (void)close(fd);
    if (memcmp(sec, back, sizeof sec) != 0)
        return set_error(CLI_ERR_IO, "the marker did not read back on %s",
                         t.path);

    if (clear)
        log_info("next-boot marker cleared on %s (LBA %llu)", t.path,
                 (unsigned long long)t.marker_lba);
    else
        log_info("next-boot marker on %s (LBA %llu): %s, good for %u "
                 "minutes", t.path, (unsigned long long)t.marker_lba, value,
                 (unsigned)(NEXTBOOT_WINDOW_SECONDS / 60u));
    return CLI_OK;
}

static cli_status next_run(const cli_args *a)
{
    const char *value = (a->npos > 0u) ? a->pos[0] : NULL;
    const char *disk = cli_value(a, "disk", NULL);
    int clear = cli_flag(a, "clear");
    int sector = cli_flag(a, "sector") || !util_is_dir("/sys/firmware/efi");

    if (clear && value != NULL)
        return set_error(CLI_ERR_USAGE, "--clear takes no value");
    if (!clear && value == NULL)
        return set_error(CLI_ERR_USAGE, "which entry? a path on p2, or %s",
                         NEXTBOOT_PERSONAL);
    if (!sector && disk != NULL)
        return set_error(CLI_ERR_USAGE,
                         "this machine booted with UEFI and keeps the marker "
                         "in a variable; --disk is for the sector (--sector)");
    if (sector)
        return set_sector(disk, value, clear);
    return set_variable(value, clear);
}

const cli_cmd mks_cmd_next_boot = {
    "next-boot",
    "start an entry on the next boot instead of drawing the menu",
    "[<value>]",
    next_opts,
    sizeof next_opts / sizeof next_opts[0],
    0, 1,
    "The value is a path on p2 (as the menu lists it) or a reserved name:\n"
    NEXTBOOT_PERSONAL " is the personal OS. The loader clears the marker\n"
    "before acting on it and checks the value against what it finds, as a\n"
    "menu pick would be.\n"
    "\n"
    "On a machine booted with UEFI it sets the FalconNextBoot variable. On a\n"
    "BIOS machine, or with --sector, it writes the marker sector of the\n"
    "disk's boot partition (the one whose identity record says boot), dated\n"
    "with the RTC's reading; the loader honours it for ten minutes. --disk\n"
    "names the disk when more than one carries a boot partition.\n"
    "\n"
    "--clear removes a pending marker. Needs root.\n",
    next_run
};
