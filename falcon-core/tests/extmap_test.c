/* Tests for falcon-core/src/media/extmap.c, the extent map the loader hands
 * falcon-hook in an EFI variable.
 *
 * The map crosses a trust boundary twice: the loader writes it from p2's
 * extent map, and the hook reads it from efivarfs inside an initramfs. So the
 * round trip is pinned byte for byte, and every way the decoder can be handed
 * something else is refused.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <media/extmap.h>

static uint8_t g_buf[EXTMAP_HDR_LEN + (EXTMAP_MAX_EXTENTS + 1u) * EXTMAP_EXT_LEN];

static void hdr_for(extmap_hdr *h, uint32_t n, uint64_t size)
{
    uint8_t i;

    memset(h, 0, sizeof(*h));
    for (i = 0; i < 16; i++)
        h->install_id[i] = (uint8_t)(0xA0u + i);
    h->part_lba = 133120;
    h->block_size = 512;
    h->n_extents = n;
    h->iso_size = size;
}

static void test_extmap_round_trip(void **state)
{
    static const extmap_extent ext[3] = {
        { 1048576u, 4096u }, { 8388608u, 1u << 20 }, { 3u << 30, 2048u }
    };
    extmap_hdr h, back;
    extmap_extent e;
    size_t len = 0;
    uint32_t i;

    (void)state;
    hdr_for(&h, 3, 4096u + (1u << 20) + 2048u);
    assert_int_equal(extmap_size(3), 64 + 48);
    assert_int_equal(extmap_encode(&h, ext, g_buf, sizeof g_buf, &len),
                     EXTMAP_OK);
    assert_int_equal(len, 112);
    assert_memory_equal(g_buf, "FLCNXT01", 8);
    assert_int_equal(g_buf[8], 2);
    assert_int_equal(g_buf[12], 64);
    assert_int_equal(g_buf[16], 0xA0);

    assert_int_equal(extmap_decode(g_buf, len, &back), EXTMAP_OK);
    assert_memory_equal(back.install_id, h.install_id, 16);
    assert_int_equal(back.part_lba, 133120);
    assert_int_equal(back.block_size, 512);
    assert_int_equal(back.n_extents, 3);
    assert_int_equal(back.iso_size, h.iso_size);
    for (i = 0; i < 3; i++) {
        extmap_extent_at(g_buf, i, &e);
        assert_int_equal(e.offset, ext[i].offset);
        assert_int_equal(e.length, ext[i].length);
    }
}

static void test_extmap_encode_refusals(void **state)
{
    static extmap_extent many[EXTMAP_MAX_EXTENTS + 1u];
    extmap_extent one = { 0, 100 };
    extmap_hdr h;
    size_t len = 0, i;

    (void)state;
    hdr_for(&h, 1, 100);
    assert_int_equal(extmap_encode(NULL, &one, g_buf, sizeof g_buf, &len),
                     EXTMAP_ERR_BAD_ARG);
    assert_int_equal(extmap_encode(&h, NULL, g_buf, sizeof g_buf, &len),
                     EXTMAP_ERR_BAD_ARG);
    assert_int_equal(extmap_encode(&h, &one, NULL, sizeof g_buf, &len),
                     EXTMAP_ERR_BAD_ARG);
    assert_int_equal(extmap_encode(&h, &one, g_buf, sizeof g_buf, NULL),
                     EXTMAP_ERR_BAD_ARG);
    assert_int_equal(extmap_encode(&h, &one, g_buf, 70, &len),
                     EXTMAP_ERR_NOSPACE);

    h.iso_size = 99;                        /* extents do not add up */
    assert_int_equal(extmap_encode(&h, &one, g_buf, sizeof g_buf, &len),
                     EXTMAP_ERR_RANGE);
    hdr_for(&h, 1, 100);
    h.block_size = 520;
    assert_int_equal(extmap_encode(&h, &one, g_buf, sizeof g_buf, &len),
                     EXTMAP_ERR_RANGE);
    hdr_for(&h, 0, 0);
    assert_int_equal(extmap_encode(&h, &one, g_buf, sizeof g_buf, &len),
                     EXTMAP_ERR_RANGE);
    /* No install ID: a map no record could ever match. */
    hdr_for(&h, 1, 100);
    memset(h.install_id, 0, sizeof h.install_id);
    assert_int_equal(extmap_encode(&h, &one, g_buf, sizeof g_buf, &len),
                     EXTMAP_ERR_RANGE);
    one.length = 0;
    hdr_for(&h, 1, 0);
    assert_int_equal(extmap_encode(&h, &one, g_buf, sizeof g_buf, &len),
                     EXTMAP_ERR_RANGE);

    for (i = 0; i <= EXTMAP_MAX_EXTENTS; i++) {
        many[i].offset = i * 4096u;
        many[i].length = 512;
    }
    hdr_for(&h, EXTMAP_MAX_EXTENTS + 1u, 512u * (EXTMAP_MAX_EXTENTS + 1u));
    assert_int_equal(extmap_encode(&h, many, g_buf, sizeof g_buf, &len),
                     EXTMAP_ERR_RANGE);
    /* The cap itself fits the 8 KiB some firmware allows a variable. */
    hdr_for(&h, EXTMAP_MAX_EXTENTS, 512u * EXTMAP_MAX_EXTENTS);
    assert_int_equal(extmap_encode(&h, many, g_buf, sizeof g_buf, &len),
                     EXTMAP_OK);
    assert_true(len <= 8192u);

    /* A sum that would overflow. */
    many[0].length = ~(uint64_t)0;
    many[1].length = 2;
    hdr_for(&h, 2, 1);
    assert_int_equal(extmap_encode(&h, many, g_buf, sizeof g_buf, &len),
                     EXTMAP_ERR_RANGE);
}

