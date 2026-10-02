/* Tests for falcon-bios/src/mem/e820.c: the placement arithmetic, which is
 * what chooses an initrd's address, and the map built for a BIOS without
 * E820.
 *
 * The map a machine hands back is the one thing here nobody can arrange on
 * demand: a hole under 4 GB, a usable region that straddles the ceiling the
 * kernel named, a map with nothing usable at all. e820_place_in takes one
 * the test wrote, so each of those is a case rather than a wait for the
 * right hardware.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <mem/e820.h>
#include <string.h>

#define MIB (1024ull * 1024ull)

/* A map shaped like QEMU's: a megabyte of conventional memory, the hole at
 * 640 KB, then everything above. */
static const e820_entry g_simple[] = {
    { 0x00000000ull,  0x0009FC00ull, E820_USABLE,   0 },
    { 0x0009FC00ull,  0x00000400ull, E820_RESERVED, 0 },
    { 0x000F0000ull,  0x00010000ull, E820_RESERVED, 0 },
    { 0x00100000ull, 2047ull * MIB,  E820_USABLE,   0 }
};
#define SIMPLE_N (unsigned)(sizeof g_simple / sizeof g_simple[0])

static uint64_t place(uint64_t size, uint64_t align, uint64_t low,
                      uint64_t high)
{
    return e820_place_in(g_simple, SIMPLE_N, size, align, low, high);
}

/* As high as the window allows, because the low end of memory is where
 * everything else already is. */
static void test_place_is_high(void **state)
{
    uint64_t at;

    (void)state;
    at = place(16ull * MIB, 4096u, 32ull * MIB, 512ull * MIB);
    assert_int_equal(at, 512ull * MIB - 16ull * MIB);
    assert_int_equal(at % 4096u, 0);

    /* The window's top, not the region's: a ceiling the kernel named is
     * what bounds it. */
    at = place(1ull * MIB, 4096u, 32ull * MIB, 100ull * MIB);
    assert_int_equal(at, 99ull * MIB);
}

/* The result is aligned DOWN, so it never creeps above the ceiling. */
static void test_place_aligns_down(void **state)
{
    uint64_t at;

    (void)state;
    at = place(1000ull, 4096u, 32ull * MIB, 64ull * MIB + 1234ull);
    assert_true(at + 1000ull <= 64ull * MIB + 1234ull);
    assert_int_equal(at % 4096u, 0);

    at = place(1ull * MIB, 2ull * MIB, 32ull * MIB, 100ull * MIB);
    assert_int_equal(at % (2ull * MIB), 0);
    assert_true(at + MIB <= 100ull * MIB);
}

/* A window with nothing usable in it, a request bigger than the region, and
 * the arguments that make no sense. Each answers 0, which the caller reads
 * as "there is nowhere". */
static void test_place_refusals(void **state)
{
    (void)state;
    /* Entirely inside the reserved hole at 640 KB. */
    assert_int_equal(place(1024ull, 16u, 0x0009FC00ull, 0x000A0000ull), 0);
    /* Larger than everything usable. */
    assert_int_equal(place(4096ull * MIB, 4096u, 1ull * MIB, 2048ull * MIB), 0);
    /* A window that ends below where it starts. */
    assert_int_equal(place(4096ull, 4096u, 100ull * MIB, 50ull * MIB), 0);
    assert_int_equal(place(0, 4096u, 1ull * MIB, 512ull * MIB), 0);
    assert_int_equal(place(4096ull, 0, 1ull * MIB, 512ull * MIB), 0);
    assert_int_equal(e820_place_in(NULL, 4, 4096ull, 4096u, 0, 1ull * MIB), 0);
    assert_int_equal(e820_place_in(g_simple, 0, 4096ull, 4096u, 0,
                                   1ull * MIB), 0);
}

/* A machine whose usable memory is in several pieces: the highest piece that
 * fits wins, even when a lower one is larger. */
