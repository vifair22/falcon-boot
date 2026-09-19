/* Tests for the edit half of falcon-core/src/fs/udf.c: a file made longer by
 * bytes appended past the image's end, as the loader serves Windows'
 * boot.wim (falcon-core/src/wim/wim.h).
 *
 * The edits are applied to a copy of an image mkudffs wrote (the short_ad
 * and the long_ad fixture of falcon-core/tools/mk-test-udf.sh): the File
 * Entry and the Partition Descriptors rewritten, the moved tail and the new
 * bytes appended. The result must read back as the old file followed by the
 * new bytes, through this driver; FALCON_UDFEDIT_KEEP leaves the images for
 * the kernel's UDF driver to be checked against by hand.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fs/udf.h>

#ifndef MEDIA_FIXTURE_DIR
#define MEDIA_FIXTURE_DIR "build"
#endif

static uint8_t *g_img;
static size_t g_len;

static int mem_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    if (off > g_len || len > g_len - off)
        return -1;
    memcpy(buf, g_img + off, (size_t)len);
    return 0;
}

static void load(const char *name, size_t extra)
{
    char path[512];
    FILE *f;
    long n;

    snprintf(path, sizeof path, "%s/%s", MEDIA_FIXTURE_DIR, name);
    f = fopen(path, "rb");
    assert_non_null(f);
    assert_int_equal(fseek(f, 0, SEEK_END), 0);
    n = ftell(f);
    assert_true(n > 0);
    rewind(f);
    free(g_img);
    g_img = calloc(1, (size_t)n + extra);
    assert_non_null(g_img);
    assert_int_equal(fread(g_img, 1, (size_t)n, f), (size_t)n);
    fclose(f);
    g_len = (size_t)n;
}

static udf_vol g_v;
static udf_file g_f;

/* Appends add bytes of a pattern to path, and checks the result. */
static void append_and_check(const char *fixture, const char *path,
                             size_t add)
{
    udf_dirent e;
    uint64_t old_size, fe_off;
    uint32_t pd[2], moved = 0, lbn;
    size_t npd = 0, i, img_blocks, grow;
    uint8_t *old, *got;

    load(fixture, add + 2u * UDF_BLOCK);
    assert_int_equal(udf_mount(&g_v, mem_read, NULL), UDF_OK);
    assert_int_equal(udf_lookup(&g_v, path, &e), UDF_OK);
    old_size = e.size;
    old = malloc(old_size ? old_size : 1);
    assert_non_null(old);
    assert_int_equal(udf_file_open(&g_v, &g_f, &e), UDF_OK);
    assert_int_equal(udf_file_read(&g_f, 0, old_size, old), UDF_OK);

    /* The new extent starts at the first block past the image. */
    img_blocks = g_len / UDF_BLOCK;
    lbn = (uint32_t)img_blocks - g_v.part_start;
    fe_off = ((uint64_t)g_v.part_start + e.icb_lbn) * UDF_BLOCK;
    assert_int_equal(udf_fe_append(g_img + fe_off, e.icb_lbn, add, lbn,
                                   &moved), UDF_OK);
    assert_int_equal(moved, old_size % UDF_BLOCK);
    grow = (moved + add + UDF_BLOCK - 1u) / UDF_BLOCK;

    assert_int_equal(udf_pd_blocks(&g_v, pd, &npd), UDF_OK);
    assert_true(npd >= 1);
    for (i = 0; i < npd; i++)
        assert_int_equal(udf_pd_set_length(g_img + (size_t)pd[i] * UDF_BLOCK,
                                           pd[i],
                                           (uint32_t)(img_blocks + grow) -
                                               g_v.part_start), UDF_OK);

    /* The appended blocks: the moved tail, then the new bytes. */
    memcpy(g_img + g_len, old + (old_size - moved), moved);
    for (i = 0; i < add; i++)
        g_img[g_len + moved + i] = (uint8_t)(i * 7u + 1u);
    g_len += grow * UDF_BLOCK;

    assert_int_equal(udf_mount(&g_v, mem_read, NULL), UDF_OK);
    assert_int_equal(udf_lookup(&g_v, path, &e), UDF_OK);
    assert_int_equal(e.size, old_size + add);
    got = malloc(old_size + add);
    assert_non_null(got);
    assert_int_equal(udf_file_open(&g_v, &g_f, &e), UDF_OK);
    assert_int_equal(udf_file_read(&g_f, 0, old_size + add, got), UDF_OK);
    assert_memory_equal(got, old, old_size);
    for (i = 0; i < add; i++)
        assert_int_equal(got[old_size + i], (uint8_t)(i * 7u + 1u));

    /* FALCON_UDFEDIT_KEEP=DIR leaves the edited image and the expected
     * file there, for a reader of its own to be pointed at (the kernel's:
     * mount -o loop,ro, then cmp). 7-Zip is not one: it refuses mkudffs's
     * images before any edit. */
    if (getenv("FALCON_UDFEDIT_KEEP") != NULL) {
        char p1[512], p2[512];
        FILE *f;

        snprintf(p1, sizeof p1, "%s/%s.img", getenv("FALCON_UDFEDIT_KEEP"),
                 path + 1);
        snprintf(p2, sizeof p2, "%s/%s.want", getenv("FALCON_UDFEDIT_KEEP"),
                 path + 1);
        for (i = 0; p1[i] != '\0'; i++) {
            if (i > strlen(getenv("FALCON_UDFEDIT_KEEP")) && p1[i] == '/')
                p1[i] = '_';
        }
        for (i = 0; p2[i] != '\0'; i++) {
            if (i > strlen(getenv("FALCON_UDFEDIT_KEEP")) && p2[i] == '/')
                p2[i] = '_';
        }
        f = fopen(p1, "wb");
        assert_non_null(f);
        assert_int_equal(fwrite(g_img, 1, g_len, f), g_len);
        fclose(f);
        f = fopen(p2, "wb");
        assert_non_null(f);
        assert_int_equal(fwrite(got, 1, old_size + add, f), old_size + add);
        fclose(f);
    }
    free(got);
    free(old);
}

