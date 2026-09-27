/* falcon-boot mkstick: the target, whether it is a USB stick or an image file.
 *
 * One abstraction over two things that behave almost but not quite alike:
 *
 *   - a block device, where the partitions the kernel publishes are named by
 *     appending to the device name, and where getting the target wrong
 *     destroys somebody's disk;
 *   - a regular file, which has to be attached through a loop device before
 *     anything can format a partition inside it, and which is what the test
 *     rig and the acceptance run use.
 *
 * The partition-name shapes are the reason this is a function and not a
 * sprintf at each call site: /dev/sda gives sda1, but /dev/nvme0n1,
 * /dev/mmcblk0 and /dev/loop0 all give <name>p1. The rule the kernel actually
 * uses is "insert a p when the base name ends in a digit", and that is what
 * is implemented here.
 */

#ifndef FALCON_MKSTICK_DISK_H
#define FALCON_MKSTICK_DISK_H

#include <stddef.h>
#include <stdint.h>

#include <common/status.h>

#define MKS_PATH_MAX 4096u
#define MKS_PARTS       4u

typedef struct {
    char     path[MKS_PATH_MAX];       /* what the user named */
    int      is_block;
    uint64_t size;                     /* bytes */
    uint32_t sector;                   /* logical sector size */

    /* Set by disk_attach: the device whose partitions we format. For a block
     * device it is `path` itself; for a file it is the loop device. */
    char     base[MKS_PATH_MAX];
    char     part[MKS_PARTS][MKS_PATH_MAX];
    int      loop_attached;
} mks_disk;

/* Identify and size the target. Does not open it for writing and does not
 * attach anything. */
cli_status disk_stat(mks_disk *d, const char *path);

/* Refuse a target that the kernel says is in use: the device itself or any
 * of its partitions appearing in /proc/mounts. Cheap, and the one check that
 * stands between a mistyped device name and a live root filesystem. */
cli_status disk_check_not_mounted(const mks_disk *d);

/* Print model, size and partition names, so the user can recognise the device
 * they are about to destroy before --force is honoured. */
void disk_describe(const mks_disk *d);

/* Attach (loop for files, identity for block devices) and fill in the
 * partition device names. Registers the loop device for cleanup. */
cli_status disk_attach(mks_disk *d);

/* Detach a loop device attached by disk_attach, popping its cleanup entry. */
cli_status disk_detach(mks_disk *d);

/* Wait for the kernel and udev to publish the partition nodes. Without it the
 * first mkfs races udev and fails with a confusing ENOENT. */
cli_status disk_settle(const mks_disk *d);

/* The same wait for one partition node, for a disk whose partitions are not
 * the four of a stick (a station's, or any table status reads). */
cli_status disk_settle_node(const char *node);

cli_status disk_partition_name(const char *base, unsigned n,
                               char *out, size_t cap);

/* ---- byte reads over a file or a partition of one ----
 *
 * Shaped exactly like gpt_read_fn / luks_read_fn / exfat_read_fn, which is what
 * lets `status` run the loader's own parsers (falcon-core/src/part/part.c,
 * falcon-core/src/luks, falcon-core/src/fs/exfat.c) over an image file with no
 * EFI anywhere in sight.
 */
typedef struct {
    int      fd;
    uint64_t base;   /* byte offset of this window within the file */
    uint64_t len;    /* window length; 0 means "to the end" */
    char     name[MKS_PATH_MAX];
} disk_io;

cli_status disk_io_open(disk_io *io, const char *path, uint64_t base,
                        uint64_t len);
void       disk_io_close(disk_io *io);
int        disk_io_read(void *ctx, uint64_t off, uint64_t len, void *buf);

#endif /* FALCON_MKSTICK_DISK_H */
