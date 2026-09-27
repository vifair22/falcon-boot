/* falcon-bios: booting the personal OS from the shelf. See pos.c. */

#ifndef FALCON_BIOS_POS_H
#define FALCON_BIOS_POS_H

#include <start/bios.h>
#include <result.h>
#include <part/table.h>

/* The user backed out, at the prompt or at the chooser. Nothing is on screen
 * worth a keypress. */
#define POS_ABORTED 1

/* Unlocks the shelf (a plaintext one needs no unlocking), offers the UKIs
 * on it, and boots the one chosen, drawing in the page the caller started. Does not return when the boot happens.
 * POS_ABORTED when the user backed out; -1 with *r filled in when it failed.
 * Every secret is wiped either way. */
int pos_run(bios_disk *d, const part_entry *shelf, boot_result *r);

/* Whether the shelf opens as LUKS2, or mounts as a plaintext filesystem, so
 * the menu knows whether to offer the row. Reads a header or a superblock;
 * asks for nothing. Which partition is the install's shelf is the caller's
 * to decide, by its record (disk/install.h). */
int pos_probe(bios_disk *d, const part_entry *shelf);

/* The row's label, and the first line of its screen: which kind of shelf
 * the last pos_probe opened. */
const uint16_t *pos_entry_label(void);

#endif /* FALCON_BIOS_POS_H */
