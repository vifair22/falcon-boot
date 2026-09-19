/* falcon-boot appliance: finding the stick, and its p2 and p3. */

#include "appliance.h"

#include <part/part.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---- partition discovery ---- */

static int disk_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    int fd = *(const int *)ctx;
    ssize_t n = pread(fd, buf, (size_t)len, (off_t)off);

    return (n == (ssize_t)len) ? 0 : -1;
}

/* /dev/sda + 3 -> /dev/sda3, /dev/nvme0n1 + 3 -> /dev/nvme0n1p3. The rule is
 * the kernel's: a trailing digit in the disk name takes a 'p' separator. */
static void part_node(char *out, size_t cap, const char *disk, uint32_t number)
{
    size_t n = strlen(disk);
    const char *sep = (n > 0 && disk[n - 1u] >= '0' && disk[n - 1u] <= '9')
                          ? "p" : "";

    (void)snprintf(out, cap, "/dev/%s%s%u", disk, sep, number);
}

/* Block devices that cannot be the stick: no loopback, no ramdisk, no
 * device-mapper node (never the stick itself), no optical
 * drive. Everything else gets a GPT read. */
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

/* Reads the first `len` bytes of a device. Used by the fallback probe and by
 * the exFAT volume-serial read; both want a boot sector and nothing else. */
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

static int looks_like_luks(const char *node)
{
    uint8_t hdr[8];
    static const uint8_t magic[6] = { 'L', 'U', 'K', 'S', 0xba, 0xbe };

    if (read_head(node, hdr, sizeof(hdr)) != 0)
        return 0;
    return memcmp(hdr, magic, sizeof(magic)) == 0;
}

static int looks_like_exfat(const char *node)
{
    uint8_t vbr[16];

    if (read_head(node, vbr, sizeof(vbr)) != 0)
        return 0;
    return memcmp(vbr + 3, "EXFAT   ", 8) == 0;
}

/* Identification proper: DESIGN Section 3's rule (p3 by its falcon type GUID,
 * p2 by Microsoft basic data plus the name FALCON-ISO), applied by the
 * loader's own matcher over a GPT read by the loader's own parser. Returns
 * nonzero when both partitions were found on one disk; a stick always has
 * both on one disk, and accepting a p2 from one device and a p3 from another
 * would be a way to get a key from one stick to bless media on somebody
 * else's. */
static int find_by_ident(const char *disk, stick_parts *out)
{
    char node[NODE_MAX];
    gpt_part parts[128];
    gpt_table t;
    int fd;
    int rc;
    uint32_t i;
    int have2 = 0;
    int have3 = 0;

    (void)snprintf(node, sizeof(node), "/dev/%s", disk);
    fd = open(node, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;

    t.parts = parts;
    t.cap = (uint32_t)(sizeof(parts) / sizeof(parts[0]));
    t.count = 0;
    t.truncated = 0;

    rc = gpt_parse(&t, disk_read, &fd, 512);
    (void)close(fd);
    if (rc != GPT_OK)
        return 0;

    for (i = 0; i < t.count; i++) {
        if (gpt_part_is_role(&parts[i], PART_ROLE_ISO)) {
            part_node(out->p2, sizeof(out->p2), disk, parts[i].number);
            have2 = 1;
        } else if (gpt_part_is_role(&parts[i], PART_ROLE_SHELF)) {
            part_node(out->p3, sizeof(out->p3), disk, parts[i].number);
            have3 = 1;
        }
    }
    return have2 && have3;
}

/* Fallback for a stick that predates this identification scheme (and for the
 * rig's older fixtures): take the first exFAT partition as p2 and the first LUKS
 * partition as p3, on a disk that carries both. Content probing rather than
 * naming, and deliberately narrower than the loader's own fallback: this
 * program is about to ask for a passphrase, and the honest failure is "no
 * falcon stick found", not a prompt aimed at some unrelated volume. */
static int find_by_content(const char *disk, stick_parts *out)
{
    char dir[PATH_MAX];
    DIR *d;
    struct dirent *e;
    int have2 = 0;
    int have3 = 0;

    (void)snprintf(dir, sizeof(dir), "/sys/block/%s", disk);
    d = opendir(dir);
    if (d == NULL)
        return 0;

    while ((e = readdir(d)) != NULL) {
        char node[NODE_MAX];

        if (e->d_name[0] == '.')
            continue;
        if (strncmp(e->d_name, disk, strlen(disk)) != 0)
            continue;   /* only this disk's partition directories */
        (void)snprintf(node, sizeof(node), "/dev/%s", e->d_name);
        if (!have2 && looks_like_exfat(node)) {
            (void)snprintf(out->p2, sizeof(out->p2), "%s", node);
            have2 = 1;
        } else if (!have3 && looks_like_luks(node)) {
            (void)snprintf(out->p3, sizeof(out->p3), "%s", node);
            have3 = 1;
        }
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

static int find_stick_once(stick_parts *out)
{
    DIR *d = opendir("/sys/block");
    struct dirent *e;
    int found = 0;

    memset(out, 0, sizeof(*out));
    if (d == NULL) {
        say("ERROR: /sys/block is not readable; is sysfs mounted?\n");
        return 0;
    }
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || skip_disk(e->d_name))
            continue;
        if (find_by_ident(e->d_name, out)) {
            say("stick: %s (falcon GPT identification)\n", e->d_name);
            found = 1;
        }
    }
    if (!found) {
        rewinddir(d);
        while (!found && (e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.' || skip_disk(e->d_name))
                continue;
            if (find_by_content(e->d_name, out)) {
                say("stick: %s (untyped GPT; exFAT and LUKS volumes probed)\n",
                    e->d_name);
                found = 1;
            }
        }
    }
    (void)closedir(d);
    return found;
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
    unsigned waited = 0;
    int said = 0;

    for (;;) {
        if (find_stick_once(out))
            return 1;
        if (waited >= STICK_WAIT_MS)
            return 0;
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
