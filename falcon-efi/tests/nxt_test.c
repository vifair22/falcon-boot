/* Tests for falcon-efi/src/nxt/nxtcore.c, the parsing half of the
 * stationary build's NXT rows (docs/DESIGN.md Section 13.4).
 *
 * Every byte parsed there was written into NVRAM by something other than
 * falcon: an OS installer, a firmware update, a vendor tool. The hostile
 * shapes below are what a careless writer leaves behind, and each has to end
 * in a refusal rather than a row or a read past the buffer.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <nxt/nxtcore.h>

/* A Boot#### variable: attributes, a path length, a description, then that
 * many bytes of path. Returns its length. */
static size_t mk_option(uint8_t *buf, uint32_t attrs, const char *desc,
                        uint16_t path_len, size_t path_have)
{
    size_t off = 0;
    size_t i;

    buf[off++] = (uint8_t)attrs;
    buf[off++] = (uint8_t)(attrs >> 8);
    buf[off++] = (uint8_t)(attrs >> 16);
    buf[off++] = (uint8_t)(attrs >> 24);
    buf[off++] = (uint8_t)path_len;
    buf[off++] = (uint8_t)(path_len >> 8);
    for (i = 0; desc[i] != '\0'; i++) {
        buf[off++] = (uint8_t)desc[i];
        buf[off++] = 0;
    }
    buf[off++] = 0;
    buf[off++] = 0;
    memset(buf + off, 0x7F, path_have);
    return off + path_have;
}

static int label_is(const uint16_t *label, const char *want)
{
    size_t i;

    for (i = 0; want[i] != '\0'; i++)
        if (label[i] != (uint16_t)(unsigned char)want[i])
            return 0;
    return label[i] == 0;
}

static void test_option_windows(void **state)
{
    uint8_t buf[256];
    uint16_t label[NXT_LABEL_MAX];
    uint32_t attrs = 0;
    size_t len = mk_option(buf, NXT_LOAD_OPTION_ACTIVE,
                           "Windows Boot Manager", 40, 40);

    (void)state;
    assert_int_equal(nxt_option_parse(buf, len, &attrs, label), NXT_OK);
    assert_int_equal(attrs, NXT_LOAD_OPTION_ACTIVE);
    assert_true(label_is(label, "Windows Boot Manager"));
    assert_true(nxt_option_offered(attrs));
}

static void test_option_optional_data(void **state)
{
    /* Bytes past the path are the entry's optional data (Windows puts a
     * WINDOWS\0 blob there); they are allowed and not read. */
    uint8_t buf[256];
    uint16_t label[NXT_LABEL_MAX];
    uint32_t attrs = 0;
    size_t len = mk_option(buf, NXT_LOAD_OPTION_ACTIVE, "UEFI OS", 10, 30);

    (void)state;
    assert_int_equal(nxt_option_parse(buf, len, &attrs, label), NXT_OK);
    assert_true(label_is(label, "UEFI OS"));
}

static void test_option_empty_description(void **state)
{
    uint8_t buf[64];
    uint16_t label[NXT_LABEL_MAX];
    uint32_t attrs = 0;
    size_t len = mk_option(buf, NXT_LOAD_OPTION_ACTIVE, "", 4, 4);

    (void)state;
    assert_int_equal(nxt_option_parse(buf, len, &attrs, label), NXT_OK);
    assert_int_equal(label[0], 0);
}

static void test_option_long_description_is_cut(void **state)
{
    static uint8_t buf[1024];
    char desc[200];
    uint16_t label[NXT_LABEL_MAX];
    uint32_t attrs = 0;
    size_t len;

    (void)state;
    memset(desc, 'x', sizeof desc - 1u);
    desc[sizeof desc - 1u] = '\0';
    len = mk_option(buf, NXT_LOAD_OPTION_ACTIVE, desc, 4, 4);
    assert_int_equal(nxt_option_parse(buf, len, &attrs, label), NXT_OK);
    assert_int_equal(label[NXT_LABEL_MAX - 2u], 'x');
    assert_int_equal(label[NXT_LABEL_MAX - 1u], 0);
}

