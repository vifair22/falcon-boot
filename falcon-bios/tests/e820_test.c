/* Tests for falcon-bios/src/e820.c's placement arithmetic, which is what
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

#include <e820.h>

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

int run_e820_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_place_is_high),
        cmocka_unit_test(test_place_aligns_down),
        cmocka_unit_test(test_place_refusals),
        cmocka_unit_test(test_place_picks_the_highest_that_fits),
    };

    return cmocka_run_group_tests_name("bios e820", tests, NULL, NULL);
}
