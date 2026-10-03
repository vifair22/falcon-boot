/* Tests for falcon-efi/src/tsc/tsccore.c, the time-stamp counter's
 * arithmetic behind the boot-timing variables (docs/DESIGN.md Section 5.5).
 *
 * The expected values were worked out apart from the module, in 128-bit
 * arithmetic, so a conversion that agrees only with itself fails here. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <tsc/tsccore.h>

/* The T430's i5-3320M, as its kernel calibrates it. */
#define HZ_T430 2594102000ull

/* ---- ticks to microseconds ---- */

static void test_usec_one_second(void **state)
{
    (void)state;
    assert_int_equal(tsc_usec(HZ_T430, HZ_T430), 1000000u);
    assert_int_equal(tsc_usec(0, HZ_T430), 0u);
    /* floor, as systemd-boot's integer division has it */
    assert_int_equal(tsc_usec(HZ_T430 - 1u, HZ_T430), 999999u);
}

static void test_usec_matches_systemd_where_it_does_not_overflow(void **state)
{
    /* 1000 * 1000 * ticks / freq, ticks small enough not to wrap. */
    uint64_t ticks = 12345678901ull;

    (void)state;
    assert_int_equal(tsc_usec(ticks, HZ_T430), 1000000u * ticks / HZ_T430);
    assert_int_equal(tsc_usec(ticks, HZ_T430), 4759133u);
}

static void test_usec_near_2_64(void **state)
{
    (void)state;
    /* A counter at the top of its range, at the slowest believed rate and
     * at others; the naive product would wrap long before. */
    assert_int_equal(tsc_usec(UINT64_MAX, TSC_HZ_MIN),
                     184467440737095516ull);
    assert_int_equal(tsc_usec(UINT64_MAX, TSC_HZ_MIN + 1u),
                     184467438892421127ull);
    assert_int_equal(tsc_usec(UINT64_MAX, HZ_T430), 7111032670924100ull);
    assert_int_equal(tsc_usec(UINT64_MAX, TSC_HZ_MAX), 922337203685477ull);
}

static void test_usec_insane_rate_is_zero(void **state)
{
    (void)state;
    assert_int_equal(tsc_usec(HZ_T430, 0), 0u);
    assert_int_equal(tsc_usec(HZ_T430, TSC_HZ_MIN - 1u), 0u);
    assert_int_equal(tsc_usec(HZ_T430, TSC_HZ_MAX + 1u), 0u);
}

/* ---- CPUID ---- */

static tsc_cpuid intel(uint32_t max_leaf, uint32_t den, uint32_t num,
                       uint32_t crystal, uint32_t base_mhz)
{
    tsc_cpuid c;

    c.max_leaf = max_leaf;
    c.intel = 1;
    c.den = den;
    c.num = num;
    c.crystal = crystal;
    c.base_mhz = base_mhz;
    return c;
}

static void test_cpuid_crystal(void **state)
{
    /* Skylake-shaped: 24 MHz crystal, ratio 216/2. */
    tsc_cpuid c = intel(0x16, 2, 216, 24000000u, 2600);

    (void)state;
    assert_int_equal(tsc_hz_cpuid(&c), 2592000000ull);
}

static void test_cpuid_base_frequency(void **state)
{
    /* Crystal not enumerated: the crystal comes from leaf 0x16 through the
     * ratio, and back, rounding as systemd-boot rounds. */
    tsc_cpuid c = intel(0x16, 2, 234, 0, 2800);

    (void)state;
    assert_int_equal(tsc_hz_cpuid(&c), 2799999891ull);
    /* Only EAX[15:0] is the frequency. */
    c.base_mhz = 0xABCD0000u | 2800u;
    assert_int_equal(tsc_hz_cpuid(&c), 2799999891ull);
}

static void test_cpuid_no_answer(void **state)
{
    tsc_cpuid c;

    (void)state;
    assert_int_equal(tsc_hz_cpuid(NULL), 0u);
    /* Ivy Bridge: leaf 0xD is the top, no 0x15. */
    c = intel(0x0D, 0, 0, 0, 0);
    assert_int_equal(tsc_hz_cpuid(&c), 0u);
    /* Leaf 0x15 present, ratio not enumerated. */
    c = intel(0x16, 0, 216, 24000000u, 2600);
    assert_int_equal(tsc_hz_cpuid(&c), 0u);
    c = intel(0x16, 2, 0, 24000000u, 2600);
    assert_int_equal(tsc_hz_cpuid(&c), 0u);
    /* No crystal and no leaf 0x16. */
    c = intel(0x15, 2, 216, 0, 2600);
    assert_int_equal(tsc_hz_cpuid(&c), 0u);
    /* No crystal and a zero base frequency. */
    c = intel(0x16, 2, 216, 0, 0);
    assert_int_equal(tsc_hz_cpuid(&c), 0u);
    /* Another vendor's leaf 0x15 is not Intel's. */
    c = intel(0x16, 2, 216, 24000000u, 2600);
    c.intel = 0;
    assert_int_equal(tsc_hz_cpuid(&c), 0u);
}

