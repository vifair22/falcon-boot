/* falcon-bios: argon2id's lanes on the machine's other processors.
 *
 * Purpose. The BIOS half of the lane runner (UEFI's is falcon-efi/src/mp):
 * falcon-core's lane pool (crypto/lanes.h) with the other processors as
 * its workers. A BIOS has no service that starts a processor, so this
 * does what an operating system does: finds them in ACPI's MADT, and wakes
 * each with INIT and two startup IPIs into a page below a megabyte
 * (mp/aptramp.S), which takes it to protected mode under the boot
 * processor's GDT and into mp_ap_main on a stack of its own. Each borrows
 * its own AVX state, serves the pool, and gives the state back.
 *
 * When the keyslots are done each worker halts, and each is sent INIT,
 * which leaves it waiting for a startup IPI as POST left it: whatever boots
 * next, a kernel or a chained DOS, finds the processors as the BIOS handed
 * them over. The local APIC is put back as found too.
 *
 * Nothing here is needed for an unlock: no local APIC (a Pentium without
 * one, the A21m's Pentium III, which has one but is alone), no MADT, one
 * processor, a local APIC the BIOS left disabled or above 4 GiB, and
 * mp_start hands back NULL, which argon2id takes as lanes in turn.
 *
 * Interactions. falcon-bios/src/pos/pos.c starts it around luks_unlock,
 * after borrowing the boot processor's AVX state, whose level the workers
 * must match.
 */

#ifndef FALCON_BIOS_MP_H
#define FALCON_BIOS_MP_H

#include <stdint.h>

#include <crypto/argon2.h>

/* Workers at most, and each one's stack: a segment's job is about 5.5 KB
 * of it. */
#define MP_WORKERS_MAX  15u
#define MP_STACK_BYTES  16384u

/* Starts at most want workers for derivations at block function level
 * level. The runner to hand argon2id, or NULL when no other processor
 * runs lanes. */
const argon2_runner *mp_start(int level, uint32_t want);

/* Releases the workers, waits for them to halt, and sends each INIT.
 * Nothing when mp_start returned NULL. */
void mp_stop(void);

/* Processors that ran lanes: the boot processor and the workers that
 * joined. */
uint32_t mp_count(void);

/* The workers' C entry, from aptramp.S; ticket is which stack. */
void mp_ap_main(uint32_t ticket);

#endif /* FALCON_BIOS_MP_H */
