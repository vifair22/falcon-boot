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

/* blk_cache_next_win: a full window inside the budget stays, a slow fill
 * shrinks to the power of two that fits, a fast small window grows back, and
 * both ends hold. */
static void test_next_win(void **state)
{
    (void)state;
    /* A per-call firmware: 64 KiB in 70 of a 100 budget. */
    assert_int_equal(blk_cache_next_win(0x10000u, 0x10000u, 70, 100),
                     0x10000u);
    /* 64 KiB a second: a 64 KiB fill takes ten budgets, 6.4 KiB fits. */
    assert_int_equal(blk_cache_next_win(0x10000u, 0x10000u, 1000, 100),
                     0x1000u);
    /* Floor: nothing goes under 4 KiB however slow. */
    assert_int_equal(blk_cache_next_win(0x10000u, 0x1000u, 1000000, 100),
                     BLK_CACHE_MIN_WIN);
    /* Between: 64 KiB in 200 gives 32 KiB; 16 KiB in 102 gives 8 KiB. */
    assert_int_equal(blk_cache_next_win(0x10000u, 0x10000u, 200, 100),
                     0x8000u);
    assert_int_equal(blk_cache_next_win(0x10000u, 0x4000u, 102, 100),
                     0x2000u);
    /* A fast small window grows, and stops at max. */
    assert_int_equal(blk_cache_next_win(0x10000u, 0x1000u, 5, 100),
                     0x10000u);
    /* No time measured, or no budget: max. */
    assert_int_equal(blk_cache_next_win(0x10000u, 0x1000u, 0, 100),
                     0x10000u);
    assert_int_equal(blk_cache_next_win(0x10000u, 0x1000u, 5, 0), 0x10000u);
    /* A max at or under the floor is the window. */
    assert_int_equal(blk_cache_next_win(0x800u, 0x800u, 1000, 1), 0x800u);
    /* A budget past 32 bits does not overflow. */
    assert_int_equal(blk_cache_next_win(0x10000u, 0x10000u, 1u,
                                        0x10000000000ull), 0x10000u);
}

/* A device whose reads cost a fixed time per call plus a time per KiB, on a
 * clock the test owns: what the cache learns a window from. */
#define BIG_WIN  0x10000u
#define BIG_LEN  (16u * BIG_WIN)

typedef struct {
    uint8_t  bytes[BIG_LEN];
    unsigned calls;
    uint64_t last_len;
    uint64_t per_call;
    uint64_t per_kib;
} timed_dev;

static timed_dev g_tdev;
static uint64_t g_now;
static blk_cache_slot g_big_slots[2];
static uint8_t g_big_mem[2u * BIG_WIN];
/* Only their addresses are used: each one is a device to the cache. */
static uint8_t g_keys[BLK_CACHE_DEVS + 1u];

static uint64_t now(void)
{
    return g_now;
}

static int timed_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    timed_dev *d = ctx;

    d->calls++;
    d->last_len = len;
    g_now += d->per_call + len / 1024u * d->per_kib;
    if (off > BIG_LEN || len > BIG_LEN - off)
        return -1;
    memcpy(buf, d->bytes + off, (size_t)len);
    return 0;
}

static void timed_setup(blk_cache *c, uint64_t per_call, uint64_t per_kib)
{
    size_t i;

    memset(&g_tdev, 0, sizeof g_tdev);
    for (i = 0; i < BIG_LEN; i++)
        g_tdev.bytes[i] = (uint8_t)(i * 7u + 3u);
    g_tdev.per_call = per_call;
    g_tdev.per_kib = per_kib;
    g_now = 0;
    blk_cache_init(c, g_big_slots, 2u, g_big_mem, BIG_WIN);
    /* A budget of 100 ticks: a tick is a millisecond here. */
    blk_cache_set_clock(c, now, 100u);
}

static int tread(blk_cache *c, const void *dev, uint32_t gen, uint64_t off,
                 uint64_t len, void *buf)
{
    return blk_cache_read(c, dev, gen, BIG_LEN, timed_read, &g_tdev, off,
                          len, buf);
}

static void assert_timed(uint64_t off, const uint8_t *buf, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        assert_int_equal(buf[i], (uint8_t)((off + i) * 7u + 3u));
}

/* A slow medium (64 KiB a second, 16 ms a KiB): the first fill is the whole
 * window and takes a second; after it each fill is 4 KiB, inside the
 * budget, and the bytes are right whichever window holds them. */
