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

/* Reads the map once and caches it: INT 15h AX=E820, or on a BIOS without
 * it the map e820_synth builds from the older calls. Returns the number of
 * entries. */
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

/* The length of the largest usable entry's part inside [low, high), or 0.
 * What heap_open could be given, said when a request is refused: an
 * argon2id keyslot's memory against what this machine has (pos.c). */
uint64_t e820_largest_in(const e820_entry *map, unsigned n, uint64_t low,
                         uint64_t high);

/* The same over the cached map. */
uint64_t e820_largest(uint64_t low, uint64_t high);

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

/* What the older calls answer on a BIOS without E820, as the registers come
 * back: the BDA's count of conventional memory in KB (0x413); INT 15h
 * AX=E801, when it returned with carry clear, with AX and CX the KB between
 * 1 and 16 MB and BX and DX the 64 KB blocks above 16 MB; and INT 15h
 * AH=88h, when it returned with carry clear, with AX the KB above 1 MB. */
typedef struct {
    uint16_t base_kb;
    int      e801;
    uint16_t e801_ax;
    uint16_t e801_bx;
    uint16_t e801_cx;
    uint16_t e801_dx;
    int      ah88;
    uint16_t ah88_ax;
} e820_legacy;

/* Which call a map came from. */
typedef enum {
    E820_FROM_NONE = 0,
    E820_FROM_E820,
    E820_FROM_E801,
    E820_FROM_88,
    E820_FROM_BDA       /* neither older call answered: conventional memory only */
} e820_from;

/* The map those answers describe, into `out` (at most `cap` entries, the
 * count written returned; six is enough for every answer): conventional
 * memory from the BDA's count, the EBDA above it and the video and ROM
 * area to 1 MB reserved, then extended memory from E801 (the 1-16 MB count,
 * the hole it stops short of reserved, the count above 16 MB) or, when that
 * call is absent or its count is not one, from AH=88h. Every entry's attrs
 * is 1, as an E820 entry's is taken to be. *from says which. */
unsigned e820_synth(const e820_legacy *in, e820_entry *out, unsigned cap,
                    e820_from *from);

#endif /* FALCON_BIOS_E820_H */