static void test_place_picks_the_highest_that_fits(void **state)
{
    static const e820_entry split[] = {
        { 0x00100000ull, 256ull * MIB, E820_USABLE,   0 },  /* big, low */
        { 0x20000000ull,   8ull * MIB, E820_RESERVED, 0 },
        { 0x20800000ull,  32ull * MIB, E820_USABLE,   0 }   /* small, high */
    };
    uint64_t at;

    (void)state;
    at = e820_place_in(split, 3, 4ull * MIB, 4096u, 1ull * MIB,
                       0x40000000ull);
    assert_true(at >= 0x20800000ull);
    assert_int_equal(at, 0x20800000ull + 32ull * MIB - 4ull * MIB);

    /* Too big for the high piece: it falls back to the low one rather than
     * failing. */
    at = e820_place_in(split, 3, 64ull * MIB, 4096u, 1ull * MIB,
                       0x40000000ull);
    assert_true(at >= 0x00100000ull && at < 0x20000000ull);
}

/* ---- carving what the loader leaves behind out of the map ---- */

#define KIB 1024ull

/* The chainload's shape: the resident at the top of conventional memory,
 * the Windows tail high in extended memory. Each usable entry they fall in
 * is cut around them, the reserved pieces are reported in its place, and
 * the entries that were never usable are untouched. */
static void test_carve_splits_usable(void **state)
{
    const e820_range res[] = {
        { 256ull * MIB, 8ull * MIB },            /* given out of order */
        { 0x9BC00ull,   16ull * KIB }
    };
    e820_entry out[16];
    unsigned n = 99;

    (void)state;
    memset(out, 0, sizeof out);
    assert_int_equal(e820_carve(g_simple, SIMPLE_N, res, 2, out, 16, &n), 0);
    /* The resident ends where the first entry does, so that one has no
     * usable tail: two pieces, the two reserved entries, three pieces. */
    assert_int_equal(n, 7);

    assert_int_equal(out[0].base, 0);
    assert_int_equal(out[0].len, 0x9BC00ull);
    assert_int_equal(out[0].type, E820_USABLE);
    assert_int_equal(out[1].base, 0x9BC00ull);
    assert_int_equal(out[1].len, 16ull * KIB);
    assert_int_equal(out[1].type, E820_RESERVED);
    assert_int_equal(out[2].base, 0x9FC00ull);
    assert_int_equal(out[2].type, E820_RESERVED);   /* was reserved already */
    assert_int_equal(out[3].base, 0xF0000ull);

    assert_int_equal(out[4].base, 1ull * MIB);
    assert_int_equal(out[4].len, 255ull * MIB);
    assert_int_equal(out[4].type, E820_USABLE);
    assert_int_equal(out[5].base, 256ull * MIB);
    assert_int_equal(out[5].len, 8ull * MIB);
    assert_int_equal(out[5].type, E820_RESERVED);
    assert_int_equal(out[6].base, 264ull * MIB);
    assert_int_equal(out[6].len, 2048ull * MIB - 264ull * MIB);
    assert_int_equal(out[6].type, E820_USABLE);
    assert_int_equal(out[7].type, 0);   /* nothing past the count is written */
}

/* A range that ends exactly where a usable entry does, one that starts at
 * its base, and one that is not in usable memory at all. No zero-length
 * pieces, and nothing reported over memory that was never usable. */
static void test_carve_edges(void **state)
{
    const e820_range res[] = {
        { 0x00000ull,  4ull * KIB },            /* at the base */
        { 0x9EC00ull,  4ull * KIB },            /* flush with the end */
        { 0xF0000ull,  4ull * KIB }             /* inside a reserved entry */
    };
    e820_entry out[16];
    unsigned n = 0;

    (void)state;
    memset(out, 0, sizeof out);
    assert_int_equal(e820_carve(g_simple, SIMPLE_N, res, 3, out, 16, &n), 0);
    assert_int_equal(n, 6);
    assert_int_equal(out[0].type, E820_RESERVED);
    assert_int_equal(out[0].base, 0);
    assert_int_equal(out[1].type, E820_USABLE);
    assert_int_equal(out[1].base, 4ull * KIB);
    assert_int_equal(out[1].len, 0x9EC00ull - 4ull * KIB);
    assert_int_equal(out[2].type, E820_RESERVED);
    assert_int_equal(out[2].base, 0x9EC00ull);
    assert_int_equal(out[3].base, 0x9FC00ull);
    assert_int_equal(out[4].base, 0xF0000ull);
    assert_int_equal(out[4].len, 0x10000ull);
    assert_int_equal(out[5].base, 1ull * MIB);
    assert_int_equal(out[5].len, 2047ull * MIB);
}

