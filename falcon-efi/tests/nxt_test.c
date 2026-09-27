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

/* A Boot#### variable whose path is real: an optional PCI node, a
 * HARDDRIVE node, the file path in one or two FILEPATH nodes, the end. */
static size_t put_node(uint8_t *p, uint8_t type, uint8_t sub,
                       const uint8_t *data, size_t dlen)
{
    size_t n = 4u + dlen;

    p[0] = type;
    p[1] = sub;
    p[2] = (uint8_t)n;
    p[3] = (uint8_t)(n >> 8);
    if (dlen != 0)
        memcpy(p + 4, data, dlen);
    return n;
}

static size_t put_file(uint8_t *p, const char *path)
{
    uint8_t d[256];
    size_t i;

    for (i = 0; path[i] != '\0'; i++) {
        d[2u * i] = (uint8_t)path[i];
        d[2u * i + 1u] = 0;
    }
    d[2u * i] = 0;
    d[2u * i + 1u] = 0;
    return put_node(p, 4, 4, d, 2u * i + 2u);
}

static size_t mk_real(uint8_t *buf, uint8_t sig_type, const char *file_a,
                      const char *file_b)
{
    uint8_t path[512];
    uint8_t hd[38];
    uint8_t pci[2] = { 0, 0x1f };
    size_t n = 0;
    size_t off;
    size_t i;

    memset(hd, 0, sizeof hd);
    hd[0] = 2;                         /* PartitionNumber 2 */
    hd[4] = 0x00; hd[5] = 0x08;        /* PartitionStart 2048 */
    for (i = 0; i < 16u; i++)
        hd[20u + i] = (uint8_t)(0xA0u + i);
    hd[36] = 2;                        /* MBRType: GPT */
    hd[37] = sig_type;
    n += put_node(path + n, 1, 1, pci, sizeof pci);
    n += put_node(path + n, 4, 1, hd, sizeof hd);
    if (file_a != NULL)
        n += put_file(path + n, file_a);
    if (file_b != NULL)
        n += put_file(path + n, file_b);
    n += put_node(path + n, 0x7F, 0xFF, NULL, 0);

    off = mk_option(buf, NXT_LOAD_OPTION_ACTIVE, "Windows Boot Manager",
                    (uint16_t)n, 0);
    memcpy(buf + off, path, n);
    return off + n;
}

static void test_target_windows(void **state)
{
    uint8_t buf[512];
    nxt_target t;
    size_t len = mk_real(buf, NXT_SIG_GPT,
                         "\\EFI\\Microsoft\\Boot\\bootmgfw.efi", NULL);

    (void)state;
    assert_int_equal(nxt_option_target(buf, len, &t), NXT_OK);
    assert_int_equal(t.has_hd, 1);
    assert_int_equal(t.part, 2);
    assert_int_equal(t.start, 2048);
    assert_int_equal(t.sig_type, NXT_SIG_GPT);
    assert_int_equal(t.sig[0], 0xA0);
    assert_int_equal(t.sig[15], 0xAF);
    assert_string_equal(t.file, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
}

static void test_target_split_file(void **state)
{
    uint8_t buf[512];
    nxt_target t;
    size_t len = mk_real(buf, NXT_SIG_MBR, "\\EFI\\falcon", "shimx64.efi");

    (void)state;
    assert_int_equal(nxt_option_target(buf, len, &t), NXT_OK);
    assert_int_equal(t.sig_type, NXT_SIG_MBR);
    assert_string_equal(t.file, "\\EFI\\falcon\\shimx64.efi");
}

static void test_target_no_media(void **state)
{
    uint8_t buf[256];
    uint8_t path[16];
    uint8_t pci[2] = { 0, 3 };
    nxt_target t;
    size_t n = 0;
    size_t off;

    (void)state;
    n += put_node(path + n, 1, 1, pci, sizeof pci);
    n += put_node(path + n, 0x7F, 0xFF, NULL, 0);
    off = mk_option(buf, NXT_LOAD_OPTION_ACTIVE, "UEFI PXE", (uint16_t)n, 0);
    memcpy(buf + off, path, n);
    assert_int_equal(nxt_option_target(buf, off + n, &t), NXT_OK);
    assert_int_equal(t.has_hd, 0);
    assert_string_equal(t.file, "");
}

static void test_target_hostile(void **state)
{
    uint8_t buf[512];
    nxt_target t;
    size_t len = mk_real(buf, NXT_SIG_GPT, "\\EFI\\x.efi", NULL);
    size_t hd_at = len - 4u - (4u + 2u * 11u) - 42u;

    (void)state;
    /* A node claiming to run past the path. */
    buf[hd_at + 2u] = 0xFF;
    assert_int_equal(nxt_option_target(buf, len, &t), NXT_ERR_PATH);
    /* A node shorter than its header. */
    buf[hd_at + 2u] = 2;
    assert_int_equal(nxt_option_target(buf, len, &t), NXT_ERR_PATH);
    assert_int_equal(nxt_option_target(NULL, len, &t), NXT_ERR_SHORT);
    assert_int_equal(nxt_option_target(buf, len, NULL), NXT_ERR_SHORT);

    /* A path unit outside printable ASCII leaves no file, not half of
     * one. */
    len = mk_real(buf, NXT_SIG_GPT, "\\EFI\\x.efi", NULL);
    buf[len - 4u - 6u] = 0x01;
    assert_int_equal(nxt_option_target(buf, len, &t), NXT_OK);
    assert_int_equal(t.has_hd, 1);
    assert_string_equal(t.file, "");
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
        cmocka_unit_test(test_target_windows),
        cmocka_unit_test(test_target_split_file),
        cmocka_unit_test(test_target_no_media),
        cmocka_unit_test(test_target_hostile),
    };

    return cmocka_run_group_tests_name("nxt", tests, NULL, NULL);
}
