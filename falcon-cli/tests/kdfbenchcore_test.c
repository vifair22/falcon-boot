/* Tests for falcon-cli/src/mkstick/kdfbenchcore.c: what `mkstick kdf-bench`
 * takes on its command line and the table it prints. The timing itself is
 * kdfbench.c's and is run by hand. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <crypto/argon2.h>
#include <crypto/crypto.h>
#include <mkstick/kdfbenchcore.h>

static void test_list(void **state)
{
    uint32_t v[KDFB_MAX_COLS];
    size_t n = 99;

    (void)state;
    assert_int_equal(kdfb_parse_list("64,128,1024", 1, 4096, v, 16, &n),
                     KDFB_OK);
    assert_int_equal(n, 3);
    assert_int_equal(v[0], 64);
    assert_int_equal(v[1], 128);
    assert_int_equal(v[2], 1024);

    assert_int_equal(kdfb_parse_list("4", 1, 64, v, 16, &n), KDFB_OK);
    assert_int_equal(n, 1);
    assert_int_equal(v[0], 4);

    /* The order given is the order printed: no sort, no dedup. */
    assert_int_equal(kdfb_parse_list("8,1,8", 1, 64, v, 16, &n), KDFB_OK);
    assert_int_equal(n, 3);
    assert_int_equal(v[0], 8);
    assert_int_equal(v[1], 1);
    assert_int_equal(v[2], 8);
}

static void test_list_refusals(void **state)
{
    uint32_t v[4];
    size_t n;

    (void)state;
    assert_int_equal(kdfb_parse_list(NULL, 1, 9, v, 4, &n), KDFB_ERR_EMPTY);
    assert_int_equal(kdfb_parse_list("", 1, 9, v, 4, &n), KDFB_ERR_EMPTY);
    assert_int_equal(kdfb_parse_list("1,,2", 1, 9, v, 4, &n),
                     KDFB_ERR_EMPTY);
    assert_int_equal(kdfb_parse_list("1,", 1, 9, v, 4, &n), KDFB_ERR_EMPTY);
    assert_int_equal(kdfb_parse_list(",1", 1, 9, v, 4, &n), KDFB_ERR_EMPTY);
    assert_int_equal(kdfb_parse_list("1x", 1, 9, v, 4, &n), KDFB_ERR_NUM);
    assert_int_equal(kdfb_parse_list("x", 1, 9, v, 4, &n), KDFB_ERR_NUM);
    assert_int_equal(kdfb_parse_list("-1", 1, 9, v, 4, &n), KDFB_ERR_NUM);
    assert_int_equal(kdfb_parse_list("1 ,2", 1, 9, v, 4, &n), KDFB_ERR_NUM);
    assert_int_equal(kdfb_parse_list("0", 1, 9, v, 4, &n), KDFB_ERR_RANGE);
    assert_int_equal(kdfb_parse_list("10", 1, 9, v, 4, &n), KDFB_ERR_RANGE);
    assert_int_equal(kdfb_parse_list("99999999999", 1, 9, v, 4, &n),
                     KDFB_ERR_RANGE);
    assert_int_equal(kdfb_parse_list("1,2,3,4,5", 1, 9, v, 4, &n),
                     KDFB_ERR_MANY);
}

static void test_level(void **state)
{
    int l = 99;

    (void)state;
    assert_int_equal(kdfb_parse_level("auto", &l), KDFB_OK);
    assert_int_equal(l, ARGON2_SIMD_AUTO);
    assert_int_equal(kdfb_parse_level("portable", &l), KDFB_OK);
    assert_int_equal(l, CRYPTO_SIMD_NONE);
    assert_int_equal(kdfb_parse_level("sse2", &l), KDFB_OK);
    assert_int_equal(l, CRYPTO_SIMD_SSE2);
    assert_int_equal(kdfb_parse_level("ssse3", &l), KDFB_OK);
    assert_int_equal(l, CRYPTO_SIMD_SSSE3);
    assert_int_equal(kdfb_parse_level("avx2", &l), KDFB_OK);
    assert_int_equal(l, CRYPTO_SIMD_AVX2);
    assert_int_equal(kdfb_parse_level("AVX2", &l), KDFB_ERR_LEVEL);
    assert_int_equal(kdfb_parse_level("avx512", &l), KDFB_ERR_LEVEL);
    assert_int_equal(kdfb_parse_level(NULL, &l), KDFB_ERR_LEVEL);
}

static void test_table(void **state)
{
    static const uint32_t passes[] = { 1, 4, 12 };
    static const double secs[] = { 0.054, -1.0, 12.345 };
    char buf[128];

    (void)state;
    kdfb_format_head(buf, sizeof buf, passes, 3);
    assert_string_equal(buf, "  memory  t=1      t=4      t=12   ");
    kdfb_format_row(buf, sizeof buf, 64, secs, 3);
    assert_string_equal(buf, "  64 MiB  0.05     -        12.35  ");
    kdfb_format_row(buf, sizeof buf, 1024, secs, 1);
    assert_string_equal(buf, "1024 MiB  0.05   ");
}

/* A buffer too short is cut, terminated, never overrun. */
static void test_table_short(void **state)
{
    static const uint32_t passes[] = { 1, 2, 4, 8 };
    static const double secs[] = { 1.0, 2.0, 3.0, 4.0 };
    char buf[16];

    (void)state;
    memset(buf, 'x', sizeof buf);
    kdfb_format_head(buf, 12, passes, 4);
    assert_int_equal(strlen(buf), 11);
    assert_int_equal(buf[12], 'x');
    kdfb_format_row(buf, 12, 64, secs, 4);
    assert_int_equal(strlen(buf), 11);
    assert_int_equal(buf[12], 'x');
    kdfb_format_row(buf, 0, 64, secs, 4);
    assert_int_equal(buf[0], ' ');
}

int run_kdfbenchcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_list),
        cmocka_unit_test(test_list_refusals),
        cmocka_unit_test(test_level),
        cmocka_unit_test(test_table),
        cmocka_unit_test(test_table_short),
    };

    return cmocka_run_group_tests_name("kdfbenchcore", tests, NULL, NULL);
}
