/* falcon-hook: recreate the ISO as a block device, then hand over to the
 * distro's own init (docs/DESIGN.md Section 6.4; docs/notes/
 * coverage-roadmap.md L2).
 *
 * The loader boots an ISO's kernel with the ISO's own cmdline plus
 * rdinit=/falcon/hook, and appends a cpio carrying this binary to the ISO's
 * own initrd. The kernel therefore starts this as PID 1, in the distro's
 * initramfs, before the distro's init has run. It:
 *
 *   1. mounts proc, sysfs, devtmpfs and efivarfs, noting which it mounted;
 *   2. reads the extent map the loader left in FalconIsoMap; with none, the
 *      boot is not one of ours and everything below is skipped;
 *   3. has the initramfs's own modprobe load what the stick and device
 *      mapper need (the drivers are the distro's; we cannot ship any);
 *   4. waits for the disk whose GPT carries the map's partition, verifying
 *      the table's CRCs with falcon-core's own parser;
 *   5. creates /dev/mapper/falcon-iso, read-only, one linear target per run
 *      of the ISO file, over p2's own partition device (the whole disk only
 *      when the kernel made no partition devices);
 *   6. unmounts what it mounted and execs the distro's init as PID 1.
 *
 * From there the distro finds a device holding its ISO9660 filesystem, with
 * its own label, and boots as if from a CD: dracut's root=live:CDLABEL=,
 * archiso's archisolabel=, casper's and live-boot's scans all find it.
 *
 * Every failure is logged to the kernel log (dmesg, and the console) and then
 * ignored: the distro's init runs whatever happened here, because a boot
 * that fails in the distro's own way says more than one that stops in ours.
 */

#include "sys.h"
#include "hookcore.h"
#include "dimod.h"

#include <media/extmap.h>
#include <media/hookcmd.h>
#include <part/part.h>
#include <lib/string.h>

#include <linux/ioctl.h>
#include <linux/dm-ioctl.h>
#include <linux/loop.h>

/* How long the disk gets to appear: USB enumeration on real hardware is
 * seconds, and a stick behind a hub is more. */
#define WAIT_MS      30000u
#define POLL_MS      250u

#define MAP_FILE_CAP (EXTMAP_HDR_LEN + EXTMAP_MAX_EXTENTS * EXTMAP_EXT_LEN + 8u)

static int g_kmsg = -1;
/* Set in the udev nudge's child, which opens the log for each line rather
 * than keeping it open. */
static int g_child;

