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
 * record meant, and is left alone. The sector's I/O, the choice of disk and
 * the --print report are nextio.c, where the tests reach them.
 *
 * --print reads whichever marker this machine would use and reports it as
 * key=value lines for a script (the format is in the help below and in
 * docs/Manual.md Section 3.5), changing nothing.
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
#include <mkstick/nextio.h>

#include <media/nextboot.h>

#define NEXT_MAX_DISKS 16u

static const cli_opt next_opts[] = {
    { "disk", CLI_OPT_VALUE, 0, "<dev>",
      "only this disk (or image) is looked at for the boot partition" },
    { "sector", CLI_OPT_FLAG, 0, NULL,
      "use the BIOS marker sector even on a UEFI machine" },
    { "clear", CLI_OPT_FLAG, 0, NULL,
      "remove a pending marker instead of setting one" },
    { "print", CLI_OPT_FLAG, 0, NULL,
      "print what is queued, as key=value lines, and change nothing" }
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

/* Every whole disk the kernel lists, bar the ones that are never a boot
 * disk. Loop devices are left out too: an image is named with --disk. */
static cli_status list_disks(char (*paths)[MKS_PATH_MAX], size_t cap,
                             size_t *n)
{
    static const char *const skip_prefix[] = {
        "loop", "ram", "zram", "sr", "fd", "dm-", "md", "nbd"
    };
    size_t i;
    DIR *dir;
    struct dirent *e;

    *n = 0;
    dir = opendir("/sys/block");
    if (dir == NULL)
        return set_error_errno(CLI_ERR_IO, "listing /sys/block");
    while ((e = readdir(dir)) != NULL && *n < cap) {
        int skip = (e->d_name[0] == '.');

        for (i = 0; i < sizeof skip_prefix / sizeof skip_prefix[0]; i++) {
            if (strncmp(e->d_name, skip_prefix[i],
                        strlen(skip_prefix[i])) == 0)
                skip = 1;
        }
        if (skip)
            continue;
        (void)snprintf(paths[*n], MKS_PATH_MAX, "/dev/%s", e->d_name);
        (*n)++;
    }
    (void)closedir(dir);
    return CLI_OK;
}

/* The disk whose marker this is. --disk is that disk and no other: the
 * kernel's list is not even read (nextio_resolve). */
static cli_status find_target(const char *disk, next_target *out)
{
    static char paths[NEXT_MAX_DISKS][MKS_PATH_MAX];
    const char *cands[NEXT_MAX_DISKS];
    size_t n = 0;
    size_t i;
    cli_status rc;

    if (disk != NULL)
        return nextio_resolve(disk, NULL, 0, out);
    rc = list_disks(paths, NEXT_MAX_DISKS, &n);
    if (rc != CLI_OK)
        return rc;
    for (i = 0; i < n; i++)
        cands[i] = paths[i];
    return nextio_resolve(NULL, cands, n, out);
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
    uint8_t probe[NEXTBOOT_SECTOR_BYTES];
    next_target t;
    uint64_t now = 0;
    cli_status rc;

    if (!clear) {
        /* The value first, so a typo is reported before the disks are. */
        if (nextboot_sector_write(probe, sizeof probe, 0, value) !=
            NEXTBOOT_OK)
            return set_error(CLI_ERR_INVALID,
                             "'%s' is not a next-boot value: printable ASCII, "
                             "a path on p2 or a reserved name such as %s",
                             value, NEXTBOOT_PERSONAL);
        rc = read_rtc(&now);
        if (rc != CLI_OK)
            return rc;
    }
    rc = find_target(disk, &t);
    if (rc != CLI_OK)
        return rc;
    rc = nextio_write(&t, clear ? NULL : value, now);
    if (rc != CLI_OK)
        return rc;

    if (clear)
        log_info("next-boot marker cleared on %s (LBA %llu)", t.path,
                 (unsigned long long)t.marker_lba);
    else
        log_info("next-boot marker on %s (LBA %llu): %s, good for %u "
                 "minutes", t.path, (unsigned long long)t.marker_lba, value,
                 (unsigned)(NEXTBOOT_WINDOW_SECONDS / 60u));
    return CLI_OK;
}

/* ---- --print ---- */

static cli_status print_sector(const char *disk)
{
    next_target t;
    next_queued q;
    uint64_t now = 0;
    int have_now;
    cli_status rc;

    rc = find_target(disk, &t);
    if (rc == CLI_OK)
        rc = nextio_read(&t, &q);
    if (rc != CLI_OK)
        return rc;
    have_now = (read_rtc(&now) == CLI_OK);
    if (!have_now)
        cli_error_clear();
    nextio_print_sector(stdout, &t, &q, have_now, now);
    return CLI_OK;
}

static cli_status print_variable(void)
{
    char path[MKS_PATH_MAX];
    uint8_t buf[4u + NEXTBOOT_MAX + 1u];
    ssize_t n;
    int fd;

    (void)snprintf(path, sizeof path, "%s/%s", NEXTCORE_EFIVARS_DIR,
                   NEXTCORE_EFIVAR);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT)
            return set_error_errno(CLI_ERR_IO, "opening %s", path);
        nextio_print_efivar(stdout, NULL, 0);
        return CLI_OK;
    }
    n = read(fd, buf, sizeof buf);
    (void)close(fd);
    if (n < 0)
        return set_error_errno(CLI_ERR_IO, "reading %s", path);
    /* A value too long to fill the buffer exactly is longer than any the
     * loader accepts; the decode refuses it as it would. */
    nextio_print_efivar(stdout, buf, (size_t)n);
    return CLI_OK;
}

