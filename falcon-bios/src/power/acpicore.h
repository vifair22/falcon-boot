/* falcon-bios: what ACPI says a power-off is (docs/DESIGN.md Section 7.9).
 *
 * Purpose. The idle power-off on a machine whose BIOS has no APM, or whose
 * APM does not take (power/power.c tries it first), is ACPI's S5: the
 * sleep type from the DSDT's \_S5 package written into the PM1 control
 * registers the FADT names, with SLP_EN set. This half finds those values
 * from the machine's memory through a read callback, so the host tests can
 * hand it tables they write; power.c reads physical memory for it and does
 * the port I/O.
 *
 * The walk: the RSDP in the EBDA's first kilobyte or 0xE0000-0xFFFFF, on a
 * 16-byte boundary with a valid checksum; the RSDT's (32-bit) entries for
 * the FADT ("FACP"); the FADT's DSDT, SMI command port, ACPI enable value
 * and PM1a/PM1b control blocks; the DSDT's AML searched for the name _S5_
 * and the package after it, whose first two integers are SLP_TYPa and
 * SLP_TYPb. That last step reads AML without an interpreter: it takes a
 * package of constants, which every firmware writes \_S5 as. A table
 * whose checksum fails is not believed. Nothing above 4 GiB is read,
 * since falcon-bios has paging off (the XSDT's 64-bit pointers are used
 * only when they fit).
 *
 * The table walk is also how mp/mp.c finds the MADT (acpi_find_table).
 *
 * Interactions. power/power.c and mp/mp.c are the callers. Freestanding, no
 * allocation: a small window is read at a time.
 */

#ifndef FALCON_BIOS_ACPICORE_H
#define FALCON_BIOS_ACPICORE_H

#include <stddef.h>
#include <stdint.h>

/* Reads len bytes at physical address addr; 0 on success. */
typedef int (*acpi_read_fn)(void *ctx, uint32_t addr, void *buf,
                            uint32_t len);

typedef struct {
    uint32_t smi_cmd;       /* 0: ACPI is always on (hardware-reduced) */
    uint8_t  acpi_enable;
    uint32_t pm1a_cnt;
    uint32_t pm1b_cnt;      /* 0: none */
    uint8_t  slp_typa;
    uint8_t  slp_typb;
} acpi_s5;

enum {
    ACPI_OK        = 0,
    ACPI_NO_RSDP   = -1,
    ACPI_NO_FADT   = -2,
    ACPI_NO_S5     = -3,   /* no \_S5 in the DSDT, or not one we can read */
    ACPI_BAD_TABLE = -4    /* a checksum or a length that does not hold */
};

/* The S5 values. ebda is the EBDA's physical address, 0 when the BDA
 * names none. */
int acpi_find_s5(acpi_read_fn rd, void *ctx, uint32_t ebda, acpi_s5 *out);

/* Any table the RSDT (or an XSDT whose pointers fit) lists, by its four
 * byte signature ("APIC" is the MADT, mp/mpcore.c's): its address and
 * length, once its checksum holds. ACPI_NO_FADT when no table carries the
 * signature, a name that predates this function. */
int acpi_find_table(acpi_read_fn rd, void *ctx, uint32_t ebda,
                    const char *sig, uint32_t *addr, uint32_t *len);

/* The \_S5 package in `aml` (a DSDT's body or part of one): SLP_TYPa and
 * SLP_TYPb. ACPI_NO_S5 when it is not there or not constants. */
int acpi_parse_s5(const uint8_t *aml, size_t len, uint8_t *typa,
                  uint8_t *typb);

/* The value written to PM1x_CNT for S5: SLP_TYP in bits 10-12, SLP_EN. */
uint16_t acpi_s5_word(uint8_t slp_typ);

#endif /* FALCON_BIOS_ACPICORE_H */
