/* falcon-boot appliance: finding the stick, and its p2 and p3. */

#include "appliance.h"
#include "stickid.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---- partition discovery (DESIGN Section 3.1) ---- */

static int part_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    int fd = *(const int *)ctx;
    ssize_t n = pread(fd, buf, (size_t)len, (off_t)off);

    return (n == (ssize_t)len) ? 0 : -1;
}

/* Block devices that cannot be the stick: no loopback, no ramdisk, no
 * device-mapper node (never the stick itself), no optical
 * drive. */
static int skip_disk(const char *name)
{
    static const char *const pre[] = { "loop", "ram", "dm-", "md", "sr", "zram" };
    size_t i;

    for (i = 0; i < sizeof(pre) / sizeof(pre[0]); i++) {
        if (strncmp(name, pre[i], strlen(pre[i])) == 0)
            return 1;
    }
    return 0;
}

/* Reads the first `len` bytes of a device, for the exFAT volume-serial
 * read, which wants a boot sector and nothing else. */
static int read_head(const char *node, void *buf, size_t len)
{
    int fd = open(node, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, buf, len);
    (void)close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

/* The install ID the loader handed over (stickid.h). */
static int install_id(ident_rec *out)
{
    char buf[IDENT_TEXT_MAX];
    int fd = open("/.extra/credentials/" IDENT_CRED, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof buf);
    (void)close(fd);
    if (n <= 0 || (size_t)n >= sizeof buf)
        return 0;
    return stickid_from_cred(buf, (size_t)n, out) == IDENT_OK;
}

/* One partition: read its record and take it if it is the stick's. */
static void judge_node(const char *node, const ident_rec *install,
                       stick_parts *out, int *have2, int *have3)
{
    ident_rec rec;
    int luks = 0;
    int fd = open(node, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return;
    if (stickid_read(part_read, &fd, &rec, &luks) == IDENT_OK) {
        switch (stickid_judge(install, &rec, luks)) {
        case STICKID_P2:
            if (!*have2)
                (void)snprintf(out->p2, sizeof(out->p2), "%s", node);
            *have2 = 1;
            break;
        case STICKID_P3:
            if (!*have3)
                (void)snprintf(out->p3, sizeof(out->p3), "%s", node);
            *have3 = 1;
            break;
        case STICKID_NONE:
        default:
            break;
        }
    }
    (void)close(fd);
}

/* Every partition of every disk, by /sys/block/DISK/PART: the record says
 * which are the stick's, wherever they are. */
static int find_by_record(const ident_rec *install, stick_parts *out)
{
    DIR *d = opendir("/sys/block");
    struct dirent *e;
    int have2 = 0;
    int have3 = 0;

    if (d == NULL) {
        say("ERROR: /sys/block is not readable; is sysfs mounted?\n");
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        char dir[PATH_MAX];
        DIR *pd;
        struct dirent *pe;

        if (e->d_name[0] == '.' || skip_disk(e->d_name))
            continue;
        (void)snprintf(dir, sizeof(dir), "/sys/block/%s", e->d_name);
        pd = opendir(dir);
        if (pd == NULL)
            continue;
        while ((pe = readdir(pd)) != NULL) {
            char node[NODE_MAX];

            /* Only this disk's partition directories. */
            if (strncmp(pe->d_name, e->d_name, strlen(e->d_name)) != 0)
                continue;
            (void)snprintf(node, sizeof(node), "/dev/%s", pe->d_name);
            judge_node(node, install, out, &have2, &have3);
        }
        (void)closedir(pd);
    }
    (void)closedir(d);
    return have2 && have3;
}

/* Names every disk the kernel currently has, so a failure to find the stick
 * says what WAS there. Without this the message is the same whether no bus
 * driver loaded, the stick is genuinely absent, or its partitions are not
 * ours, and those need different answers from the user. */
void say_block_devices(void)
{
    DIR *d = opendir("/sys/block");
    struct dirent *e;
    int n = 0;

    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || skip_disk(e->d_name))
            continue;
        say("%s %s", n == 0 ? "       disks seen:" : ",", e->d_name);
        n++;
    }
    (void)closedir(d);
    say(n == 0 ? "       disks seen: none at all\n" : "\n");
}

/* The stick does not exist yet when this program starts. finit_module returns
 * as soon as the module is in the kernel, not when the bus it drives has
 * finished enumerating, and USB mass storage takes a second or more to probe,
 * attach a SCSI device and publish /sys/block/sdX. There is no udev in here to
 * wait on, so waiting is this loop.
 *
 * The rig never showed it: an emulated SATA disk is present before init runs,
 * so a single pass always succeeded there and the race was invisible until the
 * appliance met a real USB stick (2026-09-13) and reported no stick found.
 *
 * Bounded by wall clock rather than attempt count, for the same reason the
 * same guard in falcon-winjump is: one pass costs whatever its slowest device
 * costs, so a count expires in unpredictable real time. A stick that is
 * genuinely absent should end in a legible message, not a hang, which is why
 * this gives up at all.
 */
#define STICK_WAIT_MS 30000
#define STICK_POLL_MS 250

int find_stick(stick_parts *out)
{
    ident_rec install;
    unsigned waited = 0;
    int said = 0;

    memset(out, 0, sizeof(*out));
    if (!install_id(&install))
        return STICK_NO_ID;
    for (;;) {
        if (find_by_record(&install, out))
            return STICK_FOUND;
        if (waited >= STICK_WAIT_MS)
            return STICK_NOT_FOUND;
        if (!said) {
            say("waiting for the stick to enumerate...\n");
            said = 1;
        }
        (void)usleep(STICK_POLL_MS * 1000u);
        waited += STICK_POLL_MS;
    }
}

/* p2's volume serial, in the form blkid reports it, because that is what the
 * family cmdline templates mean by %DEV% and what the booted initramfs will
 * resolve. Read from the exFAT VBR rather than asked of blkid: there is no
 * udev in here to maintain /dev/disk/by-uuid, which is where the host route's
 * lookup gets it from (falcon-cli/src/sign/util.c). */
int exfat_uuid(const char *node, char *out, size_t cap)
{
    uint8_t vbr[512];
    uint32_t serial;

    if (read_head(node, vbr, sizeof(vbr)) != 0)
        return -1;
    if (memcmp(vbr + 3, "EXFAT   ", 8) != 0)
        return -1;
    serial = (uint32_t)vbr[100] | ((uint32_t)vbr[101] << 8) |
             ((uint32_t)vbr[102] << 16) | ((uint32_t)vbr[103] << 24);
    if (snprintf(out, cap, "%04X-%04X", (unsigned)(serial >> 16),
                 (unsigned)(serial & 0xFFFFu)) >= (int)cap)
        return -1;
    return 0;
}