static void test_extmap_decode_refusals(void **state)
{
    static const extmap_extent ext[2] = { { 0, 512 }, { 4096, 512 } };
    extmap_hdr h, back;
    size_t len = 0;
    uint8_t saved;

    (void)state;
    hdr_for(&h, 2, 1024);
    assert_int_equal(extmap_encode(&h, ext, g_buf, sizeof g_buf, &len),
                     EXTMAP_OK);

    assert_int_equal(extmap_decode(NULL, len, &back), EXTMAP_ERR_BAD_ARG);
    assert_int_equal(extmap_decode(g_buf, len, NULL), EXTMAP_ERR_BAD_ARG);
    assert_int_equal(extmap_decode(g_buf, 63, &back), EXTMAP_ERR_FORMAT);
    assert_int_equal(extmap_decode(g_buf, len - 1u, &back), EXTMAP_ERR_FORMAT);

    /* One byte at a time through the fields that must hold exactly. */
    {
        static const size_t at[] = { 0, 7, 8, 12, 40, 44 };
        size_t i;

        for (i = 0; i < sizeof at / sizeof at[0]; i++) {
            saved = g_buf[at[i]];
            g_buf[at[i]] ^= 0x10;
            assert_int_equal(extmap_decode(g_buf, len, &back),
                             EXTMAP_ERR_FORMAT);
            g_buf[at[i]] = saved;
        }
    }
    /* Version 1, the GPT-GUID map: refused as a format, not misread. */
    g_buf[8] = 1;
    assert_int_equal(extmap_decode(g_buf, len, &back), EXTMAP_ERR_FORMAT);
    g_buf[8] = 2;
    /* An all-zero install ID. */
    {
        uint8_t id[16];

        memcpy(id, g_buf + 16, 16);
        memset(g_buf + 16, 0, 16);
        assert_int_equal(extmap_decode(g_buf, len, &back), EXTMAP_ERR_FORMAT);
        memcpy(g_buf + 16, id, 16);
    }
    /* The size field disagreeing with the extents. */
    g_buf[48] ^= 1;
    assert_int_equal(extmap_decode(g_buf, len, &back), EXTMAP_ERR_RANGE);
    g_buf[48] ^= 1;
    /* An extent of zero length. */
    memset(g_buf + 64 + 8, 0, 8);
    assert_int_equal(extmap_decode(g_buf, len, &back), EXTMAP_ERR_RANGE);
    /* An extent whose end overflows. */
    memset(g_buf + 64, 0xFF, 8);
    g_buf[64 + 8] = 1;
    assert_int_equal(extmap_decode(g_buf, len, &back), EXTMAP_ERR_RANGE);
    assert_int_equal(extmap_encode(&h, ext, g_buf, sizeof g_buf, &len),
                     EXTMAP_OK);
    assert_int_equal(extmap_decode(g_buf, len, &back), EXTMAP_OK);
}

int run_extmap_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_extmap_round_trip),
        cmocka_unit_test(test_extmap_encode_refusals),
        cmocka_unit_test(test_extmap_decode_refusals),
    };

    return cmocka_run_group_tests_name("extmap", tests, NULL, NULL);
}
