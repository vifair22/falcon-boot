/* Tests for falcon-core/src/blk/cache.c: the read cache of aligned windows
 * (docs/DESIGN.md Section 3.4).
 *
 * The device is memory that counts every call, so a case says both what
 * bytes came back and how many reads the firmware would have been asked
 * for, which is the cache's whole job. The window is 4 KiB here and the
 * device 10.5 windows long, so the last window is short.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <blk/cache.h>

#include "tests.h"

#define WIN       4096u
#define SLOTS     3u
#define DEV_LEN   (10u * WIN + WIN / 2u)

typedef struct {
    uint8_t  bytes[DEV_LEN];
    unsigned calls;
    unsigned fail_at;        /* 1-based call that fails; 0 never */
    uint64_t last_off;
    uint64_t last_len;
} mem_dev;

static mem_dev g_dev;
static mem_dev g_dev2;
static blk_cache g_c;
static blk_cache_slot g_slots[SLOTS];
static uint8_t g_mem[SLOTS * WIN];

static int dev_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    mem_dev *d = ctx;

    d->calls++;
    d->last_off = off;
    d->last_len = len;
    if (d->fail_at != 0u && d->calls == d->fail_at)
        return -1;
    if (off > DEV_LEN || len > DEV_LEN - off)
        return -1;
    memcpy(buf, d->bytes + off, (size_t)len);
    return 0;
}

static void fill(mem_dev *d, unsigned seed)
{
    size_t i;

    memset(d, 0, sizeof *d);
    for (i = 0; i < DEV_LEN; i++)
        d->bytes[i] = (uint8_t)((i * 131u + seed) ^ (i >> 9));
}

static int setup(void **state)
{
    (void)state;
    fill(&g_dev, 7u);
    fill(&g_dev2, 99u);
    blk_cache_init(&g_c, g_slots, SLOTS, g_mem, WIN);
    return 0;
}

static int rd(mem_dev *d, uint32_t gen, uint64_t off, uint64_t len,
              uint8_t *buf)
{
    return blk_cache_read(&g_c, d, gen, DEV_LEN, dev_read, d, off, len, buf);
}

static void assert_bytes(const mem_dev *d, uint64_t off, uint64_t len,
                         const uint8_t *buf)
{
    assert_memory_equal(buf, d->bytes + off, (size_t)len);
}

/* Many small reads in one window: one device call, which reads the whole
 * window. */
static void test_small_reads_share_a_window(void **state)
{
    uint8_t buf[600];

    (void)state;
    assert_int_equal(rd(&g_dev, 1, 100, 512, buf), 0);
    assert_bytes(&g_dev, 100, 512, buf);
    assert_int_equal(g_dev.calls, 1);
    assert_int_equal(g_dev.last_off, 0);
    assert_int_equal(g_dev.last_len, WIN);
    assert_int_equal(rd(&g_dev, 1, 3000, 600, buf), 0);
    assert_bytes(&g_dev, 3000, 600, buf);
    assert_int_equal(rd(&g_dev, 1, 0, 1, buf), 0);
    assert_bytes(&g_dev, 0, 1, buf);
    assert_int_equal(g_dev.calls, 1);
}

/* A small read across a window boundary is two windows, then free. */
static void test_read_across_windows(void **state)
{
    uint8_t buf[1000];

    (void)state;
    assert_int_equal(rd(&g_dev, 1, 2u * WIN - 300u, 1000, buf), 0);
    assert_bytes(&g_dev, 2u * WIN - 300u, 1000, buf);
    assert_int_equal(g_dev.calls, 2);
    assert_int_equal(rd(&g_dev, 1, 2u * WIN - 10u, 20, buf), 0);
    assert_int_equal(g_dev.calls, 2);
}

/* A window's worth or more goes straight through and leaves nothing. */
static void test_large_reads_pass_through(void **state)
{
    static uint8_t buf[2u * WIN];

    (void)state;
    assert_int_equal(rd(&g_dev, 1, 123, WIN, buf), 0);
    assert_bytes(&g_dev, 123, WIN, buf);
    assert_int_equal(g_dev.calls, 1);
    assert_int_equal(g_dev.last_off, 123);
    assert_int_equal(g_dev.last_len, WIN);
    assert_int_equal(rd(&g_dev, 1, 123, 10, buf), 0);
    assert_int_equal(g_dev.calls, 2);
}

/* The last window is cut at the device's end, and a read past the end is
 * the inner read's to refuse. */
static void test_device_end(void **state)
{
    uint8_t buf[64];

    (void)state;
    assert_int_equal(rd(&g_dev, 1, DEV_LEN - 64u, 64, buf), 0);
    assert_bytes(&g_dev, DEV_LEN - 64u, 64, buf);
    assert_int_equal(g_dev.last_off, 10u * WIN);
    assert_int_equal(g_dev.last_len, WIN / 2u);
    assert_int_equal(rd(&g_dev, 1, DEV_LEN - 32u, 64, buf), -1);
    assert_int_equal(g_dev.last_off, DEV_LEN - 32u);
    assert_int_equal(g_dev.last_len, 64);
}