static void test_slow_medium_shrinks(void **state)
{
    blk_cache c;
    uint8_t buf[512];
    uint8_t big[BLK_CACHE_MIN_WIN];
    uint64_t t;

    (void)state;
    timed_setup(&c, 0u, 16u);
    assert_int_equal(tread(&c, &g_tdev, 1, 0x30064u, 512, buf), 0);
    assert_timed(0x30064u, buf, 512);
    assert_int_equal(g_tdev.last_len, BIG_WIN);
    /* Inside the first window: a hit, no call. */
    assert_int_equal(tread(&c, &g_tdev, 1, 0x38000u, 512, buf), 0);
    assert_timed(0x38000u, buf, 512);
    assert_int_equal(g_tdev.calls, 1);
    /* Elsewhere: a 4 KiB fill, inside the budget. */
    t = g_now;
    assert_int_equal(tread(&c, &g_tdev, 1, 0x81234u, 512, buf), 0);
    assert_timed(0x81234u, buf, 512);
    assert_int_equal(g_tdev.calls, 2);
    assert_int_equal(g_tdev.last_len, BLK_CACHE_MIN_WIN);
    assert_true(g_now - t <= 100u);
    /* A read the size of the device's window goes straight through. */
    assert_int_equal(tread(&c, &g_tdev, 1, 0x90000u, sizeof big, big), 0);
    assert_int_equal(g_tdev.calls, 3);
    assert_int_equal(g_tdev.last_len, sizeof big);
    assert_timed(0x90000u, big, sizeof big);
    /* A flush forgets the size: the next fill measures again. */
    blk_cache_flush(&c);
    assert_int_equal(tread(&c, &g_tdev, 1, 0x20000u, 512, buf), 0);
    assert_int_equal(g_tdev.last_len, BIG_WIN);
}

/* A firmware that charges per call (70 ms whatever the size) keeps the
 * whole window, which is what the cache is for. */
static void test_per_call_medium_keeps_window(void **state)
{
    blk_cache c;
    uint8_t buf[512];
    unsigned i;

    (void)state;
    timed_setup(&c, 70u, 0u);
    for (i = 0; i < 4u; i++) {
        assert_int_equal(tread(&c, &g_tdev, 1, (uint64_t)i * 0x20000u + 9u,
                               512, buf), 0);
        assert_int_equal(g_tdev.last_len, BIG_WIN);
    }
    assert_int_equal(g_tdev.calls, 4);
}

/* Each device learns its own window, and a device the table forgot starts
 * again at the cache's. */
static void test_window_per_device(void **state)
{
    blk_cache c;
    uint8_t buf[512];
    unsigned i;

    (void)state;
    timed_setup(&c, 0u, 16u);
    assert_int_equal(tread(&c, &g_tdev, 1, 0, 512, buf), 0);
    assert_int_equal(tread(&c, &g_tdev, 1, 0x40000u, 512, buf), 0);
    assert_int_equal(g_tdev.last_len, BLK_CACHE_MIN_WIN);
    /* Another device that costs nothing reads full windows. */
    g_tdev.per_kib = 0;
    assert_int_equal(tread(&c, &g_keys[0], 1, 0x50000u, 512, buf), 0);
    assert_int_equal(g_tdev.last_len, BIG_WIN);
    /* Another generation of the slow device is another medium. */
    assert_int_equal(tread(&c, &g_tdev, 2, 0x60000u, 512, buf), 0);
    assert_int_equal(g_tdev.last_len, BIG_WIN);
    /* Enough other devices push the slow one out of the table. */
    for (i = 1; i <= BLK_CACHE_DEVS; i++)
        assert_int_equal(tread(&c, &g_keys[i], 1, 0x70000u, 512, buf), 0);
    assert_int_equal(tread(&c, &g_tdev, 1, 0xA0000u, 512, buf), 0);
    assert_int_equal(g_tdev.last_len, BIG_WIN);
}

/* Without a clock, or with no budget, every window is the cache's. */
static void test_no_clock_is_fixed(void **state)
{
    blk_cache c;
    uint8_t buf[512];

    (void)state;
    timed_setup(&c, 0u, 16u);
    blk_cache_set_clock(&c, (uint64_t (*)(void))0, 100u);
    assert_int_equal(tread(&c, &g_tdev, 1, 0, 512, buf), 0);
    assert_int_equal(tread(&c, &g_tdev, 1, 0x40000u, 512, buf), 0);
    assert_int_equal(g_tdev.last_len, BIG_WIN);
    blk_cache_set_clock(&c, now, 0u);
    assert_int_equal(tread(&c, &g_tdev, 1, 0x80000u, 512, buf), 0);
    assert_int_equal(g_tdev.last_len, BIG_WIN);
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
        cmocka_unit_test(test_next_win),
        cmocka_unit_test(test_slow_medium_shrinks),
        cmocka_unit_test(test_per_call_medium_keeps_window),
        cmocka_unit_test(test_window_per_device),
        cmocka_unit_test(test_no_clock_is_fixed),
    };

    return cmocka_run_group_tests_name("blkcache", tests, NULL, NULL);
}