static void test_option_short(void **state)
{
    uint8_t buf[8] = { 1, 0, 0, 0, 4, 0, 0, 0 };
    uint16_t label[NXT_LABEL_MAX];
    uint32_t attrs = 0;

    (void)state;
    assert_int_equal(nxt_option_parse(buf, 7, &attrs, label), NXT_ERR_SHORT);
    assert_int_equal(nxt_option_parse(NULL, 8, &attrs, label),
                     NXT_ERR_SHORT);
}

static void test_option_unterminated_description(void **state)
{
    uint8_t buf[64];
    uint16_t label[NXT_LABEL_MAX];
    uint32_t attrs = 0;
    size_t len = mk_option(buf, NXT_LOAD_OPTION_ACTIVE, "abc", 0, 0);

    (void)state;
    /* Cut before the description's terminator. */
    assert_int_equal(nxt_option_parse(buf, len - 2u, &attrs, label),
                     NXT_ERR_DESC);
    assert_int_equal(label[0], 0);
    /* And an odd length that splits the terminator. */
    assert_int_equal(nxt_option_parse(buf, len - 1u, &attrs, label),
                     NXT_ERR_DESC);
}

static void test_option_path_overruns(void **state)
{
    uint8_t buf[64];
    uint16_t label[NXT_LABEL_MAX];
    uint32_t attrs = 0;
    size_t len = mk_option(buf, NXT_LOAD_OPTION_ACTIVE, "Boot", 20, 19);

    (void)state;
    assert_int_equal(nxt_option_parse(buf, len, &attrs, label),
                     NXT_ERR_PATH);
    assert_int_equal(label[0], 0);
}

static void test_offered(void **state)
{
    (void)state;
    assert_true(nxt_option_offered(NXT_LOAD_OPTION_ACTIVE));
    assert_false(nxt_option_offered(0));
    assert_false(nxt_option_offered(NXT_LOAD_OPTION_ACTIVE |
                                    NXT_LOAD_OPTION_HIDDEN));
    /* LOAD_OPTION_CATEGORY_APP: a firmware's own applications. */
    assert_false(nxt_option_offered(NXT_LOAD_OPTION_ACTIVE | 0x00000100u));
    /* FORCE_RECONNECT (0x2) says nothing about offering. */
    assert_true(nxt_option_offered(NXT_LOAD_OPTION_ACTIVE | 0x00000002u));
}

static void test_order(void **state)
{
    const uint8_t buf[] = { 0x03, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
                            0x01, 0x10, 0xFF };
    uint16_t out[8];

    (void)state;
    /* 0003 0000 000A (0000 again, dropped) 1001, and the odd byte ignored. */
    assert_int_equal(nxt_order_parse(buf, sizeof buf, out, 8), 4);
    assert_int_equal(out[0], 0x0003);
    assert_int_equal(out[1], 0x0000);
    assert_int_equal(out[2], 0x000A);
    assert_int_equal(out[3], 0x1001);
    assert_int_equal(nxt_order_parse(buf, sizeof buf, out, 2), 2);
    assert_int_equal(nxt_order_parse(NULL, 4, out, 8), 0);
    assert_int_equal(nxt_order_parse(buf, 1, out, 8), 0);
}

int run_nxt_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_option_windows),
        cmocka_unit_test(test_option_optional_data),
        cmocka_unit_test(test_option_empty_description),
        cmocka_unit_test(test_option_long_description_is_cut),
        cmocka_unit_test(test_option_short),
        cmocka_unit_test(test_option_unterminated_description),
        cmocka_unit_test(test_option_path_overruns),
        cmocka_unit_test(test_offered),
        cmocka_unit_test(test_order),
    };

    return cmocka_run_group_tests_name("nxt", tests, NULL, NULL);
}
