/* falcon jump: installing the reverse thunk (rthunk.S; DESIGN Section 14.6).
 *
 * After jres_install, an INT 13h call for `drive` from real mode reaches
 * `entry` (a module's jmod_ops.int13) in protected mode on the stack at
 * `stack_top`, and every other call reaches the handler that was there
 * before. The block is taken from the top of conventional memory
 * (mem/lowmem.h); jres_remove gives it back and unhooks the vectors, for a
 * route that fails after installing and before it hands over.
 */

#ifndef FALCON_BIOS_JRES_H
#define FALCON_BIOS_JRES_H

#include <stdint.h>

#include <mem/e820.h>

/* 0 on success, with the block's linear address in *base. -1 when the
 * machine will not give up the memory, or the carved map will not fit.
 *
 * With `keep` (n_keep ranges), INT 15h is hooked too and answers the memory
 * map with the block and those ranges carved out (E820, both halves of
 * E801, AH=88h): the USB route's, which has no virtual CD to do it. With
 * none, INT 15h is left alone. */
int jres_install(uint8_t drive, uint32_t entry, uint32_t stack_top,
                 const e820_range *keep, unsigned n_keep, uint32_t *base);

/* Undoes the last jres_install, if the vector and the BDA's count are still
 * the ones it left. */
void jres_remove(void);

#endif /* FALCON_BIOS_JRES_H */