/* No room is a refusal, not a truncated map: a map with a range missing
 * would hand out exactly the memory this exists to protect. */
static void test_carve_refusals(void **state)
{
    const e820_range res[] = { { 0x9BC00ull, 16ull * KIB } };
    const e820_range many[E820_CARVE_MAX + 1u] = { { 0, 1 } };
    e820_entry out[16];
    unsigned n = 7;

    (void)state;
    assert_int_equal(e820_carve(g_simple, SIMPLE_N, res, 1, out, 4, &n), -1);
    assert_int_equal(n, 0);
    assert_int_equal(e820_carve(g_simple, SIMPLE_N, res, 1, out, 5, &n), 0);
    assert_int_equal(n, 5);
    assert_int_equal(e820_carve(g_simple, SIMPLE_N, many, E820_CARVE_MAX + 1u,
                                out, 16, &n), -1);
    assert_int_equal(e820_carve((const e820_entry *)0, 1, res, 1, out, 16,
                                &n), -1);
    assert_int_equal(e820_carve(g_simple, SIMPLE_N, res, 1,
                                (e820_entry *)0, 16, &n), -1);
    assert_int_equal(e820_carve(g_simple, SIMPLE_N, res, 1, out, 16,
                                (unsigned *)0), -1);

    /* Nothing to carve is the map as it was. */
    assert_int_equal(e820_carve(g_simple, SIMPLE_N, (const e820_range *)0, 0,
                                out, 16, &n), 0);
    assert_int_equal(n, SIMPLE_N);
}

/* E801 counts 64 KB blocks above 16 MB as one run, so it stops short of the
 * lowest range above 16 MB, and a range below it changes nothing. */
static void test_e801_cap(void **state)
{
    const e820_range tail[] = {
        { 0x9BC00ull, 16ull * KIB },
        { 512ull * MIB, 8ull * MIB },
        { 256ull * MIB, 8ull * MIB }
    };
    const e820_range low[] = { { 0x9BC00ull, 16ull * KIB } };
    const e820_range straddle[] = { { 15ull * MIB, 2ull * MIB } };
    const e820_range huge[] = { { 8192ull * MIB, MIB } };

    (void)state;
    assert_int_equal(e820_e801_cap(tail, 3), (256u - 16u) * 16u);
    assert_int_equal(e820_e801_cap(low, 1), 0xFFFFu);
    assert_int_equal(e820_e801_cap(straddle, 1), 0);
    assert_int_equal(e820_e801_cap(huge, 1), 0xFFFFu);
    assert_int_equal(e820_e801_cap((const e820_range *)0, 0), 0xFFFFu);
}

/* The module region below 16 MB, on the maps a machine hands over. */
static void test_is_ram(void **state)
{
    static const e820_entry plain[] = {
        { 0x0ull, 0x9FC00ull, E820_USABLE, 0 },
        { 0x9FC00ull, 0x400ull, E820_RESERVED, 0 },
        { 0x100000ull, 511ull * MIB, E820_USABLE, 0 },
    };
    /* Two usable entries that abut, listed out of order. */
    static const e820_entry split[] = {
        { 0x00F80000ull, 0x01000000ull, E820_USABLE, 0 },
        { 0x00100000ull, 0x00E80000ull, E820_USABLE, 0 },
    };
    /* A reserved hole inside a usable entry. */
    static const e820_entry holed[] = {
        { 0x00100000ull, 63ull * MIB, E820_USABLE, 0 },
        { 0x00F40000ull, 0x1000ull, E820_RESERVED, 0 },
    };
    /* A 15 MB machine. */
    static const e820_entry small[] = {
        { 0x00100000ull, 14ull * MIB, E820_USABLE, 0 },
    };

    (void)state;
    assert_true(e820_is_ram_in(plain, 3, 0x00F00000ull, 1ull * MIB));
    assert_false(e820_is_ram_in(plain, 3, 0x9F000ull, 0x2000ull));
    assert_true(e820_is_ram_in(split, 2, 0x00F00000ull, 1ull * MIB));
    assert_false(e820_is_ram_in(holed, 2, 0x00F00000ull, 1ull * MIB));
    assert_true(e820_is_ram_in(holed, 2, 0x00F00000ull, 0x40000ull));
    assert_false(e820_is_ram_in(small, 1, 0x00F00000ull, 1ull * MIB));
    assert_false(e820_is_ram_in(plain, 3, 0x00F00000ull, 0));
    assert_false(e820_is_ram_in(NULL, 3, 0x00F00000ull, 1ull * MIB));
    assert_false(e820_is_ram_in(plain, 3, ~0ull - 10u, 100u));
}

