/* falcon-bios: which processors take argon2id lanes, the half of the BIOS
 * lane runner (mp/mp.c) with no hardware in it, host-tested.
 *
 * A BIOS hands a boot loader one processor and leaves the others waiting
 * for a startup IPI. The machine's others are listed in ACPI's MADT
 * ("APIC"), one entry a logical processor: a local APIC entry (type 0, an
 * 8-bit APIC ID) or an x2APIC one (type 9, 32 bits), each with an enabled
 * flag. The local APIC's address is in the table's head, or in an
 * override entry (type 5) where the firmware put it above.
 *
 * A worker is the first enabled thread of a core the boot processor is not
 * on, as on UEFI (falcon-efi/src/mp/mpcore.h): two lanes on the threads of
 * one core run little faster than one. The core is the APIC ID without
 * its thread bits, how many CPUID leaf 0Bh says; a processor that does not
 * say has every APIC ID its own core.
 *
 * Interactions. mp.c reads the table through power/acpicore.c and hands
 * the bytes here.
 */

#ifndef FALCON_BIOS_MPCORE_H
#define FALCON_BIOS_MPCORE_H

#include <stddef.h>
#include <stdint.h>

#define MPB_CPUS_MAX 256u

typedef struct {
    uint32_t apic_id;
    uint8_t  enabled;
} mpb_cpu;

/* The processors a MADT of len bytes lists, at most cap of them in table
 * order, each APIC ID once, into cpus; *lapic the local APIC's address
 * (0 when it is above 4 GiB, which falcon-bios cannot reach). 0, or -1
 * for a table that is not a MADT or does not hold together. */
int mpb_parse_madt(const uint8_t *madt, size_t len, uint32_t *lapic,
                   mpb_cpu *cpus, size_t cap, size_t *n);

/* The thread bits of an APIC ID from CPUID leaf 0Bh, sub-leaf 0: its EAX
 * and ECX, given that the processor's highest basic leaf (max) reaches
 * 0Bh. 0 when it does not, or the level is not the SMT one. */
uint32_t mpb_smt_shift(uint32_t max, uint32_t eax_b0, uint32_t ecx_b0);

/* At most want workers out of cpus[0..n), the boot processor's APIC ID
 * being bsp: their APIC IDs into ids, in table order. How many. */
uint32_t mpb_pick(const mpb_cpu *cpus, size_t n, uint32_t bsp,
                  uint32_t smt_shift, uint32_t want, uint32_t *ids);

#endif /* FALCON_BIOS_MPCORE_H */
