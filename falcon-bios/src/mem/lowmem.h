/* falcon-bios: taking a block off the top of conventional memory for a
 * resident (DESIGN Section 13.6).
 *
 * The only region a chainloaded bootloader leaves alone below a megabyte is
 * one the BIOS says is not there, so a resident lives in kilobytes the word
 * at 0x413 stops counting. MEMDISK has done it since 2001. The virtual CD,
 * the virtual floppy and falcon jump's reverse thunk all take their blocks
 * this way; this is the one place the rule is written.
 *
 * Asked for, then taken separately at the point past which nothing can fail:
 * a refusal between the two that had already lowered the count would leave
 * the machine that much shorter for the rest of its uptime, once per
 * attempt. Residents stack: each asks below what the last one took.
 */

#ifndef FALCON_BIOS_LOWMEM_H
#define FALCON_BIOS_LOWMEM_H

#include <stdint.h>

/* The BIOS data area's count, in KB. */
#define BDA_MEMSIZE 0x413u

/* Nothing may be placed below the bounce buffer's end, which is stage 3's
 * until the handoff (biosdefs.h; stage2.S holds the EBDA above it). */
#define LOWMEM_FLOOR 0x98000u

/* The segment a block of `bytes` (a whole number of KB) would start at if
 * taken from a machine whose count is `have_kb`; 0 when it cannot be. The
 * arithmetic, host-tested. */
uint16_t lowmem_segment_for(uint16_t have_kb, uint32_t bytes);

/* The same against the BDA as it stands. Takes nothing. */
uint16_t lowmem_segment(uint32_t bytes);

/* Lowers the BDA's count to end at `seg`. */
void lowmem_take(uint16_t seg);

#endif /* FALCON_BIOS_LOWMEM_H */