/* The 1-16 MB half: falcon jump's region at 15 MB leaves 14 MB. */
static void test_e801_cap_low(void **state)
{
    static const e820_range jump[] = {
        { 0x9B800ull, 0x4000ull },            /* conventional: not this half */
        { 0x00F00000ull, 0x00100000ull },
        { 0x40000000ull, 0x1000ull },         /* above 16 MB: the other half */
    };
    static const e820_range none[] = {
        { 0x9B800ull, 0x4000ull },
        { 0x01000000ull, 0x1000ull },
    };
    /* Straddling 1 MB: the whole half is gone. */
    static const e820_range low[] = {
        { 0x000F0000ull, 0x20000ull },
    };

    (void)state;
    assert_int_equal(e820_e801_cap_low(jump, 3), 14u * 1024u);
    assert_int_equal(e820_e801_cap_low(none, 2), 0xFFFFu);
    assert_int_equal(e820_e801_cap_low(low, 1), 0);
    assert_int_equal(e820_e801_cap_low(NULL, 0), 0xFFFFu);
}

/* The largest usable run inside a window: what a refused heap request is
 * compared with. Reserved entries are not counted, the window cuts an entry
 * that straddles it, and a window with nothing usable answers 0. */
static void test_largest(void **state)
{
    static const e820_entry split[] = {
        { 0x00000000ull,   0x0009FC00ull, E820_USABLE,   0 },
        { 0x00100000ull,   63ull * MIB,   E820_USABLE,   0 },
        { 64ull * MIB,     1ull * MIB,    E820_RESERVED, 0 },
        { 65ull * MIB,     200ull * MIB,  E820_USABLE,   0 },
        { 300ull * MIB,    100ull * MIB,  E820_USABLE,   0 },
        { 0xFEC00000ull,   0x01400000ull, E820_RESERVED, 0 }
    };
    const unsigned n = (unsigned)(sizeof split / sizeof split[0]);

    (void)state;
    assert_int_equal(e820_largest_in(split, n, 64ull * MIB,
                                     4096ull * MIB), 200ull * MIB);
    /* The window's floor cuts the 65 MB run below the 300 MB one. */
    assert_int_equal(e820_largest_in(split, n, 180ull * MIB,
                                     4096ull * MIB), 100ull * MIB);
    /* The ceiling cuts it too. */
    assert_int_equal(e820_largest_in(split, n, 64ull * MIB, 100ull * MIB),
                     35ull * MIB);
    assert_int_equal(e820_largest_in(split, n, 64ull * MIB, 65ull * MIB),
                     0);
    assert_int_equal(e820_largest_in(NULL, n, 0, 4096ull * MIB), 0);
    assert_int_equal(e820_largest_in(g_simple, SIMPLE_N, 64ull * MIB,
                                     4096ull * MIB), 2048ull * MIB - 64ull * MIB);
}

/* ---- the map a BIOS without E820 describes with the older calls ---- */

/* What a 128 MB machine with E801 answers: 15 MB between 1 and 16 MB, 1792
 * blocks above. Conventional memory from the BDA's count, the EBDA and the
 * video area reserved, then the two extended runs, every attrs 1. */
