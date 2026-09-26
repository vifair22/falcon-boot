/* Tests for falcon-winjump/src/winjumpcore.c: the arithmetic winjump does to
 * feed falcon-vdisk. The Windows I/O around it (enumerate the disks, read
 * their GPTs, load the driver) is a thin layer over Win32 that runs only in a
 * WinPE; what must not be wrong is here.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <winjumpcore.h>
#include <media/extmap.h>

/* Build a GPT header sector and its entry array, one entry carrying `guid`. */
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static void make_gpt(uint8_t hdr[512], uint8_t entries[4 * 128],
                     int which, const uint8_t guid[16], uint64_t first_lba)
{
    int i;

    memset(hdr, 0, 512);
    memcpy(hdr, "EFI PART", 8);
    put_le64(hdr + 72, 2);      /* entries at LBA 2 */
    put_le32(hdr + 80, 4);      /* four entries */
    put_le32(hdr + 84, 128);    /* 128 bytes each */

    memset(entries, 0, 4 * 128);
    for (i = 0; i < 4; i++) {
        uint8_t *e = entries + i * 128;

        /* a plausible type GUID so the slot is not "empty" */
        e[0] = (uint8_t)(0x10 + i);
        if (i == which) {
            memcpy(e + 16, guid, 16);
            put_le64(e + 32, first_lba);
        } else {
            memset(e + 16, (uint8_t)(0x40 + i), 16);
            put_le64(e + 32, 1000u + (uint64_t)i);
        }
    }
}

static void test_gpt_match(void **state)
{
    uint8_t hdr[512], entries[4 * 128];
    static const uint8_t guid[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01
    };
    uint64_t lba = 0;

    (void)state;
    make_gpt(hdr, entries, 2, guid, 133120u);
    assert_int_equal(wj_gpt_match(hdr, entries, sizeof entries, guid, &lba),
                     WJ_OK);
    assert_int_equal((long long)lba, 133120);
}

static void test_gpt_no_match(void **state)
{
    uint8_t hdr[512], entries[4 * 128];
    static const uint8_t guid[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01
    };
    uint8_t other[16];
    uint64_t lba = 0;

    (void)state;
    make_gpt(hdr, entries, 2, guid, 133120u);
    memset(other, 0x5a, sizeof other);
    assert_int_equal(wj_gpt_match(hdr, entries, sizeof entries, other, &lba),
                     WJ_ERR_NOMATCH);
}

static void test_gpt_not_a_table(void **state)
{
    uint8_t hdr[512], entries[4 * 128];
    static const uint8_t guid[16] = { 0 };
    uint64_t lba = 0;

    (void)state;
    make_gpt(hdr, entries, 0, guid, 1u);
    memcpy(hdr, "NOT PART", 8);
    assert_int_equal(wj_gpt_match(hdr, entries, sizeof entries, guid, &lba),
                     WJ_ERR_FORMAT);
}

/* Encode a map of `n` runs and hand it back, with its header. */
static uint8_t g_map[EXTMAP_HDR_LEN + 8u * EXTMAP_EXT_LEN];

static void encode_map(extmap_hdr *h, const extmap_extent *ext, uint32_t n,
                       uint64_t part_lba, uint32_t bs)
{
    uint64_t total = 0;
    uint32_t i;
    size_t len = 0;

    memset(h, 0, sizeof *h);
    for (i = 0; i < 16; i++)
        h->part_guid[i] = (uint8_t)(0xA0u + i);
    h->part_lba = part_lba;
    h->block_size = bs;
    h->n_extents = n;
    for (i = 0; i < n; i++)
        total += ext[i].length;
    h->iso_size = total;
    assert_int_equal(extmap_encode(h, ext, g_map, sizeof g_map, &len),
                     EXTMAP_OK);
}

static void test_backing_extents(void **state)
{
    static const extmap_extent ext[2] = {
        { 4096u, 1u << 20 },        /* 4 KiB into the partition, 1 MiB long */
        { 2u << 20, 2048u }
    };
    extmap_hdr h;
    wj_extent out[8];
    uint32_t n = 0;

    (void)state;
    encode_map(&h, ext, 2, 133120u, 512u);
    assert_int_equal(wj_backing_extents(&h, g_map, sizeof g_map, out, 8, &n),
                     WJ_OK);
    assert_int_equal(n, 2u);

    /* disk_offset = part_lba*block_size + run offset; iso_offset accumulates. */
    assert_int_equal((long long)out[0].iso_offset, 0);
    assert_int_equal((long long)out[0].disk_offset, 133120LL * 512 + 4096);
    assert_int_equal((long long)out[0].length, 1 << 20);

    assert_int_equal((long long)out[1].iso_offset, 1 << 20);
    assert_int_equal((long long)out[1].disk_offset, 133120LL * 512 + (2 << 20));
    assert_int_equal((long long)out[1].length, 2048);
}

static void test_backing_nospace(void **state)
{
    static const extmap_extent ext[2] = {
        { 4096u, 4096u }, { 8192u, 4096u }
    };
    extmap_hdr h;
    wj_extent out[1];
    uint32_t n = 0;

    (void)state;
    encode_map(&h, ext, 2, 64u, 512u);
    assert_int_equal(wj_backing_extents(&h, g_map, sizeof g_map, out, 1, &n),
                     WJ_ERR_NOSPACE);
}

static void test_backing_bad_args(void **state)
{
    extmap_hdr h;
    wj_extent out[4];
    uint32_t n = 0;
    uint64_t lba = 0;
    uint8_t guid[16] = { 0 };
    uint8_t ents[128] = { 0 };
    static const extmap_extent ext[1] = { { 0u, 4096u } };

    (void)state;
    encode_map(&h, ext, 1, 64u, 512u);
    assert_int_equal(wj_backing_extents(NULL, g_map, sizeof g_map, out, 4, &n),
                     WJ_ERR_BAD_ARG);
    assert_int_equal(wj_backing_extents(&h, NULL, sizeof g_map, out, 4, &n),
                     WJ_ERR_BAD_ARG);
    assert_int_equal(wj_gpt_match(NULL, ents, sizeof ents, guid, &lba),
                     WJ_ERR_BAD_ARG);
}

int run_winjumpcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_gpt_match),
        cmocka_unit_test(test_gpt_no_match),
        cmocka_unit_test(test_gpt_not_a_table),
        cmocka_unit_test(test_backing_extents),
        cmocka_unit_test(test_backing_nospace),
        cmocka_unit_test(test_backing_bad_args),
    };
    return cmocka_run_group_tests_name("winjumpcore", tests, NULL, NULL);
}
