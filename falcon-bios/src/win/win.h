/* falcon-bios: Windows install media, patched at boot. See win.c. */

#ifndef FALCON_BIOS_WIN_H
#define FALCON_BIOS_WIN_H

#include <start/bios.h>
#include <result.h>
#include <fs/fs.h>
#include <part/part.h>

/* Patches the medium's boot.wim in memory, lays the result over the served
 * medium, and chainloads it. `iso_rel` is the ISO's path as Windows will see
 * it on p2, which is what falcon-winjump is told to attach.
 *
 * Returns only when it did not boot, with *r filled in. `*unpatchable` is
 * set when the medium is not Windows install media at all, which is a cue to
 * chainload it plainly rather than a failure. */
int win_boot(bios_disk *d, fs_vol *p2, const gpt_part *p2part,
             const fs_dirent *iso, const char *iso_rel, boot_result *r,
             int *unpatchable);

/* Windows 2000/XP install media (no WinPE): injects the virtual-CD driver
 * (falcon-vdisk) into text-mode setup by editing the served ISO's
 * i386/txtsetup.sif and adding i386/_FALCON.SYS, then chainloads it. Returns
 * only when it did not boot, with *r filled in. `*not_xp` is set when the
 * medium is not 2000/XP media, a cue to boot it plainly rather than a failure.
 * (windows-route.md 7.4). */
int win_xp_boot(bios_disk *d, fs_vol *p2, const gpt_part *p2part,
                const fs_dirent *iso, boot_result *r, int *not_xp);

#endif /* FALCON_BIOS_WIN_H */
