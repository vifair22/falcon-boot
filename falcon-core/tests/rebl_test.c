/* Tests for falcon-core/src/blk/rebl.c: the re-blocking layer under the read
 * callback (docs/DESIGN.md Section 3.4).
 *
 * The inner device here is a memory disk that behaves as firmware does
 * (whole sectors at sector-aligned offsets, anything else refused) and
 * records every call, so each test can say both what bytes came back and
 * which device requests produced them. The sweep checks every head, middle
 * and tail shape against the disk's bytes at three sector sizes, one of them
 * not a power of two.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <blk/rebl.h>

#include "kndev.h"
#include "tests.h"

#define DISK_SECTORS 8u
#define MAX_SECTOR   4096u
#define CALLS_MAX    8u

typedef struct {
    uint64_t off;
    uint64_t len;
    void    *buf;
} call_rec;

typedef struct {
    uint8_t  bytes[DISK_SECTORS * MAX_SECTOR];
    uint32_t sector;
    unsigned calls;
    unsigned refused;
    unsigned fail_at;        /* 1-based call that fails; 0 never */
    call_rec rec[CALLS_MAX];
} mem_disk;

static mem_disk g_disk;
static uint8_t  g_bounce[MAX_SECTOR];

static int mem_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    mem_disk *d = ctx;

    if (len == 0 || off % d->sector != 0 || len % d->sector != 0 ||
        off + len > (uint64_t)DISK_SECTORS * d->sector) {
        d->refused++;
        return -1;
    }
    if (d->calls < CALLS_MAX) {
        d->rec[d->calls].off = off;
        d->rec[d->calls].len = len;
        d->rec[d->calls].buf = buf;
    }
    d->calls++;
    if (d->fail_at != 0 && d->calls == d->fail_at)
        return -1;
    memcpy(buf, d->bytes + off, (size_t)len);
    return 0;
}

static void disk_setup(uint32_t sector)
{
    size_t i;

    memset(&g_disk, 0, sizeof g_disk);
    g_disk.sector = sector;
    /* Not a counter: a byte from one sector too far must not equal the
     * byte that was wanted. */
    for (i = 0; i < sizeof g_disk.bytes; i++)
        g_disk.bytes[i] = (uint8_t)((i * 131u) ^ (i >> 9) ^ 0x5Au);
}

static blk_rebl rebl_for(uint32_t sector)
{
    blk_rebl r;

    disk_setup(sector);
    r.read = mem_read;
    r.ctx = &g_disk;
    r.sector_size = sector;
    r.bounce = g_bounce;
    r.bounce_len = sizeof g_bounce;
    return r;
}

/* Reads [off, off+len) and compares it, with a guard byte either side of
 * the caller's buffer that must survive. */
static void check_span(blk_rebl *r, uint64_t off, uint64_t len)
{
    static uint8_t out[DISK_SECTORS * MAX_SECTOR + 2u];

    memset(out, 0xEE, sizeof out);
    g_disk.calls = 0;
    assert_int_equal(blk_rebl_read(r, off, len, out + 1), 0);
    assert_memory_equal(out + 1, g_disk.bytes + off, (size_t)len);
    assert_int_equal(out[0], 0xEE);
    assert_int_equal(out[len + 1u], 0xEE);
    assert_int_equal(g_disk.refused, 0);
    /* A head, a middle and a tail at most. */
    assert_true(g_disk.calls <= 3u);
}

/* ---- the shapes ---- */

static void test_aligned_passes_through(void **state)
{
    uint8_t out[3u * 512u];
    blk_rebl r = rebl_for(512u);

    (void)state;
    assert_int_equal(blk_rebl_read(&r, 1024u, sizeof out, out), 0);
    /* One call, the caller's own arguments and buffer: no bounce, no copy. */
    assert_int_equal(g_disk.calls, 1);
    assert_int_equal(g_disk.rec[0].off, 1024u);
    assert_int_equal(g_disk.rec[0].len, sizeof out);
    assert_ptr_equal(g_disk.rec[0].buf, out);
    assert_memory_equal(out, g_disk.bytes + 1024u, sizeof out);
}

static void test_zero_length(void **state)
{
    blk_rebl r = rebl_for(4096u);

    (void)state;
    /* No inner call, and no buffer needed. */
    assert_int_equal(blk_rebl_read(&r, 123u, 0, (void *)0), 0);
    assert_int_equal(g_disk.calls, 0);
}

