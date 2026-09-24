/* falcon-bios: reading an ISO on p2. See scan.c. */

#ifndef FALCON_BIOS_SCAN_H
#define FALCON_BIOS_SCAN_H

#include <bios.h>
#include <result.h>
#include <fs/fs.h>
#include <part/part.h>
#include <media/family.h>

/* What the ISO named by `ent` on the mounted p2 volume is, from its own
 * filesystem; NULL when nothing on it could be read. What it found is logged
 * to serial. Decides the route, as the UEFI loader's survey does. */
const family_desc *scan_family(fs_vol *p2, const fs_dirent *ent);

/* Boots the ISO's default boot entry directly, with falcon-hook and its
 * extent map appended when p2's GPT entry is known (DESIGN Section 6.4).
 * Returns only when it did not boot, with *r filled in; r->title is the
 * entry, once one was chosen. */
int scan_direct(fs_vol *p2, const gpt_part *p2part, const fs_dirent *ent,
                uint32_t block_size, boot_result *r);

/* Serves the same ISO to its own bootloader through a resident INT 13h
 * handler (vcd.c). Returns only when it did not boot, with *r filled in. */
int scan_chain(bios_disk *d, fs_vol *p2, const gpt_part *p2part,
               const fs_dirent *ent, boot_result *r);

#endif /* FALCON_BIOS_SCAN_H */
