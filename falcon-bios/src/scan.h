/* falcon-bios: reading an ISO on p2. See scan.c. */

#ifndef FALCON_BIOS_SCAN_H
#define FALCON_BIOS_SCAN_H

#include <bios.h>
#include <fs/fs.h>
#include <part/part.h>

/* Mounts the ISO named by `ent` on the mounted p2 volume, reports what it
 * holds to the screen, and boots it. `path` is its path from p2's root (the
 * survey's label), which is how Windows finds it again (win.c). `p2part` is
 * p2's own GPT entry, which is what names the partition to falcon-hook; NULL
 * boots without the hook, and the medium is then the kernel's problem.
 * Returns 0 only when it did not boot, since a boot does not return. */
int scan_iso(fs_vol *p2, const gpt_part *p2part, const fs_dirent *ent,
             const uint16_t *path, uint32_t block_size);

/* Serves the same ISO to its own bootloader through a resident INT 13h
 * handler and hands the machine over (vcd.c). Returns 0 only when it did
 * not: a chainload does not return. */
int scan_chain(bios_disk *d, fs_vol *p2, const gpt_part *p2part,
               const fs_dirent *ent);

/* The disk p2 is on. Set once, before any scan: the BSD route needs it to
 * install its handler and nothing else here does. */
void scan_disk(bios_disk *d);

#endif /* FALCON_BIOS_SCAN_H */