static void test_synth_e801(void **state)
{
    e820_legacy in = { 639u, 1, 0x3C00u, 1792u, 0x3C00u, 1792u, 1, 0xFC00u };
    e820_entry out[8];
    e820_from from = E820_FROM_NONE;
    unsigned n;
    unsigned i;

    (void)state;
    memset(out, 0, sizeof out);
    n = e820_synth(&in, out, 8, &from);
    assert_int_equal(from, E820_FROM_E801);
    assert_int_equal(n, 5);
    assert_int_equal(out[0].base, 0);
    assert_int_equal(out[0].len, 639ull * KIB);
    assert_int_equal(out[0].type, E820_USABLE);
    assert_int_equal(out[1].base, 639ull * KIB);
    assert_int_equal(out[1].len, 0xA0000ull - 639ull * KIB);
    assert_int_equal(out[1].type, E820_RESERVED);
    assert_int_equal(out[2].base, 0xA0000ull);
    assert_int_equal(out[2].len, 0x60000ull);
    assert_int_equal(out[2].type, E820_RESERVED);
    assert_int_equal(out[3].base, 1ull * MIB);
    assert_int_equal(out[3].len, 15ull * MIB);
    assert_int_equal(out[3].type, E820_USABLE);
    assert_int_equal(out[4].base, 16ull * MIB);
    assert_int_equal(out[4].len, 112ull * MIB);
    assert_int_equal(out[4].type, E820_USABLE);
    for (i = 0; i < n; i++)
        assert_int_equal(out[i].attrs, 1);
    assert_int_equal(out[5].type, 0);

    /* What the consumers ask of it: the module region is RAM, an initrd
     * goes at the top. */
    assert_true(e820_is_ram_in(out, n, 0x00F00000ull, 1ull * MIB));
    assert_int_equal(e820_place_in(out, n, 4ull * MIB, 4096u, 16ull * MIB,
                                   128ull * MIB), 124ull * MIB);
}

/* A 15 MB machine whose E801 count stops at 14 MB: the hole up to 16 MB is
 * reserved and there is nothing above it, so the module region is not RAM.
 * And the AX=0 form, where the counts are in CX and DX. */
static void test_synth_e801_hole(void **state)
{
    e820_legacy in = { 639u, 1, 0x3800u, 0, 0x3800u, 0, 0, 0 };
    e820_legacy cxdx = { 639u, 1, 0, 0, 0x3C00u, 16u, 0, 0 };
    e820_entry out[8];
    e820_from from;
    unsigned n;

    (void)state;
    n = e820_synth(&in, out, 8, &from);
    assert_int_equal(from, E820_FROM_E801);
    assert_int_equal(n, 5);
    assert_int_equal(out[3].base, 1ull * MIB);
    assert_int_equal(out[3].len, 14ull * MIB);
    assert_int_equal(out[3].type, E820_USABLE);
    assert_int_equal(out[4].base, 15ull * MIB);
    assert_int_equal(out[4].len, 1ull * MIB);
    assert_int_equal(out[4].type, E820_RESERVED);
    assert_false(e820_is_ram_in(out, n, 0x00F00000ull, 1ull * MIB));

    n = e820_synth(&cxdx, out, 8, &from);
    assert_int_equal(from, E820_FROM_E801);
    assert_int_equal(n, 5);
    assert_int_equal(out[3].len, 15ull * MIB);
    assert_int_equal(out[4].base, 16ull * MIB);
    assert_int_equal(out[4].len, 1ull * MIB);
    assert_int_equal(out[4].type, E820_USABLE);
}

/* AH=88h: one run from 1 MB, when E801 is absent and when it answered with
 * AX left as it was called (no E801 either, whatever the carry said). */
