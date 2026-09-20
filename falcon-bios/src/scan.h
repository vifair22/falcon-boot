/* falcon-bios: reading an ISO on p2. See scan.c. */

#ifndef FALCON_BIOS_SCAN_H
#define FALCON_BIOS_SCAN_H

#include <fs/fs.h>

/* Mounts the ISO named by `ent` on the mounted p2 volume, reports what it
 * holds to the screen, and returns 0 when it found a boot entry. */
int scan_iso(fs_vol *p2, const fs_dirent *ent);

#endif /* FALCON_BIOS_SCAN_H */
