/* falcon-boot: an ISO's boot files, read through a mounted filesystem
 * (docs/DESIGN.md Section 6.4; docs/notes/coverage-roadmap.md W2 D1).
 *
 * bootcfg (falcon-core/src/media/bootcfg.c) reads a configuration through
 * three callbacks and never names a filesystem. This is those callbacks over
 * the registry: the loader mounts the ISO file on p2 with the ISO9660 or UDF
 * driver, and the entry bootcfg picks is read out of the same mount, kernel
 * and initrds included. The sign tool has its own provider over its index of
 * the image (falcon-cli/src/sign/isoprobe.c), which answers the same
 * questions about the same bytes.
 *
 * Every lookup walks from the root (fs_lookup), so a path is resolved the same
 * way whichever callback asks, and names match ASCII-case-insensitively, which
 * is what an ISO9660 name space without Rock Ridge needs.
 */

#ifndef FALCON_BOOTFS_H
#define FALCON_BOOTFS_H

#include <stddef.h>
#include <stdint.h>

#include <fs/fs.h>
#include <media/bootcfg.h>

/* The iterators and handles a lookup needs, one set per mounted image.
 * Large (a few kilobytes each); the caller places it. */
typedef struct {
    fs_vol   *vol;
    fs_dir    walk;    /* fs_lookup's scratch */
    fs_dir    list;    /* the directory a list call is enumerating */
    fs_file   file;    /* the file a read call has open */
    fs_dirent ent;
} bootfs_ctx;

/* Points *src at ctx, which reads vol. vol must stay mounted, and ctx must
 * stay put, for as long as src is used. */
void bootfs_src(bootfs_ctx *ctx, fs_vol *vol, bootcfg_src *src);

/* Opens the regular file at path into *f (a handle the caller owns, so the
 * kernel and an initrd can be open at once). FS_OK with *size its length;
 * FS_ERR_NOT_FOUND for a missing path or one that is not a regular file; any
 * other status as the driver reported it. */
int bootfs_open(bootfs_ctx *ctx, const char *path, fs_file *f,
                uint64_t *size);

#endif /* FALCON_BOOTFS_H */
