/* falcon-bios: the drive swap's resident: a disk's number and 0x80 trade places
 * for everything that calls INT 13h after it (swapres.S).
 */

#ifndef FALCON_BIOS_SWAP_H
#define FALCON_BIOS_SWAP_H

#include <stdint.h>

/* Copies the resident to seg, which the caller got from lowmem_segment, and
 * hooks INT 13h with it. Cannot fail. */
void swap_install(uint16_t seg, uint8_t drive);

#endif /* FALCON_BIOS_SWAP_H */
