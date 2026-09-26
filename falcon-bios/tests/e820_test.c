/* Tests for falcon-bios/src/mem/e820.c's placement arithmetic, which is what
 * chooses an initrd's address.
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
    };

    return cmocka_run_group_tests_name("bios e820", tests, NULL, NULL);
}
