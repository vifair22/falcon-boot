/* Tests for falcon-bios/src/hw/pcc/pcccore.c: a PC Card's CIS and the ExCA
 * window registers falcon jump's PCC.MOD programs (DESIGN Section 13.4).
 *
 * The CIS is the one falcon-bios/tools/qemu-pcc/falcon-pcc.c gives the rig's
 * CompactFlash card, which is the CF standard's shape: a memory-mapped
 * default entry, the contiguous-I/O entry, and the primary IDE range.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <hw/pcc/pcccore.h>

static const uint8_t k_cis[] = {
    0x01, 0x03, 0xD9, 0x01, 0xFF,                         /* DEVICE */
    0x21, 0x02, 0x04, 0x01,                               /* FUNCID disk */
    0x1A, 0x05, 0x01, 0x03, 0x00, 0x02, 0x0F,             /* CONFIG 0x200 */
    0x1B, 0x09, 0xC0, 0xC0, 0x21, 0x21, 0xB5, 0x1E, 0x4D, 0x08, 0x00,
    0x1B, 0x07, 0x01, 0x09, 0x21, 0xB5, 0x1E, 0x4D, 0x64,
    0x1B, 0x0E, 0x02, 0x09, 0x21, 0xB5, 0x1E, 0x4D, 0xEA, 0x61, 0xF0, 0x01,
    0x07, 0xF6, 0x03, 0x01,
    0xFF
};

static void test_cf_card(void **state)
{
    pcc_cis c;

    (void)state;
    assert_int_equal(pcc_cis_parse(k_cis, sizeof k_cis, &c), 0);
    assert_int_equal(c.funcid, FUNCID_FIXED_DISK);
    assert_int_equal(c.config_base, 0x200u);
    /* The contiguous-I/O entry, not the memory-mapped default nor the
     * fixed primary range. */
    assert_int_equal(c.index, 1);
    assert_int_equal(c.io_lines, 4);
    assert_int_equal(c.io_len, 0);
}

/* A card offering only the fixed range: that range is taken. */
static void test_fixed_only(void **state)
{
    static const uint8_t cis[] = {
        0x21, 0x02, 0x04, 0x01,
        0x1A, 0x05, 0x01, 0x03, 0x00, 0x02, 0x0F,
        0x1B, 0x0E, 0x02, 0x09, 0x21, 0xB5, 0x1E, 0x4D, 0xEA, 0x61, 0xF0,
        0x01, 0x07, 0xF6, 0x03, 0x01,
        0xFF
    };
    pcc_cis c;

    (void)state;
    assert_int_equal(pcc_cis_parse(cis, sizeof cis, &c), 0);
    assert_int_equal(c.index, 2);
    assert_int_equal(c.io_lines, 10);
    assert_int_equal(c.io_base, 0x1F0);
    assert_int_equal(c.io_len, 8);
}

static void test_refusals(void **state)
{
    static const uint8_t no_config[] = {
        0x21, 0x02, 0x04, 0x01,
        0x1B, 0x07, 0x01, 0x09, 0x21, 0xB5, 0x1E, 0x4D, 0x64,
        0xFF
    };
    static const uint8_t no_io[] = {
        0x1A, 0x05, 0x01, 0x03, 0x00, 0x02, 0x0F,
        0x1B, 0x09, 0xC0, 0xC0, 0x21, 0x21, 0xB5, 0x1E, 0x4D, 0x08, 0x00,
        0xFF
    };
    uint8_t cut[20];
    pcc_cis c;

    (void)state;
    assert_int_equal(pcc_cis_parse(no_config, sizeof no_config, &c), -1);
    assert_int_equal(pcc_cis_parse(no_io, sizeof no_io, &c), -1);
    /* A tuple whose length runs past the end. */
    memcpy(cut, k_cis, sizeof cut);
    assert_int_equal(pcc_cis_parse(cut, sizeof cut, &c), -1);
    assert_int_equal(pcc_cis_parse(NULL, 4u, &c), -1);
}

static void test_windows(void **state)
{
    uint8_t m[6];
    uint8_t io[4];

    (void)state;
    /* 4 KB of attribute memory at card 0, seen at host 0xD0000: the offset
     * is -0xD0 pages, modulo 2^14. */
    assert_int_equal(pcc_mem_window(0xD0000u, 0x1000u, 0, 1, m), 0);
    assert_int_equal(m[0], 0xD0);
    assert_int_equal(m[1], 0x00);
    assert_int_equal(m[2], 0xD0);
    assert_int_equal(m[3], 0x00);
    assert_int_equal((m[4] | ((m[5] & 0x3Fu) << 8)), (0x4000u - 0xD0u));
    assert_int_equal(m[5] & 0x40u, 0x40u);
    /* Card = host: a zero offset. */
    assert_int_equal(pcc_mem_window(0xC8000u, 0x2000u, 0xC8000u, 0, m), 0);
    assert_int_equal(m[2], 0xC9);
    assert_int_equal(m[4], 0);
    assert_int_equal(m[5], 0);
    assert_int_equal(pcc_mem_window(0xD0800u, 0x1000u, 0, 1, m), -1);
    assert_int_equal(pcc_mem_window(0xFFF000u, 0x2000u, 0, 1, m), -1);
    assert_int_equal(pcc_mem_window(0xD0000u, 0, 0, 1, m), -1);

    assert_int_equal(pcc_io_window(0x1E0u, 16u, io), 0);
    assert_int_equal(io[0], 0xE0);
    assert_int_equal(io[1], 0x01);
    assert_int_equal(io[2], 0xEF);
    assert_int_equal(io[3], 0x01);
    assert_int_equal(pcc_io_window(0xFFF8u, 16u, io), -1);
    assert_int_equal(pcc_io_window(0x1E0u, 0, io), -1);
}

int run_pcccore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_cf_card),
        cmocka_unit_test(test_fixed_only),
        cmocka_unit_test(test_refusals),
        cmocka_unit_test(test_windows),
    };

    return cmocka_run_group_tests_name("pcccore", tests, NULL, NULL);
}