static size_t slen(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

/* Appends src to dst (cap bytes), always terminating. */
static void cat(char *dst, size_t cap, const char *src)
{
    size_t d = slen(dst), i = 0;

    while (src[i] != '\0' && d + 1u < cap)
        dst[d++] = src[i++];
    dst[d] = '\0';
}

static void cat_dec(char *dst, size_t cap, long v)
{
    char tmp[24];
    size_t n = 0;
    unsigned long u;

    if (v < 0) {
        cat(dst, cap, "-");
        u = (unsigned long)(-(v + 1)) + 1u;
    } else {
        u = (unsigned long)v;
    }
    do {
        tmp[n++] = (char)('0' + (int)(u % 10u));
        u /= 10u;
    } while (u != 0 && n < sizeof tmp);
    while (n > 0) {
        char c[2] = { tmp[--n], '\0' };

        cat(dst, cap, c);
    }
}

/* One line to the kernel log at "info", prefixed so it can be found. */
static void logmsg(const char *a, const char *b, long num, int with_num)
{
    char line[256];

    line[0] = '\0';
    cat(line, sizeof line, "<6>falcon-hook: ");
    cat(line, sizeof line, a);
    if (b != (const char *)0)
        cat(line, sizeof line, b);
    if (with_num)
        cat_dec(line, sizeof line, num);
    cat(line, sizeof line, "\n");
    if (g_kmsg >= 0) {
        (void)sys_write(g_kmsg, line, slen(line));
    } else if (g_child) {
        long fd = sys_open("/dev/kmsg", HOOK_O_WRONLY | HOOK_O_CLOEXEC, 0);

        if (fd >= 0) {
            (void)sys_write((int)fd, line, slen(line));
            (void)sys_close((int)fd);
        }
    }
}

#define LOG(a)          logmsg((a), (const char *)0, 0, 0)
#define LOG2(a, b)      logmsg((a), (b), 0, 0)
#define LOGN(a, n)      logmsg((a), (const char *)0, (long)(n), 1)

/* ---- mounts ---- */

enum { M_PROC, M_SYS, M_DEV, M_EFIVARS, M_COUNT };

static const char *const g_mnt_path[M_COUNT] = {
    "/proc", "/sys", "/dev", "/sys/firmware/efi/efivars"
};
static const char *const g_mnt_type[M_COUNT] = {
    "proc", "sysfs", "devtmpfs", "efivarfs"
};
static int g_mounted[M_COUNT];

static void mount_one(int which)
{
    long rc;

    (void)sys_mkdir(g_mnt_path[which], 0755);
    rc = sys_mount(g_mnt_type[which], g_mnt_path[which], g_mnt_type[which],
                   0, (const void *)0);
    if (rc == 0)
        g_mounted[which] = 1;
}

static void unmount_all(void)
{
    int i;

    for (i = M_COUNT - 1; i >= 0; i--) {
        if (g_mounted[i]) {
            if (i == M_DEV && g_kmsg >= 0) {
                (void)sys_close(g_kmsg);
                g_kmsg = -1;
            }
            if (sys_umount(g_mnt_path[i]) != 0 && g_kmsg >= 0)
                LOG2("could not unmount ", g_mnt_path[i]);
            g_mounted[i] = 0;
        }
    }
}

/* ---- small file helpers ---- */

static long read_file(const char *path, void *buf, size_t cap)
{
    long fd = sys_open(path, HOOK_O_RDONLY | HOOK_O_CLOEXEC, 0);
    long got = 0;

    if (fd < 0)
        return fd;
    while ((size_t)got < cap) {
        long n = sys_read((int)fd, (uint8_t *)buf + got, cap - (size_t)got);

        if (n <= 0)
            break;
        got += n;
    }
    (void)sys_close((int)fd);
    return got;
}

/* ---- modules ---- */

/* What the stick and the mapping need, by the names every distro kernel
 * uses. Built in on some kernels and absent on others; modprobe -a loads
 * what it finds and the failures are no business of ours. */
static char *g_modules[] = {
    "modprobe", "-q", "-a",
    "dm_mod", "ahci", "sd_mod", "usb_storage", "uas", "xhci_pci",
    "xhci_hcd", "ehci_pci", "ohci_pci", "uhci_hcd", "nvme", "virtio_blk",
    "virtio_scsi", "virtio_pci", "sdhci_pci", "mmc_block", "efivarfs",
    "loop", "isofs", (char *)0
};

static void load_modules(char **envp)
{
    static const char *const where[] = {
        "/sbin/modprobe", "/usr/sbin/modprobe", "/bin/modprobe",
        "/usr/bin/modprobe", (const char *)0
    };
    size_t i;

    for (i = 0; where[i] != (const char *)0; i++) {
        long pid;
        int status = 0;
        long fd = sys_open(where[i], HOOK_O_RDONLY | HOOK_O_CLOEXEC, 0);

        if (fd < 0)
            continue;
        (void)sys_close((int)fd);
        pid = sys_fork();
        if (pid == 0) {
            (void)sys_execve(where[i], g_modules, envp);
            sys_exit(127);
        }
        if (pid > 0)
            (void)sys_wait(pid, &status);
        LOG2("loaded drivers with ", where[i]);
        return;
    }
    LOG("no modprobe in this initramfs; relying on built-in drivers");
}

/* ---- finding the disk ---- */

typedef struct {
    int fd;
    uint32_t block;
} disk_ctx;

static int disk_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    disk_ctx *d = ctx;
    uint64_t done = 0;

    while (done < len) {
        long n = sys_pread(d->fd, (uint8_t *)buf + done, (size_t)(len - done),
                           off + done);

        if (n <= 0)
            return -1;
        done += (uint64_t)n;
    }
    return 0;
}

