/* Tests for falcon-core/src/gz/gz.c: inflate and the gzip member around it,
 * against streams Python's zlib wrote (gz_vectors.h): each block type RFC
 * 1951 has, a whole member, and the ways a floppy's worth of it can arrive
 * wrong. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <gz/gz.h>

#include "gz_vectors.h"

static uint8_t g_out[4096];

static void text(uint8_t *buf, size_t *len)
{
    static const char line[] =
        "falcon jump reads less off the floppy, since a floppy reads slowly. ";
    size_t n = 0;
    unsigned i;

    for (i = 0; i < TEXT_REPEAT; i++) {
        memcpy(buf + n, line, sizeof line - 1u);
        n += sizeof line - 1u;
    }
    *len = n;
}

static void test_blocks(void **state)
{
    uint8_t want[4096];
    size_t want_len;
    size_t len;
    size_t used;

    (void)state;
    text(want, &want_len);

    assert_int_equal(gz_inflate(k_fixed, sizeof k_fixed, g_out, sizeof g_out,
                                &len, &used), GZ_OK);
    assert_int_equal(len, want_len);
    assert_memory_equal(g_out, want, len);
    assert_int_equal(used, sizeof k_fixed);

    assert_int_equal(gz_inflate(k_dynamic, sizeof k_dynamic, g_out,
                                sizeof g_out, &len, &used), GZ_OK);
    assert_int_equal(len, want_len);
    assert_memory_equal(g_out, want, len);

    assert_int_equal(gz_inflate(k_stored, sizeof k_stored, g_out,
                                sizeof g_out, &len, &used), GZ_OK);
    assert_int_equal(len, 300);
    /* A stored block is its bytes after a five-byte header. */
    assert_memory_equal(g_out, k_stored + 5, 300);
}

static void test_member(void **state)
{
    uint8_t want[4096];
    uint8_t bad[sizeof k_member];
    size_t want_len;
    size_t len;

    (void)state;
    text(want, &want_len);
    assert_int_equal(gz_member(k_member, sizeof k_member, g_out, sizeof g_out,
                               &len), GZ_OK);
    assert_int_equal(len, want_len);
    assert_memory_equal(g_out, want, len);
    assert_int_equal(gz_crc32(0, want, want_len),
                     (uint32_t)k_member[sizeof k_member - 8] |
                     ((uint32_t)k_member[sizeof k_member - 7] << 8) |
                     ((uint32_t)k_member[sizeof k_member - 6] << 16) |
                     ((uint32_t)k_member[sizeof k_member - 5] << 24));

    /* The CRC flipped: inflated, and refused. */
    memcpy(bad, k_member, sizeof bad);
    bad[sizeof bad - 8] ^= 1u;
    assert_int_equal(gz_member(bad, sizeof bad, g_out, sizeof g_out, &len),
                     GZ_ERR_CHECK);
    /* The length flipped. */
    memcpy(bad, k_member, sizeof bad);
    bad[sizeof bad - 4] ^= 1u;
    assert_int_equal(gz_member(bad, sizeof bad, g_out, sizeof g_out, &len),
                     GZ_ERR_CHECK);
    /* Not gzip, and not deflate inside it. */
    memcpy(bad, k_member, sizeof bad);
    bad[0] = 0;
    assert_int_equal(gz_member(bad, sizeof bad, g_out, sizeof g_out, &len),
                     GZ_ERR_FORMAT);
    memcpy(bad, k_member, sizeof bad);
    bad[2] = 7;
    assert_int_equal(gz_member(bad, sizeof bad, g_out, sizeof g_out, &len),
                     GZ_ERR_FORMAT);
    memcpy(bad, k_member, sizeof bad);
    bad[3] = 0x80;
    assert_int_equal(gz_member(bad, sizeof bad, g_out, sizeof g_out, &len),
                     GZ_ERR_FORMAT);
}

static void test_damage(void **state)
{
    uint8_t bad[sizeof k_dynamic];
    size_t len;
    size_t cut;

    (void)state;
    /* Cut short anywhere: an error, never a crash or a false success. */
    for (cut = 0; cut < sizeof k_dynamic; cut++)
        assert_int_not_equal(gz_inflate(k_dynamic, cut, g_out, sizeof g_out,
                                        &len, (size_t *)0), GZ_OK);
    /* Too little room. */
    assert_int_equal(gz_inflate(k_dynamic, sizeof k_dynamic, g_out, 100u,
                                &len, (size_t *)0), GZ_ERR_SPACE);
    assert_int_equal(gz_inflate(k_stored, sizeof k_stored, g_out, 299u,
                                &len, (size_t *)0), GZ_ERR_SPACE);
    /* A stored block whose length and complement disagree. */
    memcpy(bad, k_stored, 5);
    bad[3] ^= 0xFFu;
    assert_int_equal(gz_inflate(bad, 5u, g_out, sizeof g_out, &len,
                                (size_t *)0), GZ_ERR_DATA);
    /* Block type 3, which does not exist. */
    bad[0] = 0x07;
    assert_int_equal(gz_inflate(bad, 1u, g_out, sizeof g_out, &len,
                                (size_t *)0), GZ_ERR_DATA);
    /* Every single-bit flip of the dynamic stream decodes to an error or to
     * something else, never past the buffer: what the checks after it catch
     * is gz_member's to prove, this that it stays in bounds. */
    {
        size_t i;

        for (i = 0; i < sizeof k_dynamic * 8u; i++) {
            memcpy(bad, k_dynamic, sizeof bad);
            bad[i / 8u] ^= (uint8_t)(1u << (i % 8u));
            (void)gz_inflate(bad, sizeof bad, g_out, sizeof g_out, &len,
                             (size_t *)0);
        }
    }
    assert_int_equal(gz_inflate(NULL, 4u, g_out, sizeof g_out, &len,
                                (size_t *)0), GZ_ERR_ARG);
}

/* A distance reaching before the output's start: a fixed-Huffman block of
 * one length/distance pair and nothing written yet. */
static void test_distance_before_start(void **state)
{
    /* BFINAL 1, BTYPE 01; symbol 257 (length 3) is 7 bits 0000001; distance
     * code 0 (distance 1) is 5 bits 00000. */
    static const uint8_t s[] = { 0x03, 0x02, 0x00 };
    size_t len;

    (void)state;
    assert_int_equal(gz_inflate(s, sizeof s, g_out, sizeof g_out, &len,
                                (size_t *)0), GZ_ERR_DATA);
}

int run_gz_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_blocks),
        cmocka_unit_test(test_member),
        cmocka_unit_test(test_damage),
        cmocka_unit_test(test_distance_before_start),
    };

    return cmocka_run_group_tests_name("gz", tests, NULL, NULL);
}