/* Another device, or the same one under a new generation, is not the
 * window held; a flush forgets them all. */
static void test_keys_and_flush(void **state)
{
    uint8_t buf[16];

    (void)state;
    assert_int_equal(rd(&g_dev, 1, 10, 16, buf), 0);
    assert_int_equal(rd(&g_dev2, 1, 10, 16, buf), 0);
    assert_bytes(&g_dev2, 10, 16, buf);
    assert_int_equal(g_dev2.calls, 1);
    assert_int_equal(rd(&g_dev, 2, 10, 16, buf), 0);
    assert_int_equal(g_dev.calls, 2);
    assert_int_equal(rd(&g_dev, 2, 10, 16, buf), 0);
    assert_int_equal(g_dev.calls, 2);
    blk_cache_flush(&g_c);
    assert_int_equal(rd(&g_dev, 2, 10, 16, buf), 0);
    assert_int_equal(g_dev.calls, 3);
}

/* With every slot full, the least recently used window goes. */
static void test_eviction_is_lru(void **state)
{
    uint8_t buf[8];

    (void)state;
    assert_int_equal(rd(&g_dev, 1, 0u * WIN, 8, buf), 0);
    assert_int_equal(rd(&g_dev, 1, 1u * WIN, 8, buf), 0);
    assert_int_equal(rd(&g_dev, 1, 2u * WIN, 8, buf), 0);
    assert_int_equal(rd(&g_dev, 1, 0u * WIN, 8, buf), 0);   /* 0 is fresh */
    assert_int_equal(g_dev.calls, 3);
    assert_int_equal(rd(&g_dev, 1, 3u * WIN, 8, buf), 0);   /* evicts 1 */
    assert_int_equal(g_dev.calls, 4);
    assert_int_equal(rd(&g_dev, 1, 0u * WIN, 8, buf), 0);
    assert_int_equal(rd(&g_dev, 1, 2u * WIN, 8, buf), 0);
    assert_int_equal(g_dev.calls, 4);
    assert_int_equal(rd(&g_dev, 1, 1u * WIN, 8, buf), 0);
    assert_int_equal(g_dev.calls, 5);
    assert_bytes(&g_dev, 1u * WIN, 8, buf);
}

/* A window that fails to read is not kept: the next read asks again. */
static void test_failure_leaves_no_window(void **state)
{
    uint8_t buf[8];

    (void)state;
    g_dev.fail_at = 1;
    assert_int_equal(rd(&g_dev, 1, 5, 8, buf), -1);
    assert_int_equal(rd(&g_dev, 1, 5, 8, buf), 0);
    assert_bytes(&g_dev, 5, 8, buf);
    assert_int_equal(g_dev.calls, 2);
}

/* No slots, no memory, no device or nothing to read: straight through, or
 * nothing at all. */
static void test_degenerate(void **state)
{
    blk_cache none;
    uint8_t buf[8];

    (void)state;
    blk_cache_init(&none, g_slots, SLOTS, (uint8_t *)0, WIN);
    assert_int_equal(none.n, 0);
    assert_int_equal(blk_cache_read(&none, &g_dev, 1, DEV_LEN, dev_read,
                                    &g_dev, 5, 8, buf), 0);
    assert_int_equal(blk_cache_read(&none, &g_dev, 1, DEV_LEN, dev_read,
                                    &g_dev, 5, 8, buf), 0);
    assert_int_equal(g_dev.calls, 2);
    assert_int_equal(g_dev.last_len, 8);
    assert_int_equal(blk_cache_read((blk_cache *)0, &g_dev, 1, DEV_LEN,
                                    dev_read, &g_dev, 5, 8, buf), 0);
    assert_int_equal(blk_cache_read(&g_c, (const void *)0, 1, DEV_LEN,
                                    dev_read, &g_dev, 5, 8, buf), 0);
    assert_int_equal(g_dev.calls, 4);
    assert_int_equal(rd(&g_dev, 1, 5, 0, buf), 0);
    assert_int_equal(g_dev.calls, 4);
}

int run_blkcache_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_small_reads_share_a_window, setup),
        cmocka_unit_test_setup(test_read_across_windows, setup),
        cmocka_unit_test_setup(test_large_reads_pass_through, setup),
        cmocka_unit_test_setup(test_device_end, setup),
        cmocka_unit_test_setup(test_keys_and_flush, setup),
        cmocka_unit_test_setup(test_eviction_is_lru, setup),
        cmocka_unit_test_setup(test_failure_leaves_no_window, setup),
        cmocka_unit_test_setup(test_degenerate, setup),
    };

    return cmocka_run_group_tests_name("blkcache", tests, NULL, NULL);
}