static gpt_part g_parts[128];

/* Does the disk NAME carry the map's partition? On a match, its dev number,
 * its logical block size, and the partition's 1-based GPT index. */
static int disk_matches(const char *name, const extmap_hdr *h, uint32_t *maj,
                        uint32_t *min, uint32_t *block, uint32_t *partno)
{
    char path[96], txt[32];
    disk_ctx d;
    gpt_table t;
    long n;
    uint32_t i;
    int ok = 0;

    path[0] = '\0';
    cat(path, sizeof path, "/sys/block/");
    cat(path, sizeof path, name);
    cat(path, sizeof path, "/dev");
    n = read_file(path, txt, sizeof txt - 1u);
    if (n <= 0 || !hook_parse_devnum(txt, (size_t)n, maj, min))
        return 0;
    path[0] = '\0';
    cat(path, sizeof path, "/sys/block/");
    cat(path, sizeof path, name);
    cat(path, sizeof path, "/queue/logical_block_size");
    n = read_file(path, txt, sizeof txt - 1u);
    if (n <= 0)
        return 0;
    txt[n] = '\0';
    d.block = 0;
    for (i = 0; txt[i] >= '0' && txt[i] <= '9'; i++)
        d.block = d.block * 10u + (uint32_t)(txt[i] - '0');
    if (d.block != 512u && d.block != 1024u && d.block != 2048u &&
        d.block != 4096u)
        return 0;

    path[0] = '\0';
    cat(path, sizeof path, "/dev/");
    cat(path, sizeof path, name);
    n = sys_open(path, HOOK_O_RDONLY | HOOK_O_CLOEXEC, 0);
    if (n < 0) {
        /* devtmpfs names the node after the kernel's name; if something
         * renamed it, make our own from the numbers. */
        path[0] = '\0';
        cat(path, sizeof path, "/dev/falcon-disk");
        (void)sys_mknod(path, HOOK_S_IFBLK | 0600, hook_devnum(*maj, *min));
        n = sys_open(path, HOOK_O_RDONLY | HOOK_O_CLOEXEC, 0);
        if (n < 0)
            return 0;
    }
    d.fd = (int)n;
    t.parts = g_parts;
    t.cap = sizeof(g_parts) / sizeof(g_parts[0]);
    t.count = 0;
    t.truncated = 0;
    if (gpt_parse(&t, disk_read, &d, d.block) == GPT_OK) {
        for (i = 0; i < t.count; i++) {
            if (memcmp(t.parts[i].unique, h->part_guid, 16) == 0) {
                ok = (t.parts[i].start_lba == h->part_lba &&
                      d.block == h->block_size);
                if (!ok)
                    LOG2("partition found, but not where the map says, on ",
                         name);
                *partno = t.parts[i].number;
                break;
            }
        }
    }
    (void)sys_close(d.fd);
    *block = d.block;
    return ok;
}

static char g_disk[64];

static int find_disk(const extmap_hdr *h, uint32_t *maj, uint32_t *min,
                     uint32_t *block, uint32_t *partno)
{
    static uint8_t dents[4096];
    unsigned waited = 0;

    for (;;) {
        long fd = sys_open("/sys/block", HOOK_O_RDONLY | HOOK_O_DIRECTORY |
                           HOOK_O_CLOEXEC, 0);

        if (fd >= 0) {
            long n;

            while ((n = sys_getdents64((int)fd, dents, sizeof dents)) > 0) {
                long off = 0;

                while (off < n) {
                    /* linux_dirent64: ino u64, off s64, reclen u16, type u8,
                     * name[] */
                    uint16_t reclen = (uint16_t)(dents[off + 16] |
                                                 (dents[off + 17] << 8));
                    const char *name = (const char *)dents + off + 19;

                    if (reclen == 0)
                        break;
                    if (hook_disk_candidate(name) &&
                        disk_matches(name, h, maj, min, block, partno)) {
                        g_disk[0] = '\0';
                        cat(g_disk, sizeof g_disk, name);
                        (void)sys_close((int)fd);
                        LOG2("found the ISO's partition on ", name);
                        return 1;
                    }
                    off += reclen;
                }
            }
            (void)sys_close((int)fd);
        }
        if (waited >= WAIT_MS)
            return 0;
        (void)sys_sleep_ms(POLL_MS);
        waited += POLL_MS;
    }
}

