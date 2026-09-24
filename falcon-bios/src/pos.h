/* falcon-bios: booting the personal OS from p3. See pos.c. */

#ifndef FALCON_BIOS_POS_H
#define FALCON_BIOS_POS_H

#include <bios.h>
#include <result.h>
#include <part/part.h>

/* The user backed out, at the prompt or at the chooser. Nothing is on screen
 * worth a keypress. */
#define POS_ABORTED 1

/* Unlocks p3, offers the UKIs on it, and boots the one chosen, drawing in
 * the page the caller started. Does not return when the boot happens.
 * POS_ABORTED when the user backed out; -1 with *r filled in when it failed.
 * Every secret is wiped either way. */
int pos_run(bios_disk *d, const gpt_part *p3, boot_result *r);

/* Whether p3 was found and opened as LUKS2 at all, so the menu knows whether
 * to offer the row. Reads the header only; asks for nothing. */
int pos_probe(bios_disk *d, const gpt_part *p3);

/* The row's label, and the first line of its screen. */
const uint16_t *pos_entry_label(void);

#endif /* FALCON_BIOS_POS_H */
