/* Tests for falcon-bios/tools/mkidentargs.c: the words falcon-bios-mkident
 * turns into a record. A tool that wrote a record the loaders refuse would
 * make a rig disk that does not boot for a reason nobody looks for. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include "../tools/mkidentargs.h"

#define ID "0123456789abcdef0123456789abcdef"

static void test_record(void **state)
{
    uint8_t sec[512];
    const char *why = NULL;
    ident_rec r;
    ident_rec back;

    (void)state;
    assert_int_equal(mkident_record("boot", ID, "32", "0x8", &r, &why), 0);
    assert_int_equal(r.role, IDENT_ROLE_BOOT);
    assert_int_equal(r.stage3_sector, 32);
    assert_int_equal(r.marker_sector, 8);
    assert_int_equal(r.install_id[0], 0x01);
    assert_int_equal(r.install_id[15], 0xef);

    /* What it makes, the loaders take. */
    memset(sec, 0, sizeof sec);
    ident_encode_bin(&r, sec);
    assert_int_equal(ident_decode_bin(sec, &back), IDENT_OK);
    assert_true(ident_same_install(&r, &back));
    assert_int_equal(back.stage3_sector, 32);

    assert_int_equal(mkident_record("library", ID, "0", "0", &r, &why), 0);
    assert_int_equal(r.role, IDENT_ROLE_LIBRARY);
    assert_int_equal(mkident_record("shelf", ID, "0", "0", &r, &why), 0);
    assert_int_equal(r.role, IDENT_ROLE_SHELF);
}

static void test_refusals(void **state)
{
    const char *why = NULL;
    ident_rec r;

    (void)state;
    assert_int_not_equal(mkident_record("esp", ID, "0", "0", &r, &why), 0);
    assert_non_null(why);
    assert_int_not_equal(mkident_record("boot", "0123", "32", "8", &r, &why), 0);
    assert_int_not_equal(mkident_record("boot",
        "0123456789ABCDEF0123456789ABCDEF", "32", "8", &r, &why), 0);
    assert_int_not_equal(mkident_record("boot",
        "00000000000000000000000000000000", "32", "8", &r, &why), 0);
    assert_int_not_equal(mkident_record("boot", ID, "65536", "8", &r, &why), 0);
    assert_int_not_equal(mkident_record("boot", ID, "32x", "8", &r, &why), 0);
    assert_int_not_equal(mkident_record("boot", ID, "-1", "8", &r, &why), 0);
    assert_int_not_equal(mkident_record("boot", ID, "", "8", &r, &why), 0);
    /* Only a boot record names sectors. */
    assert_int_not_equal(mkident_record("library", ID, "32", "0", &r, &why), 0);
    assert_int_not_equal(mkident_record("shelf", ID, "0", "8", &r, &why), 0);
    assert_int_not_equal(mkident_record(NULL, ID, "0", "0", &r, &why), 0);
}

static void test_offset(void **state)
{
    uint64_t off = 0;

    (void)state;
    assert_int_equal(mkident_offset("0", &off), 0);
    assert_int_equal(off, 0);
    assert_int_equal(mkident_offset("1048576", &off), 0);
    assert_int_equal(off, 1048576u);
    assert_int_equal(mkident_offset("0x100000", &off), 0);
    assert_int_equal(off, 1048576u);
    assert_int_not_equal(mkident_offset("513", &off), 0);
    assert_int_not_equal(mkident_offset("x", &off), 0);
    assert_int_not_equal(mkident_offset(NULL, &off), 0);
}

int run_mkident_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_record),
        cmocka_unit_test(test_refusals),
        cmocka_unit_test(test_offset),
    };

    return cmocka_run_group_tests_name("mkident", tests, NULL, NULL);
}