/* The partition device for GPT index partno of disk: the entry under
 * /sys/block/DISK/ whose "partition" file holds that number. 1 with its dev
 * number on success. */
static int find_partition(const char *disk, uint32_t partno, uint32_t *maj,
                          uint32_t *min)
{
    static uint8_t dents[4096];
    char path[128], txt[32];
    long fd, n;
    size_t dl = slen(disk);
    int found = 0;

    path[0] = '\0';
    cat(path, sizeof path, "/sys/block/");
    cat(path, sizeof path, disk);
    fd = sys_open(path, HOOK_O_RDONLY | HOOK_O_DIRECTORY | HOOK_O_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    while (!found && (n = sys_getdents64((int)fd, dents, sizeof dents)) > 0) {
        long off = 0;

        while (!found && off < n) {
            uint16_t reclen = (uint16_t)(dents[off + 16] |
                                         (dents[off + 17] << 8));
            const char *name = (const char *)dents + off + 19;
            long m;
            uint32_t num = 0, i;

            if (reclen == 0)
                break;
            off += reclen;
            if (memcmp(name, disk, dl) != 0)
                continue;
            path[0] = '\0';
            cat(path, sizeof path, "/sys/block/");
            cat(path, sizeof path, disk);
            cat(path, sizeof path, "/");
            cat(path, sizeof path, name);
            cat(path, sizeof path, "/partition");
            m = read_file(path, txt, sizeof txt - 1u);
            if (m <= 0)
                continue;
            for (i = 0; i < (uint32_t)m && txt[i] >= '0' && txt[i] <= '9'; i++)
                num = num * 10u + (uint32_t)(txt[i] - '0');
            if (num != partno)
                continue;
            path[0] = '\0';
            cat(path, sizeof path, "/sys/block/");
            cat(path, sizeof path, disk);
            cat(path, sizeof path, "/");
            cat(path, sizeof path, name);
            cat(path, sizeof path, "/dev");
            m = read_file(path, txt, sizeof txt - 1u);
            if (m > 0 && hook_parse_devnum(txt, (size_t)m, maj, min))
                found = 1;
        }
    }
    (void)sys_close((int)fd);
    return found;
}

/* ---- device mapper ---- */

static int dm_control(void)
{
    long fd = sys_open("/dev/mapper/control", HOOK_O_RDWR | HOOK_O_CLOEXEC, 0);
    char txt[32];
    uint32_t maj, min;
    long n;

    if (fd >= 0)
        return (int)fd;
    /* devtmpfs had no node yet (dm-mod was loaded after it was mounted on
     * some kernels): make one from the misc device's numbers. */
    n = read_file("/sys/class/misc/device-mapper/dev", txt, sizeof txt - 1u);
    if (n <= 0 || !hook_parse_devnum(txt, (size_t)n, &maj, &min))
        return -1;
    (void)sys_mkdir("/dev/mapper", 0755);
    (void)sys_mknod("/dev/mapper/control", HOOK_S_IFCHR | 0600,
                    hook_devnum(maj, min));
    fd = sys_open("/dev/mapper/control", HOOK_O_RDWR | HOOK_O_CLOEXEC, 0);
    return fd >= 0 ? (int)fd : -1;
}

static uint8_t g_table[sizeof(struct dm_ioctl) +
                       EXTMAP_MAX_EXTENTS * (sizeof(struct dm_target_spec) + 48u)];

/* The names the distro will look for, made in devtmpfs directly: the
 * /dev/mapper node for a live-media= argument, and the by-label and by-uuid
 * links an initramfs finds its CD by. udev would make the last two from the
 * device-mapper rules, but only for a device created with a udev cookie, and
 * this one is created before udev exists. devtmpfs is one instance however
 * often it is mounted, so what is made here is still there after the
 * distro's init mounts /dev again. */
static void make_names(uint32_t maj, uint32_t min)
{
    static uint8_t pvd[2048];
    char label[160], uuid[32], path[224];
    long fd;

    (void)sys_mkdir("/dev/mapper", 0755);
    (void)sys_mknod("/dev/mapper/" HOOK_DM_NAME, HOOK_S_IFBLK | 0600,
                    hook_devnum(maj, min));
    fd = sys_open("/dev/mapper/" HOOK_DM_NAME, HOOK_O_RDONLY | HOOK_O_CLOEXEC,
                  0);
    if (fd < 0)
        return;
    if (sys_pread((int)fd, pvd, sizeof pvd, 32768u) != (long)sizeof pvd ||
        !hook_iso_names(pvd, label, sizeof label, uuid, sizeof uuid)) {
        (void)sys_close((int)fd);
        return;
    }
    (void)sys_close((int)fd);
    (void)sys_mkdir("/dev/disk", 0755);
    if (label[0] != '\0') {
        (void)sys_mkdir("/dev/disk/by-label", 0755);
        path[0] = '\0';
        cat(path, sizeof path, "/dev/disk/by-label/");
        cat(path, sizeof path, label);
        (void)sys_symlink("../../mapper/" HOOK_DM_NAME, path);
        LOG2("label ", label);
    }
    if (uuid[0] != '\0') {
        (void)sys_mkdir("/dev/disk/by-uuid", 0755);
        path[0] = '\0';
        cat(path, sizeof path, "/dev/disk/by-uuid/");
        cat(path, sizeof path, uuid);
        (void)sys_symlink("../../mapper/" HOOK_DM_NAME, path);
    }
}

/* How long the udev nudge waits for the distro's udevd before giving up. */
#define UDEV_WAIT_MS 90000u

/* What udev's database would hold for a dm device activated the normal way,
 * through libdevmapper with a udev cookie: the flag 10-dm.rules checks to
 * tell a real activation from a spurious event. */
static const char g_udev_db[] =
    "E:DM_UDEV_PRIMARY_SOURCE_FLAG=1\n"
    "E:DM_UDEV_RULES_VSN=2\n"
    "V:1\n";

/* The device-mapper udev rules (10-dm.rules) treat a dm device that udev
 * first meets on the coldplug "add" as spurious, and record in its database
 * that the disk rules (the by-label link) and every other rule must stay off
 * for it; nothing about this device says otherwise, because it was made
 * before udevd existed and without the cookie libdevmapper would pass. So a
 * child stays behind when PID 1 execs the distro's init, and once that init's
 * udevd has handled the coldplug (its database entry for the device
 * appears), replaces the entry with what a normal activation leaves there and
 * asks the kernel to re-announce the device. The distro's own rules then do
 * the rest: the by-label link, systemd's device unit, and dracut's live-media
 * trigger keyed on the label. Twice, the second a little later, in case the
 * first raced the coldplug; the child is gone within a minute and a half
 * whatever happens. */
static void udev_nudge(uint32_t dm_major, uint32_t dm_minor)
{
    char uevent[96], db[64];
    unsigned waited = 0;
    long pid = sys_fork();

    if (pid != 0)
        return;     /* the parent, or a fork that failed: carry on */

    /* Hold nothing open: a descriptor on the devtmpfs PID 1 mounted would
     * keep it from being unmounted, and a devtmpfs left stacked under the
     * distro's own /dev breaks that /dev's move into the real root. The
     * log is opened per line from here on (logmsg). */
    if (g_kmsg >= 0) {
        (void)sys_close(g_kmsg);
        g_kmsg = -1;
    }
    g_child = 1;

    uevent[0] = '\0';
    cat(uevent, sizeof uevent, "/sys/devices/virtual/block/dm-");
    cat_dec(uevent, sizeof uevent, (long)dm_minor);
    cat(uevent, sizeof uevent, "/uevent");
    db[0] = '\0';
    cat(db, sizeof db, "/run/udev/data/b");
    cat_dec(db, sizeof db, (long)dm_major);
    cat(db, sizeof db, ":");
    cat_dec(db, sizeof db, (long)dm_minor);

    /* udevd up, then its entry for the device written (coldplug done). */
    while (waited < UDEV_WAIT_MS && sys_exists("/run/udev/control") != 0) {
        (void)sys_sleep_ms(200);
        waited += 200u;
    }
    while (waited < UDEV_WAIT_MS && sys_exists(db) != 0) {
        (void)sys_sleep_ms(200);
        waited += 200u;
    }
    if (waited < UDEV_WAIT_MS) {
        int round;

        for (round = 0; round < 2; round++) {
            long fd, rc = -1;

            (void)sys_sleep_ms(round == 0 ? 500u : 5000u);
            fd = sys_open(db, HOOK_O_WRONLY | HOOK_O_CREAT | HOOK_O_TRUNC |
                          HOOK_O_CLOEXEC, 0644);
            if (fd >= 0) {
                (void)sys_write((int)fd, g_udev_db, sizeof g_udev_db - 1u);
                (void)sys_close((int)fd);
            }
            fd = sys_open(uevent, HOOK_O_WRONLY | HOOK_O_CLOEXEC, 0);
            if (fd >= 0) {
                rc = sys_write((int)fd, "change", 6);
                (void)sys_close((int)fd);
            }
            if (rc < 0)
                LOGN("the udev re-announce failed: ", rc);
        }
    }
    sys_exit(0);
}

static int dm_build(const extmap_hdr *h, const uint8_t *map, uint32_t maj,
                    uint32_t min, uint64_t base)
{
    struct dm_ioctl io;
    size_t len = 0;
    long rc;
    int fd = dm_control();

    if (fd < 0) {
        LOG("no device-mapper in this kernel or initramfs");
        return 0;
    }
    (void)hook_dm_header(&io, sizeof io, 1);
    rc = sys_ioctl(fd, DM_DEV_CREATE, &io);
    if (rc < 0) {
        LOGN("creating the device failed: ", rc);
        (void)sys_close(fd);
        return 0;
    }
    if (hook_dm_table(h, map, maj, min, base, g_table, sizeof g_table,
                      &len) != HOOK_OK) {
        LOG("the extent map cannot be expressed as a table");
        (void)hook_dm_header(&io, sizeof io, 0);
        (void)sys_ioctl(fd, DM_DEV_REMOVE, &io);
        (void)sys_close(fd);
        return 0;
    }
    rc = sys_ioctl(fd, DM_TABLE_LOAD, g_table);
    if (rc >= 0) {
        (void)hook_dm_header(&io, sizeof io, 0);
        rc = sys_ioctl(fd, DM_DEV_SUSPEND, &io);   /* no flag: resume */
    }
    if (rc < 0) {
        LOGN("loading the table failed: ", rc);
        (void)hook_dm_header(&io, sizeof io, 0);
        (void)sys_ioctl(fd, DM_DEV_REMOVE, &io);
        (void)sys_close(fd);
        return 0;
    }
    (void)sys_close(fd);
    /* The resume answers with the device's number in io.dev. */
    {
        uint32_t dmaj = (uint32_t)(((io.dev >> 8) & 0xfffu) |
                                   ((io.dev >> 32) & ~0xfffu));
        uint32_t dmin = (uint32_t)((io.dev & 0xffu) |
                                   ((io.dev >> 12) & ~0xffu));

        make_names(dmaj, dmin);
        udev_nudge(dmaj, dmin);
    }
    return 1;
}

/* No device-mapper (Alpine's initramfs has loop and no dm-mod): an ISO in
 * one piece, which is how a copy onto a stick with room lands, is a loop
 * device at its offset over the same partition, read-only. A fragmented one
 * has no such form and is left to fail as it would have. */
static int loop_build(const extmap_hdr *h, const uint8_t *map, uint32_t maj,
                      uint32_t min, uint64_t base)
{
    static const char part[] = "/dev/.falcon-part";
    struct loop_info64 info;
    extmap_extent e;
    char node[40];
    long ctl, num, lfd, bfd, rc;

    if (h->n_extents != 1) {
        LOGN("no device-mapper, and the ISO is in pieces: ", h->n_extents);
        return 0;
    }
    extmap_extent_at(map, 0, &e);
    ctl = sys_open("/dev/loop-control", HOOK_O_RDWR | HOOK_O_CLOEXEC, 0);
    if (ctl < 0) {
        LOG("no loop devices in this kernel or initramfs either");
        return 0;
    }
    num = sys_ioctl((int)ctl, LOOP_CTL_GET_FREE, (void *)0);
    (void)sys_close((int)ctl);
    if (num < 0) {
        LOGN("no free loop device: ", num);
        return 0;
    }
    node[0] = '\0';
    cat(node, sizeof node, "/dev/loop");
    cat_dec(node, sizeof node, num);
    (void)sys_mknod(node, HOOK_S_IFBLK | 0600, hook_devnum(7u, (uint32_t)num));
    lfd = sys_open(node, HOOK_O_RDONLY | HOOK_O_CLOEXEC, 0);
    (void)sys_mknod(part, HOOK_S_IFBLK | 0600, hook_devnum(maj, min));
    bfd = sys_open(part, HOOK_O_RDONLY | HOOK_O_CLOEXEC, 0);
    (void)sys_unlink(part);
    if (lfd < 0 || bfd < 0) {
        LOG("the loop device or the stick could not be opened");
        if (lfd >= 0)
            (void)sys_close((int)lfd);
        if (bfd >= 0)
            (void)sys_close((int)bfd);
        return 0;
    }
    rc = sys_ioctl((int)lfd, LOOP_SET_FD, (void *)(uintptr_t)bfd);
    if (rc >= 0) {
        memset(&info, 0, sizeof info);
        info.lo_offset = base + e.offset;
        info.lo_sizelimit = h->iso_size;
        info.lo_flags = LO_FLAGS_READ_ONLY;
        rc = sys_ioctl((int)lfd, LOOP_SET_STATUS64, &info);
        if (rc < 0)
            (void)sys_ioctl((int)lfd, LOOP_CLR_FD, (void *)0);
    }
    (void)sys_close((int)bfd);
    (void)sys_close((int)lfd);
    if (rc < 0) {
        LOGN("setting up the loop device failed: ", rc);
        return 0;
    }
    make_names(7u, (uint32_t)num);
    LOG2("the ISO is ", node);
    return 1;
}

/* The pre-mounts of falcon-hook/src/hookcore.c: the ISO mounted where an
 * initramfs that looks there first will find it. */
static void apply_premounts(const char *node)
{
    size_t count = 0, i;
    const hook_premount *p = hook_premounts(&count);

    for (i = 0; i < count; i++) {
        long rc;

        if (sys_exists(p[i].marker) != 0)
            continue;
        (void)sys_mkdir(p[i].dir, 0755);
        rc = sys_mount(node, p[i].dir, "iso9660", 1ul /* MS_RDONLY */,
                       (const void *)0);
        if (rc < 0)
            LOGN("could not mount the ISO for the installer: ", rc);
        else
            LOG2("mounted the ISO for ", p[i].what);
    }
}

/* The table in falcon-hook/src/hookcore.c, applied to the initramfs the
 * distro is about to run. */
static char g_text[256u * 1024u];
static char g_patched[256u * 1024u + 512u];

static void apply_nudges(void)
{
    size_t count = 0, i, out = 0;
    const hook_nudge *n = hook_nudges(&count);

    for (i = 0; i < count; i++) {
        long len = read_file(n[i].path, g_text, sizeof g_text);
        long fd;

        if (len <= 0 || (size_t)len >= sizeof g_text)
            continue;
        if (hook_patch(g_text, (size_t)len, n[i].find, n[i].replace,
                       g_patched, sizeof g_patched, &out) != 1)
            continue;
        fd = sys_open(n[i].path, HOOK_O_WRONLY | HOOK_O_TRUNC |
                      HOOK_O_CLOEXEC, 0);
        if (fd < 0 || sys_write((int)fd, g_patched, out) != (long)out) {
            LOG2("could not nudge ", n[i].what);
        } else {
            LOG2("nudged ", n[i].what);
        }
        if (fd >= 0)
            (void)sys_close((int)fd);
    }
}

/* ---- the whole ---- */

static uint8_t g_map[MAP_FILE_CAP];
static char g_cmdline[4096];

int hook_main(int argc, char **argv, char **envp)
{
    char init[256];
    char *nargv[2];
    extmap_hdr h;
    const uint8_t *data;
    size_t dlen = 0;
    long n;
    uint32_t maj = 0, min = 0, block = 0, partno = 0;

    (void)argc;
    mount_one(M_PROC);
    mount_one(M_SYS);
    mount_one(M_DEV);
    g_kmsg = (int)sys_open("/dev/kmsg", HOOK_O_WRONLY | HOOK_O_CLOEXEC, 0);

    init[0] = '\0';
    n = read_file("/proc/cmdline", g_cmdline, sizeof g_cmdline - 1u);
    if (n > 0)
        g_cmdline[n] = '\0';
    if (n <= 0 || !hook_cmdline_arg(g_cmdline, (size_t)n, HOOK_INIT_ARG, init,
                                    sizeof init))
        cat(init, sizeof init, HOOK_DEFAULT_INIT);

    /* The map the loader put in the initrd (a direct boot), else the one
     * it published in FalconIsoMap (a blessed wrap, built before the ISO's
     * extents were known). */
    n = read_file(HOOKCMD_MAP_PATH, g_map, sizeof g_map);
    if (n > 0) {
        data = g_map;
        dlen = (size_t)n;
    } else {
        mount_one(M_EFIVARS);
        n = read_file("/sys/firmware/efi/efivars/" FALCON_EXTMAP_VAR "-"
                      FALCON_EXTMAP_GUID_STR, g_map, sizeof g_map);
        if (n <= 0 && !g_mounted[M_EFIVARS]) {
            /* efivarfs may be a module; load drivers first and try again. */
            load_modules(envp);
            mount_one(M_EFIVARS);
            n = read_file("/sys/firmware/efi/efivars/" FALCON_EXTMAP_VAR "-"
                          FALCON_EXTMAP_GUID_STR, g_map, sizeof g_map);
        }
        data = hook_efivar_data(g_map, n > 0 ? (size_t)n : 0u, &dlen);
    }
    if (data == (const uint8_t *)0 || extmap_decode(data, dlen, &h) != EXTMAP_OK) {
        LOG("no ISO map from the loader; handing over unchanged");
    } else {
        LOGN("ISO map read, extents: ", h.n_extents);
        load_modules(envp);
        if (!find_disk(&h, &maj, &min, &block, &partno)) {
            LOG("the stick's partition never appeared");
        } else {
            uint32_t pmaj = 0, pmin = 0;
            uint64_t base = 0;

            if (find_partition(g_disk, partno, &pmaj, &pmin)) {
                maj = pmaj;
                min = pmin;
            } else {
                LOG("no partition device; mapping over the whole disk");
                base = h.part_lba * block;
            }
            int built = dm_build(&h, data, maj, min, base);

            if (built) {
                LOG("/dev/mapper/" HOOK_DM_NAME " is the ISO");
            } else {
                built = loop_build(&h, data, maj, min, base);
                /* Neither driver in this initramfs: the ISO's own package
                 * pool may carry loop for this kernel. */
                if (!built && dimod_bring("loop-modules-", "loop", &h, data,
                                          maj, min, base, envp)) {
                    LOG("loaded loop from the ISO's own packages");
                    built = loop_build(&h, data, maj, min, base);
                }
            }
            if (built) {
                apply_nudges();
                apply_premounts("/dev/mapper/" HOOK_DM_NAME);
            }
        }
    }

    LOG2("starting ", init);
    unmount_all();
    nargv[0] = init;
    nargv[1] = (char *)0;
    if (argv != (char **)0 && argv[0] != (char *)0 && argv[1] != (char *)0) {
        /* Arguments the kernel passed to init (cmdline words without '=')
         * go to the distro's init unchanged. */
        argv[0] = init;
        (void)sys_execve(init, argv, envp);
    }
    (void)sys_execve(init, nargv, envp);
    /* Nothing left to hand over to: the initramfs has no init at that path.
     * Returning from PID 1 panics the kernel, which is the honest outcome. */
    return 1;
}