static void test_inside_one_sector(void **state)
{
    uint8_t out[10];
    blk_rebl r = rebl_for(4096u);

    (void)state;
    assert_int_equal(blk_rebl_read(&r, 4096u + 100u, sizeof out, out), 0);
    assert_int_equal(g_disk.calls, 1);
    assert_int_equal(g_disk.rec[0].off, 4096u);
    assert_int_equal(g_disk.rec[0].len, 4096u);
    assert_ptr_equal(g_disk.rec[0].buf, g_bounce);
    assert_memory_equal(out, g_disk.bytes + 4096u + 100u, sizeof out);
}

static void test_short_at_sector_start(void **state)
{
    uint8_t out[512];
    blk_rebl r = rebl_for(4096u);

    (void)state;
    /* The FAT and exFAT boot-sector read: 512 bytes at 0. */
    assert_int_equal(blk_rebl_read(&r, 0, sizeof out, out), 0);
    assert_int_equal(g_disk.calls, 1);
    assert_int_equal(g_disk.rec[0].off, 0);
    assert_memory_equal(out, g_disk.bytes, sizeof out);
}

static void test_ext4_superblock(void **state)
{
    uint8_t out[1024];
    blk_rebl r = rebl_for(4096u);

    (void)state;
    assert_int_equal(blk_rebl_read(&r, 1024u, sizeof out, out), 0);
    assert_int_equal(g_disk.calls, 1);
    assert_memory_equal(out, g_disk.bytes + 1024u, sizeof out);
}

static void test_straddles_two_sectors(void **state)
{
    uint8_t out[2];
    blk_rebl r = rebl_for(4096u);

    (void)state;
    assert_int_equal(blk_rebl_read(&r, 4095u, sizeof out, out), 0);
    assert_int_equal(g_disk.calls, 2);
    assert_int_equal(g_disk.rec[0].off, 0);
    assert_int_equal(g_disk.rec[1].off, 4096u);
    assert_memory_equal(out, g_disk.bytes + 4095u, sizeof out);
}

static void test_head_middle_tail(void **state)
{
    static uint8_t out[3u * 4096u];
    blk_rebl r = rebl_for(4096u);

    (void)state;
    /* 96 bytes of sector 0, sectors 1 and 2 whole, 4000 of sector 3. */
    assert_int_equal(blk_rebl_read(&r, 4000u, 96u + 8192u + 4000u, out), 0);
    assert_int_equal(g_disk.calls, 3);
    assert_int_equal(g_disk.rec[0].off, 0);
    assert_ptr_equal(g_disk.rec[0].buf, g_bounce);
    /* The middle lands in the caller's buffer directly, just past the head. */
    assert_int_equal(g_disk.rec[1].off, 4096u);
    assert_int_equal(g_disk.rec[1].len, 8192u);
    assert_ptr_equal(g_disk.rec[1].buf, out + 96);
    assert_int_equal(g_disk.rec[2].off, 12288u);
    assert_ptr_equal(g_disk.rec[2].buf, g_bounce);
    assert_memory_equal(out, g_disk.bytes + 4000u, 96u + 8192u + 4000u);
}

static void test_aligned_start_ragged_end(void **state)
{
    static uint8_t out[4096u + 10u];
    blk_rebl r = rebl_for(4096u);

    (void)state;
    assert_int_equal(blk_rebl_read(&r, 8192u, sizeof out, out), 0);
    assert_int_equal(g_disk.calls, 2);
    assert_ptr_equal(g_disk.rec[0].buf, out);
    assert_ptr_equal(g_disk.rec[1].buf, g_bounce);
    assert_memory_equal(out, g_disk.bytes + 8192u, sizeof out);
}

/* Every head offset against every length over three sectors' worth, at
 * three sector sizes: 512 (the ordinary disk, where the drivers' reads are
 * mostly already whole), 4096, and 520, which is no power of two. */
static void test_sweep(void **state)
{
    static const uint32_t sizes[] = { 512u, 520u, 4096u };
    size_t s;

    (void)state;
    for (s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        uint32_t ss = sizes[s];
        blk_rebl r = rebl_for(ss);
        uint64_t step = ss / 8u + 1u;
        uint64_t off, len;

        for (off = 0; off < 2u * ss; off += step)
            for (len = 1; off + len <= 5u * ss && len <= 3u * ss + 1u;
                 len += step)
                check_span(&r, off, len);
        /* The exact edges: one byte, one sector, sector plus and minus one. */
        check_span(&r, ss - 1u, 1u);
        check_span(&r, ss, ss);
        check_span(&r, ss - 1u, ss + 2u);
        check_span(&r, 1u, ss - 1u);
        check_span(&r, 0, (uint64_t)DISK_SECTORS * ss);
    }
}