static cli_status next_run(const cli_args *a)
{
    const char *value = (a->npos > 0u) ? a->pos[0] : NULL;
    const char *disk = cli_value(a, "disk", NULL);
    int clear = cli_flag(a, "clear");
    int print = cli_flag(a, "print");
    int sector = cli_flag(a, "sector") || !util_is_dir("/sys/firmware/efi");

    if (print && (clear || value != NULL))
        return set_error(CLI_ERR_USAGE, "--print takes no value and no "
                                        "--clear");
    if (clear && value != NULL)
        return set_error(CLI_ERR_USAGE, "--clear takes no value");
    if (!clear && !print && value == NULL)
        return set_error(CLI_ERR_USAGE, "which entry? a path on p2, or %s",
                         NEXTBOOT_PERSONAL);
    if (!sector && disk != NULL)
        return set_error(CLI_ERR_USAGE,
                         "this machine booted with UEFI and keeps the marker "
                         "in a variable; --disk is for the sector (--sector)");
    if (print)
        return sector ? print_sector(disk) : print_variable();
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
    "disk's boot partition (the one whose identity record says boot), one\n"
    "whole sector of the disk (512 bytes, or 4096 on a 4Kn disk), dated\n"
    "with the RTC's reading; the loader honours it for ten minutes. Without\n"
    "--disk every disk is looked at and exactly one must carry a boot\n"
    "partition. With --disk only that disk is looked at, whatever else is\n"
    "attached.\n"
    "\n"
    "--clear removes a pending marker. Both need root (or write access to\n"
    "the disk).\n"
    "\n"
    "--print writes what is queued to stdout, one key=value per line, and\n"
    "exits 0 whether or not anything is. The UEFI variable:\n"
    "\n"
    "  source=efivar\n"
    "  queued=yes|no|invalid\n"
    "  value=<the value>            (queued=yes only)\n"
    "\n"
    "The sector (a BIOS machine, or --sector):\n"
    "\n"
    "  source=sector\n"
    "  disk=<the disk>\n"
    "  lba=<the marker's LBA, in the disk's sectors>\n"
    "  sector_size=<512 or 4096>\n"
    "  queued=yes|no|invalid\n"
    "  value=<the value>            (this and the rest: queued=yes only)\n"
    "  set_rtc=<the RTC's reading when it was set>\n"
    "  age=<seconds since; negative if the clock went back>\n"
    "  window=yes|no                (the loader would honour it now)\n"
    "\n"
    "invalid is a marker the loader would clear and not act on. age and\n"
    "window are unknown when the RTC cannot be read.\n",
    next_run
};
