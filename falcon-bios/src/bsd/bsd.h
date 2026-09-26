/* falcon-bios: BSD media served as its own memory disk. See bsd.c. */

#ifndef FALCON_BIOS_BSD_H
#define FALCON_BIOS_BSD_H

#include <start/bios.h>
#include <result.h>
#include <fs/fs.h>
#include <part/part.h>

/* Lays media/bsdmd.c's two edits over the served medium and chainloads it,
 * so the BSD loader inside mounts root from a memory disk.
 *
 * Returns only when it did not boot, with *r filled in. `*not_bsd` is set
 * when the medium is not one this route applies to at all, which is a cue to
 * boot it another way rather than a failure. */
int bsd_boot(bios_disk *d, fs_vol *p2, const gpt_part *p2part,
             const fs_dirent *iso, boot_result *r, int *not_bsd);

#endif /* FALCON_BIOS_BSD_H */
