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

/* The same, over a map the caller supplies rather than the cached one. The
 * loader only ever has one map; this exists so the arithmetic that picks an
 * initrd's address can be tested against maps a machine would be awkward to
 * arrange. */
uint64_t e820_place_in(const e820_entry *map, unsigned n, uint64_t size,
                       uint64_t align, uint64_t low, uint64_t high);

/* Whether every byte of [base, base + len) is usable memory: covered by
 * usable entries and overlapped by no other kind, since a real map can carry
 * a reserved entry inside a usable one. A fixed address this loader puts
 * something at (falcon jump's module region, a fixed-address kernel) is
 * checked with it. Nonzero when it is RAM; an empty range is not. */
int e820_is_ram_in(const e820_entry *map, unsigned n, uint64_t base,
                   uint64_t len);

/* The same over the cached map. */
int e820_is_ram(uint64_t base, uint64_t len);

/* A stretch of memory the loader leaves in use behind it when it hands the
 * machine over: the resident CD handler, and the Windows route's patched
 * tail (vcd.c). */
typedef struct {
    uint64_t base;
    uint64_t len;
} e820_range;

/* The most reserved ranges e820_carve takes. */
#define E820_CARVE_MAX 4u

/* The map a chainloaded loader should see: `in` with every range in `res`
 * taken out of the usable memory it overlaps and reported as reserved in
 * its place. Entries that are not usable pass through unchanged, and the
 * order of `in` is kept, so each usable entry becomes up to one piece more
 * per range than it was.
 *
 * Why: a chainloaded bootmgr or kernel allocates from the E820 map, and the
 * memory this loader leaves in use behind it has to be absent from that map
 * or it will be handed out. MEMDISK does the same.
 *
 * 0 with *n_out set, or -1 on a bad argument or when `out` has no room for
 * the result. */
int e820_carve(const e820_entry *in, unsigned n, const e820_range *res,
               unsigned n_res, e820_entry *out, unsigned cap,
               unsigned *n_out);

/* INT 15h AX=E801's answer for memory above 16 MB is one contiguous count of
 * 64 KB blocks, so it cannot describe a hole. This is the count that stops
 * short of the lowest range in `res` that reaches above 16 MB, or 0xFFFF
 * (no limit) when none does. */
uint16_t e820_e801_cap(const e820_range *res, unsigned n_res);

/* The other half of E801's answer, the KB between 1 and 16 MB, which is one
 * contiguous count too: the count that stops short of the lowest range in
 * `res` that reaches into that stretch (falcon jump's drivers, at 15 MB), or
 * 0xFFFF (no limit) when none does. */
uint16_t e820_e801_cap_low(const e820_range *res, unsigned n_res);

#endif /* FALCON_BIOS_E820_H */