static void test_udf_append_short_ad(void **state)
{
    (void)state;
    /* A partial last block: its bytes move to the new extent. */
    append_and_check("test-udf.img", "/sizes/s1048583.bin", 5000);
    append_and_check("test-udf.img", "/sizes/s2049.bin", 1);
    /* A file of whole blocks: nothing moves, one extent is added. */
    append_and_check("test-udf.img", "/big.bin", 70000);
}

static void test_udf_append_long_ad(void **state)
{
    (void)state;
    append_and_check("test-udf-long.img", "/sizes/s1048583.bin", 3);
    append_and_check("test-udf-long.img", "/big.bin", 4096);
}

static void test_udf_edit_refusals(void **state)
{
    uint8_t blk[UDF_BLOCK];
    uint32_t moved, pd[2];
    size_t n;
    udf_dirent e;

    (void)state;
    memset(blk, 0, sizeof blk);
    assert_int_equal(udf_fe_append(blk, 0, 1, 0, &moved), UDF_ERR_CORRUPT);
    assert_int_equal(udf_fe_append(NULL, 0, 1, 0, &moved), UDF_ERR_BAD_ARG);
    assert_int_equal(udf_fe_append(blk, 0, 0, 0, &moved), UDF_ERR_BAD_ARG);
    assert_int_equal(udf_pd_set_length(blk, 0, 1), UDF_ERR_CORRUPT);
    assert_int_equal(udf_pd_set_length(NULL, 0, 1), UDF_ERR_BAD_ARG);
    assert_int_equal(udf_avdp_at(blk, 1), UDF_ERR_CORRUPT);
    assert_int_equal(udf_avdp_at(NULL, 1), UDF_ERR_BAD_ARG);
    assert_int_equal(udf_pd_blocks(NULL, pd, &n), UDF_ERR_BAD_ARG);

    load("test-udf.img", UDF_BLOCK);
    assert_int_equal(udf_mount(&g_v, mem_read, NULL), UDF_OK);
    /* The anchor, copied to one block past the end, mounts from there
     * too: the second anchor a reader may look for. */
    memcpy(blk, g_img + 256u * UDF_BLOCK, sizeof blk);
    assert_int_equal(udf_avdp_at(blk, (uint32_t)(g_len / UDF_BLOCK)), UDF_OK);
    memcpy(g_img + 256u * UDF_BLOCK, blk, sizeof blk);
    assert_int_equal(udf_mount(&g_v, mem_read, NULL), UDF_ERR_BAD_SUPER);
    assert_int_equal(udf_avdp_at(g_img + 256u * UDF_BLOCK, 256u), UDF_OK);
    assert_int_equal(udf_mount(&g_v, mem_read, NULL), UDF_OK);
    assert_int_equal(udf_lookup(&g_v, "/nope", &e), UDF_ERR_NOT_FOUND);
    assert_int_equal(udf_lookup(&g_v, "/big.bin/x", &e), UDF_ERR_NOT_FOUND);
    assert_int_equal(udf_lookup(&g_v, "/", &e), UDF_ERR_BAD_ARG);
    assert_int_equal(udf_lookup(&g_v, "/MANY/ENTRY-001.TXT", &e), UDF_OK);
    /* Its data is embedded in the entry: nothing to extend. */
    {
        uint8_t fe[UDF_BLOCK];

        memcpy(fe, g_img + ((size_t)g_v.part_start + e.icb_lbn) * UDF_BLOCK,
               sizeof fe);
        assert_int_equal(udf_fe_append(fe, e.icb_lbn, 1, 0, &moved),
                         UDF_ERR_UNSUPPORTED);
    }
    assert_int_equal(udf_lookup(NULL, "/a", &e), UDF_ERR_BAD_ARG);
}

static int teardown(void **state)
{
    (void)state;
    free(g_img);
    g_img = NULL;
    return 0;
}

int run_udfedit_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_udf_append_short_ad, teardown),
        cmocka_unit_test_teardown(test_udf_append_long_ad, teardown),
        cmocka_unit_test_teardown(test_udf_edit_refusals, teardown),
    };

    return cmocka_run_group_tests_name("udf edits", tests, NULL, NULL);
}
