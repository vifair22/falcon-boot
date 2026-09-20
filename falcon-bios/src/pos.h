/* falcon-bios: booting the personal OS from p3. See pos.c. */

#ifndef FALCON_BIOS_POS_H
#define FALCON_BIOS_POS_H

#include <bios.h>
#include <part/part.h>

/* Unlocks p3, lists the UKIs on it, and boots the one chosen. Returns 0 only
 * when it did not boot: a boot does not return. */
int pos_run(bios_disk *d, const gpt_part *p3);

/* Whether p3 was found and opened as LUKS2 at all, so the menu knows whether
 * to offer a row. Asked after pos_probe. */
int pos_probe(bios_disk *d, const gpt_part *p3);

#endif /* FALCON_BIOS_POS_H */
