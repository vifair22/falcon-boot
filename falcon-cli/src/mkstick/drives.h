/* falcon-boot mkstick: the drives interactive `create` offers (DESIGN
 * Section 8).
 *
 * Every whole disk the kernel lists under <sys>/block, bar the kinds that
 * are never a stick (loop, ram, zram, optical, floppy, device-mapper, md,
 * nbd) and empty card-reader slots. Removable and USB disks come first.
 *
 * A disk is offered only when nothing on this machine is using it: none of
 * it, its partitions, or anything stacked on them (LUKS, LVM, md) is
 * mounted, swapped on, or held open by a mapping. The disk holding this
 * machine's root filesystem is named as such.
 *
 * The paths are parameters so a test can hand it a tree of its own.
 */

#ifndef FALCON_MKSTICK_DRIVES_H
#define FALCON_MKSTICK_DRIVES_H

#include <stddef.h>
#include <stdint.h>

#include <common/status.h>
#include <mkstick/disk.h>

#define DRV_MAX       32u
#define DRV_MAX_PARTS 16u

typedef struct {
    const char *sys;     /* /sys */
    const char *dev;     /* /dev: a disk's node is <dev>/<name> */
    const char *mounts;  /* /proc/mounts */
    const char *swaps;   /* /proc/swaps */
} drv_env;

typedef struct {
    char     name[32];               /* sdb, nvme1n1 */
    char     path[MKS_PATH_MAX];     /* <dev>/<name> */
    char     model[128];             /* vendor and model, as sysfs has them */
    uint64_t size;                   /* bytes */
    int      removable;
    int      usb;
    unsigned n_parts;
    char     part[DRV_MAX_PARTS][40];
    uint64_t part_size[DRV_MAX_PARTS];
    /* Holds this machine's root filesystem. */
    int      system;
    /* Why it cannot be chosen ("mounted at /home", "used as swap", "held
     * by dm-3"), or empty when it can. */
    char     busy[160];
} drv_entry;

/* The live machine's paths. */
void drv_env_default(drv_env *env);

/* Fills out[0..*n) and sorts it: removable or USB first, then by name. */
cli_status drv_list(const drv_env *env, drv_entry *out, size_t cap,
                    size_t *n);

#endif /* FALCON_MKSTICK_DRIVES_H */
