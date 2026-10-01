/* falcon jump: installing the reverse thunk (rthunk.S; DESIGN Section 13.6).
 *
 * After jres_install, an INT 13h call for `drive` from real mode reaches
 * `entry` (a module's jmod_ops.int13) in protected mode on the stack at
 * `stack_top`, and every other call reaches the handler that was there
 * before; with keyboards named (jres_keyboards), every INT 16h call polls
 * them first. The block is taken from the top of conventional memory
 * (mem/lowmem.h); jres_remove gives it back and unhooks the vectors, for a
 * route that fails after installing and before it hands over.
 */

#ifndef FALCON_BIOS_JRES_H
#define FALCON_BIOS_JRES_H

#include <stdint.h>

#include <mem/e820.h>

/* The keyboard entries (modules' jmod_ops.int16, up to JRES_KBD_MAX) the
 * next jres_install hooks INT 16h for, on the stack it is given; none, the
 * default and what that install leaves set, hooks no INT 16h. */
void jres_keyboards(const uint32_t *entries, unsigned n);

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

/* The drive the resident on INT 13h answers, or 0 when INT 13h does not
 * lead to one: a block this build or any other falcon jump installed, found
 * by its magic. */
uint8_t jres_drive(void);

/* Releases the resident INT 13h leads to, whoever installed it: every hook
 * and BDA count its release record holds goes back to what the BIOS left,
 * where nothing has changed it since. For a system started on another disk,
 * which reads no disk through the BIOS once it runs and must not inherit
 * vectors into memory it is about to own: Windows 2000 froze on the A21m
 * started on top of one. The drive it answered, or 0 when there was none. */
uint8_t jres_release(void);

#endif /* FALCON_BIOS_JRES_H */
