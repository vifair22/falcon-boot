/* falcon-bios: Windows install media, patched at boot. See win.c. */

#ifndef FALCON_BIOS_WIN_H
#define FALCON_BIOS_WIN_H

#include <bios.h>
#include <fs/fs.h>
#include <part/part.h>

/* Patches the medium's boot.wim in memory, lays the result over the served
 * medium, and chainloads it. `iso_rel` is the ISO's path as Windows will see
 * it on p2, which is what falcon-winjump is told to attach.
 *
 * Returns 0 only when it did not boot. `*unpatchable` is set when the medium
 * is not Windows install media at all, which is a cue to chainload it
 * plainly rather than a failure. */
int win_boot(bios_disk *d, fs_vol *p2, const gpt_part *p2part,
             const fs_dirent *iso, const char *iso_rel, int *unpatchable);

#endif /* FALCON_BIOS_WIN_H */