static void test_synth_88(void **state)
{
    e820_legacy in = { 639u, 0, 0, 0, 0, 0, 1, 0xFC00u };
    e820_legacy bogus = { 639u, 1, 0xE801u, 0, 0, 0, 1, 0x3C00u };
    e820_entry out[8];
    e820_from from;
    unsigned n;

    (void)state;
    n = e820_synth(&in, out, 8, &from);
    assert_int_equal(from, E820_FROM_88);
    assert_int_equal(n, 4);
    assert_int_equal(out[3].base, 1ull * MIB);
    assert_int_equal(out[3].len, 0xFC00ull * KIB);
    assert_int_equal(out[3].type, E820_USABLE);
    assert_int_equal(out[3].attrs, 1);

    n = e820_synth(&bogus, out, 8, &from);
    assert_int_equal(from, E820_FROM_88);
    assert_int_equal(n, 4);
    assert_int_equal(out[3].len, 15ull * MIB);
    assert_true(e820_is_ram_in(out, n, 0x00F00000ull, 1ull * MIB));
}

/* Neither call: conventional memory alone. A BDA count of 0 has no usable
 * entry, one past 640 KB is held to the video area, and a 640 KB count has
 * no EBDA entry. */
static void test_synth_bda(void **state)
{
    e820_legacy in = { 639u, 0, 0, 0, 0, 0, 0, 0 };
    e820_entry out[8];
    e820_from from;
    unsigned n;

    (void)state;
    n = e820_synth(&in, out, 8, &from);
    assert_int_equal(from, E820_FROM_BDA);
    assert_int_equal(n, 3);
    assert_int_equal(out[2].base, 0xA0000ull);
    assert_int_equal(out[2].len, 0x60000ull);
    assert_int_equal(e820_largest_in(out, n, 1ull * MIB, 4096ull * MIB), 0);

    in.base_kb = 0;
    n = e820_synth(&in, out, 8, &from);
    assert_int_equal(n, 2);
    assert_int_equal(out[0].base, 0);
    assert_int_equal(out[0].len, 0xA0000ull);
    assert_int_equal(out[0].type, E820_RESERVED);

    in.base_kb = 1024u;
    n = e820_synth(&in, out, 8, &from);
    assert_int_equal(n, 2);
    assert_int_equal(out[0].len, 0xA0000ull);
    assert_int_equal(out[0].type, E820_USABLE);
    assert_int_equal(out[1].base, 0xA0000ull);

    in.base_kb = 640u;
    n = e820_synth(&in, out, 8, &from);
    assert_int_equal(n, 2);
    assert_int_equal(out[0].len, 0xA0000ull);
    assert_int_equal(out[0].type, E820_USABLE);
}

/* No room is a truncated map, the count written; a bad argument is 0. */
static void test_synth_refusals(void **state)
{
    e820_legacy in = { 639u, 1, 0x3C00u, 1792u, 0, 0, 1, 0xFC00u };
    e820_entry out[8];
    e820_from from = E820_FROM_E820;

    (void)state;
    assert_int_equal(e820_synth(&in, out, 2, &from), 2);
    assert_int_equal(e820_synth(&in, out, 4, &from), 4);
    assert_int_equal(from, E820_FROM_E801);
    assert_int_equal(e820_synth(&in, out, 0, &from), 0);
    assert_int_equal(e820_synth(NULL, out, 8, &from), 0);
    assert_int_equal(from, E820_FROM_NONE);
    assert_int_equal(e820_synth(&in, NULL, 8, &from), 0);
    assert_int_equal(e820_synth(&in, out, 8, NULL), 5);
}

int run_e820_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_place_is_high),
        cmocka_unit_test(test_place_aligns_down),
        cmocka_unit_test(test_place_refusals),
        cmocka_unit_test(test_place_picks_the_highest_that_fits),
        cmocka_unit_test(test_carve_splits_usable),
        cmocka_unit_test(test_carve_edges),
        cmocka_unit_test(test_carve_refusals),
        cmocka_unit_test(test_e801_cap),
        cmocka_unit_test(test_is_ram),
        cmocka_unit_test(test_e801_cap_low),
        cmocka_unit_test(test_largest),
        cmocka_unit_test(test_synth_e801),
        cmocka_unit_test(test_synth_e801_hole),
        cmocka_unit_test(test_synth_88),
        cmocka_unit_test(test_synth_bda),
        cmocka_unit_test(test_synth_refusals),
    };

    return cmocka_run_group_tests_name("bios e820", tests, NULL, NULL);
}