/* ---- failures ---- */

static void test_inner_failure_each_part(void **state)
{
    static uint8_t out[3u * 4096u];
    unsigned at;

    (void)state;
    /* The head, the middle, the tail: each failure is the caller's. */
    for (at = 1; at <= 3u; at++) {
        blk_rebl r = rebl_for(4096u);

        g_disk.fail_at = at;
        assert_int_not_equal(blk_rebl_read(&r, 4000u, 96u + 8192u + 4000u,
                                           out), 0);
        /* No retry, and nothing read after the failure. */
        assert_int_equal(g_disk.calls, at);
    }
}

static void test_bad_arguments(void **state)
{
    uint8_t out[16];
    blk_rebl r = rebl_for(4096u);

    (void)state;
    assert_int_not_equal(blk_rebl_read((void *)0, 0, 16u, out), 0);
    assert_int_not_equal(blk_rebl_read(&r, 0, 16u, (void *)0), 0);
    /* A range that wraps past 2^64. */
    assert_int_not_equal(blk_rebl_read(&r, ~(uint64_t)0 - 3u, 16u, out), 0);

    r.sector_size = 0;
    assert_int_not_equal(blk_rebl_read(&r, 0, 16u, out), 0);
    r.sector_size = 4096u;
    r.read = (blk_read_fn)0;
    assert_int_not_equal(blk_rebl_read(&r, 0, 16u, out), 0);
    assert_int_equal(g_disk.calls, 0);
}

static void test_bounce_too_small(void **state)
{
    static uint8_t out[4096];
    blk_rebl r = rebl_for(4096u);

    (void)state;
    /* A bounce shorter than the sector still serves whole sectors... */
    r.bounce_len = 512u;
    assert_int_equal(blk_rebl_read(&r, 4096u, sizeof out, out), 0);
    assert_memory_equal(out, g_disk.bytes + 4096u, sizeof out);
    /* ...and refuses a ragged head or tail without touching the device. */
    g_disk.calls = 0;
    assert_int_not_equal(blk_rebl_read(&r, 0, 512u, out), 0);
    assert_int_not_equal(blk_rebl_read(&r, 4096u, 4096u + 1u, out), 0);
    assert_int_equal(g_disk.calls, 1);   /* the second one's middle */
    r.bounce = (uint8_t *)0;
    r.bounce_len = sizeof g_bounce;
    assert_int_not_equal(blk_rebl_read(&r, 1u, 1u, out), 0);
}

/* ---- the test device the driver suites use ---- */

static void test_kn_strict_refuses(void **state)
{
    kn_dev k;
    uint8_t out[512];

    (void)state;
    disk_setup(4096u);
    kn_init(&k, mem_read, &g_disk);
    /* The strict device alone is a 4Kn disk: 512 bytes is refused. */
    assert_int_not_equal(kn_strict_read(&k.strict, 0, sizeof out, out), 0);
    assert_int_equal(k.strict.refused, 1);
    assert_int_not_equal(kn_strict_read(&k.strict, 0, 0, out), 0);
    /* Through the layer it is served, and the device sees one sector. */
    assert_int_equal(blk_rebl_read(&k.rebl, 512u, sizeof out, out), 0);
    assert_memory_equal(out, g_disk.bytes + 512u, sizeof out);
    assert_int_equal(k.strict.calls, 1);
    assert_int_equal(k.strict.refused, 2);
}

int run_rebl_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_aligned_passes_through),
        cmocka_unit_test(test_zero_length),
        cmocka_unit_test(test_inside_one_sector),
        cmocka_unit_test(test_short_at_sector_start),
        cmocka_unit_test(test_ext4_superblock),
        cmocka_unit_test(test_straddles_two_sectors),
        cmocka_unit_test(test_head_middle_tail),
        cmocka_unit_test(test_aligned_start_ragged_end),
        cmocka_unit_test(test_sweep),
        cmocka_unit_test(test_inner_failure_each_part),
        cmocka_unit_test(test_bad_arguments),
        cmocka_unit_test(test_bounce_too_small),
        cmocka_unit_test(test_kn_strict_refuses),
    };

    return cmocka_run_group_tests_name("blk rebl", tests, NULL, NULL);
}