static void test_cpuid_insane_or_overflowing(void **state)
{
    tsc_cpuid c;

    (void)state;
    /* A 1 Hz crystal. */
    c = intel(0x15, 1, 1, 1, 0);
    assert_int_equal(tsc_hz_cpuid(&c), 0u);
    /* Products past 2^64 are refused, not wrapped into range. */
    c = intel(0x15, 1, 0xFFFFFFFFu, 0xFFFFFFFFu, 0);
    assert_int_equal(tsc_hz_cpuid(&c), 0u);
    c = intel(0x16, 0xFFFFFFFFu, 1, 0, 0xFFFF);
    assert_int_equal(tsc_hz_cpuid(&c), 0u);
}

/* ---- the calibration window ---- */

static void test_window(void **state)
{
    (void)state;
    /* 10 ms at the T430's rate. */
    assert_int_equal(tsc_hz_window(1000u, 1000u + 25941020u, 10000u),
                     HZ_T430);
    assert_int_equal(tsc_hz_window(1000u, 1000u, 10000u), 0u);
    assert_int_equal(tsc_hz_window(2000u, 1000u, 10000u), 0u);
    assert_int_equal(tsc_hz_window(1000u, 1000u + 25941020u, 0), 0u);
    /* A Stall that returned at once. */
    assert_int_equal(tsc_hz_window(1000u, 1100u, 10000u), 0u);
    /* A span that overflows the product is refused. */
    assert_int_equal(tsc_hz_window(0, 1ull << 63, 10000u), 0u);
}

/* ---- the wait ---- */

static void test_span(void **state)
{
    tsc_span s;

    (void)state;
    memset(&s, 0, sizeof s);
    tsc_span_open(&s, 100u);
    tsc_span_close(&s, 250u);
    assert_int_equal(s.total, 150u);
    tsc_span_open(&s, 1000u);
    tsc_span_close(&s, 1010u);
    assert_int_equal(s.total, 160u);
}

static void test_span_nested_counts_outermost(void **state)
{
    tsc_span s;

    (void)state;
    memset(&s, 0, sizeof s);
    tsc_span_open(&s, 100u);
    tsc_span_open(&s, 150u);
    tsc_span_close(&s, 160u);
    assert_int_equal(s.total, 0u);
    tsc_span_close(&s, 300u);
    assert_int_equal(s.total, 200u);
    assert_int_equal(s.depth, 0u);
}

static void test_span_unbalanced_and_backwards(void **state)
{
    tsc_span s;

    (void)state;
    memset(&s, 0, sizeof s);
    tsc_span_close(&s, 100u);
    assert_int_equal(s.total, 0u);
    assert_int_equal(s.depth, 0u);
    tsc_span_open(&s, 500u);
    tsc_span_close(&s, 400u);
    assert_int_equal(s.total, 0u);
    assert_int_equal(s.depth, 0u);
}

/* ---- decimal ---- */

static void assert_dec(uint64_t v, unsigned minw, const char *want)
{
    uint16_t out[24];
    char got[24];
    size_t n;

    memset(out, 0xEE, sizeof out);
    n = tsc_dec16(v, minw, out, 24u);
    assert_int_equal(n, strlen(want));
    assert_int_equal(out[n], 0);
    ucs2_to_ascii(out, got, sizeof got);
    assert_string_equal(got, want);
}

static void test_dec(void **state)
{
    (void)state;
    assert_dec(0, 0, "0");
    assert_dec(7, 0, "7");
    assert_dec(1234567, 0, "1234567");
    assert_dec(UINT64_MAX, 0, "18446744073709551615");
    assert_dec(7, 2, "07");
    assert_dec(70, 2, "70");
    assert_dec(4660, 2, "4660");
    assert_dec(0, 2, "00");
    assert_dec(5, 99, "00000000000000000005");
}

static void test_dec_too_small(void **state)
{
    uint16_t out[4] = { 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE };

    (void)state;
    /* Four digits and a terminator do not fit in four units. */
    assert_int_equal(tsc_dec16(1234u, 0, out, 4u), 0u);
    assert_int_equal(out[0], 0xEEEE);
    assert_int_equal(tsc_dec16(123u, 0, out, 4u), 3u);
    assert_int_equal(tsc_dec16(1u, 0, NULL, 4u), 0u);
    /* All 20 digits and the terminator fit in 21. */
    {
        uint16_t big[21];

        assert_int_equal(tsc_dec16(UINT64_MAX, 0, big, 21u), 20u);
        assert_int_equal(tsc_dec16(UINT64_MAX, 0, big, 20u), 0u);
    }
}

int run_tsc_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_usec_one_second),
        cmocka_unit_test(test_usec_matches_systemd_where_it_does_not_overflow),
        cmocka_unit_test(test_usec_near_2_64),
        cmocka_unit_test(test_usec_insane_rate_is_zero),
        cmocka_unit_test(test_cpuid_crystal),
        cmocka_unit_test(test_cpuid_base_frequency),
        cmocka_unit_test(test_cpuid_no_answer),
        cmocka_unit_test(test_cpuid_insane_or_overflowing),
        cmocka_unit_test(test_window),
        cmocka_unit_test(test_span),
        cmocka_unit_test(test_span_nested_counts_outermost),
        cmocka_unit_test(test_span_unbalanced_and_backwards),
        cmocka_unit_test(test_dec),
        cmocka_unit_test(test_dec_too_small),
    };

    return cmocka_run_group_tests_name("tsc", tests, NULL, NULL);
}
