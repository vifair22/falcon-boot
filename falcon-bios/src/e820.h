/* falcon-bios: the BIOS memory map. See e820.c. */

#ifndef FALCON_BIOS_E820_H
#define FALCON_BIOS_E820_H

#include <stdint.h>

/* The entry INT 15h AX=E820 writes, and the shape Linux's zero page wants it
 * in: the boot protocol's e820_table is an array of exactly this, so the map
 * is copied over rather than converted. */
typedef struct {
    uint64_t base;
    uint64_t len;
    uint32_t type;
    uint32_t attrs;
} e820_entry;

#define E820_USABLE   1u
#define E820_RESERVED 2u

/* What the zero page has room for (boot protocol E820_MAX_ENTRIES_ZEROPAGE).
 * A map longer than this is truncated, which is what every other loader does
 * and what the kernel expects when it sees the count at the limit. */
#define E820_MAX 128u

/* Reads the map once and caches it. Returns the number of entries, which is
 * 0 when the BIOS has no E820 at all. */
unsigned e820_read(void);

unsigned e820_count(void);
const e820_entry *e820_at(unsigned i);

/* The highest `align`-aligned address in [low, high) with `size` bytes of
 * usable memory above it, or 0 when there is nowhere. Used to place an
 * initrd, which is the one object whose size is not known in advance. */
uint64_t e820_place_high(uint64_t size, uint64_t align, uint64_t low,
                         uint64_t high);

#endif /* FALCON_BIOS_E820_H */
