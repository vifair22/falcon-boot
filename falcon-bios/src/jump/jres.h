/* falcon jump: installing the reverse thunk (rthunk.S; DESIGN Section 14.6).
 *
 * After jres_install, an INT 13h call for `drive` from real mode reaches
 * `entry` (a module's jmod_ops.int13) in protected mode on the stack at
 * `stack_top`, and every other call reaches the handler that was there
 * before. The block is taken from the top of conventional memory
 * (mem/lowmem.h); jres_remove gives it back and unhooks the vector, for a
 * route that fails after installing and before it hands over.
 */

#ifndef FALCON_BIOS_JRES_H
#define FALCON_BIOS_JRES_H

#include <stdint.h>

/* 0 on success, with the block's linear address in *base. -1 when the
 * machine will not give up the memory. */
int jres_install(uint8_t drive, uint32_t entry, uint32_t stack_top,
                 uint32_t *base);

/* Undoes the last jres_install, if the vector and the BDA's count are still
 * the ones it left. */
void jres_remove(void);

#endif /* FALCON_BIOS_JRES_H */
