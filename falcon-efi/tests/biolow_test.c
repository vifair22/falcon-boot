/* Tests for falcon-efi/src/discover/biolow.c, Block I/O reads through memory
 * below 4 GiB (docs/DESIGN.md Section 3.1).
 *
 * The fake Block I/O refuses, with EFI_UNSUPPORTED, any transfer whose
 * buffer reaches above 4 GiB: what the AMI NVMe driver on an ASUS B550 does
 * when the firmware has loaded the station high, which left that machine
 * unable to read its own partition. The OVMF the rigs run never refuses, so
 * this suite is where that behaviour is held.
 *
 * The buffers are real addresses on either side of the line: mmap places the
 * high ones at a fixed address above 4 GiB and the low one with MAP_32BIT,
 * so the module's address test meets what it meets on the firmware.
 */

#define _GNU_SOURCE

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>
#include <sys/mman.h>

#include <discover/biolow.h>

#define BS        ((UINTN)512u)
#define LOW_LEN   ((UINTN)16u * 4096u)
#define HIGH_ADDR 0x200000000ull         /* 8 GiB */
#define EDGE_ADDR (0x100000000ull - 8192u) /* two pages under 4 GiB */
#define MAP_LEN   ((size_t)1024u * 1024u)

/* ---- the fake Block I/O ---- */

typedef struct {
    EFI_BLOCK_IO_PROTOCOL bio;
    EFI_BLOCK_IO_MEDIA media;
    unsigned calls;
    unsigned fail_at;        /* 1-based call to fail, 0 never */
    void *last_buf;
    UINTN max_size;          /* largest single transfer seen */
} fake_bio;

/* The byte at a given place on the fake disk. */
static UINT8 disk_byte(UINT64 lba, UINTN i)
{
    UINT64 off = lba * BS + i;

    return (UINT8)((off * 131u) ^ (off >> 9));
}

static EFI_STATUS EFIAPI fake_read(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
                                   EFI_LBA Lba, UINTN BufferSize, void *Buffer)
{
    fake_bio *f = (fake_bio *)This;
    UINT8 *p = Buffer;
    UINTN i;

    assert_int_equal(MediaId, f->media.MediaId);
    assert_int_equal(BufferSize % BS, 0);
    f->calls++;
    f->last_buf = Buffer;
    if (BufferSize > f->max_size)
        f->max_size = BufferSize;
    if (f->fail_at != 0u && f->calls == f->fail_at)
        return EFI_DEVICE_ERROR;
    if ((uint64_t)(uintptr_t)Buffer + BufferSize > BIOLOW_LIMIT)
        return EFI_UNSUPPORTED;
    for (i = 0; i < BufferSize; i++)
        p[i] = disk_byte(Lba, i);
    return EFI_SUCCESS;
}

static void fake_init(fake_bio *f)
{
    memset(f, 0, sizeof *f);
    f->media.MediaId = 7u;
    f->media.BlockSize = BS;
    f->bio.Media = &f->media;
    f->bio.ReadBlocks = fake_read;
}

/* ---- the memory either side of 4 GiB ---- */

static UINT8 *g_lowmem;
static unsigned g_low_calls;
static int g_low_none;

static UINT8 *low_fn(void)
{
    g_low_calls++;
    return g_low_none ? (UINT8 *)0 : g_lowmem;
}

static void *map_at(uint64_t addr)
{
    void *p = mmap((void *)(uintptr_t)addr, MAP_LEN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

    assert_true(p == (void *)(uintptr_t)addr);
    return p;
}

static int setup(void **state)
{
    (void)state;
    g_lowmem = mmap(NULL, LOW_LEN, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (g_lowmem == MAP_FAILED)
        return -1;
    g_low_calls = 0;
    g_low_none = 0;
    return 0;
}

static int teardown(void **state)
{
    (void)state;
    munmap(g_lowmem, LOW_LEN);
    return 0;
}

static void assert_disk(const UINT8 *buf, UINT64 lba, UINTN len)
{
    UINTN i;

    for (i = 0; i < len; i++)
        if (buf[i] != disk_byte(lba, i))
            fail_msg("byte %zu of a read at lba %llu is wrong", (size_t)i,
                     (unsigned long long)lba);
}

/* ---- the cases ---- */

/* The fake is the driver: a high buffer read directly is refused. */
static void test_fake_refuses_high(void **state)
{
    fake_bio f;
    UINT8 *high = map_at(HIGH_ADDR);

    (void)state;
    fake_init(&f);
    assert_int_equal(f.bio.ReadBlocks(&f.bio, 7u, 0, BS, high),
                     EFI_UNSUPPORTED);
    munmap(high, MAP_LEN);
}

/* A buffer below 4 GiB is read directly, and no low memory is asked for. */
static void test_low_buffer_direct(void **state)
{
    fake_bio f;
    UINT8 *buf = g_lowmem + 4096u;

    (void)state;
    fake_init(&f);
    assert_int_equal(biolow_read(&f.bio, 3, 8u * BS, buf, low_fn, LOW_LEN),
                     EFI_SUCCESS);
    assert_int_equal(f.calls, 1);
    assert_ptr_equal(f.last_buf, buf);
    assert_int_equal(g_low_calls, 0);
    assert_disk(buf, 3, 8u * BS);
}

/* The B550 case: a high buffer reads, through the low memory, in chunks of
 * at most its size, and lands intact. */
static void test_high_buffer_bounced(void **state)
{
    fake_bio f;
    UINT8 *high = map_at(HIGH_ADDR);
    UINTN len = 5u * LOW_LEN / 2u;      /* two and a half chunks */

    (void)state;
    fake_init(&f);
    assert_int_equal(biolow_read(&f.bio, 100, len, high, low_fn, LOW_LEN),
                     EFI_SUCCESS);
    assert_int_equal(f.calls, 3);
    assert_ptr_equal(f.last_buf, g_lowmem);
    assert_int_equal(f.max_size, LOW_LEN);
    assert_disk(high, 100, len);
    munmap(high, MAP_LEN);
}

/* A buffer that starts below 4 GiB and runs over it is bounced too. */
static void test_buffer_across_the_line(void **state)
{
    fake_bio f;
    UINT8 *edge = map_at(EDGE_ADDR);

    (void)state;
    fake_init(&f);
    assert_int_equal(biolow_read(&f.bio, 9, 32u * BS, edge, low_fn, LOW_LEN),
                     EFI_SUCCESS);
    assert_ptr_equal(f.last_buf, g_lowmem);
    assert_disk(edge, 9, 32u * BS);
    /* Ending exactly on the line is still below it. */
    fake_init(&f);
    assert_int_equal(biolow_read(&f.bio, 9, 16u * BS, edge, low_fn, LOW_LEN),
                     EFI_SUCCESS);
    assert_ptr_equal(f.last_buf, edge);
    munmap(edge, MAP_LEN);
}

/* Low memory that is not a whole number of blocks is used in whole blocks. */
static void test_chunk_rounds_to_blocks(void **state)
{
    fake_bio f;
    UINT8 *high = map_at(HIGH_ADDR);

    (void)state;
    fake_init(&f);
    assert_int_equal(biolow_read(&f.bio, 0, 10u * BS, high, low_fn,
                                 3u * BS + 100u),
                     EFI_SUCCESS);
    assert_int_equal(f.max_size, 3u * BS);
    assert_int_equal(f.calls, 4);
    assert_disk(high, 0, 10u * BS);
    munmap(high, MAP_LEN);
}

/* No low memory, or less than a block of it: the read goes direct and the
 * firmware's refusal is what the caller sees. */
static void test_no_low_memory(void **state)
{
    fake_bio f;
    UINT8 *high = map_at(HIGH_ADDR);

    (void)state;
    fake_init(&f);
    g_low_none = 1;
    assert_int_equal(biolow_read(&f.bio, 0, BS, high, low_fn, LOW_LEN),
                     EFI_UNSUPPORTED);
    assert_ptr_equal(f.last_buf, high);
    g_low_none = 0;
    fake_init(&f);
    assert_int_equal(biolow_read(&f.bio, 0, BS, high, low_fn, BS - 1u),
                     EFI_UNSUPPORTED);
    assert_ptr_equal(f.last_buf, high);
    munmap(high, MAP_LEN);
}

/* A chunk that fails stops the read with its status. */
static void test_error_mid_read(void **state)
{
    fake_bio f;
    UINT8 *high = map_at(HIGH_ADDR);

    (void)state;
    fake_init(&f);
    f.fail_at = 2u;
    assert_int_equal(biolow_read(&f.bio, 0, 3u * LOW_LEN, high, low_fn,
                                 LOW_LEN),
                     EFI_DEVICE_ERROR);
    assert_int_equal(f.calls, 2);
    assert_disk(high, 0, LOW_LEN);
    munmap(high, MAP_LEN);
}

/* FalconReadInfo's count: every firmware call, failed ones too, with its
 * bytes and the clock's ticks across it. The clock here moves 35 ticks a
 * reading, so each call costs 35. The count is the module's for the whole
 * run, so the case works in differences. */
static UINT64 g_ticks;

static UINT64 fake_clock(void)
{
    g_ticks += 35u;
    return g_ticks;
}

static void test_counted(void **state)
{
    fake_bio f;
    UINT8 *high = map_at(HIGH_ADDR);
    biolow_count a, b;

    (void)state;
    fake_init(&f);
    biolow_set_clock(fake_clock);

    biolow_counted(&a);
    assert_int_equal(biolow_read(&f.bio, 0, 8u * BS, g_lowmem, low_fn,
                                 LOW_LEN), EFI_SUCCESS);
    biolow_counted(&b);
    assert_int_equal(b.calls - a.calls, 1);
    assert_int_equal(b.bytes - a.bytes, 8u * BS);
    assert_int_equal(b.ticks - a.ticks, 35);

    /* Bounced: each chunk is a firmware call of its own. */
    a = b;
    assert_int_equal(biolow_read(&f.bio, 0, 3u * LOW_LEN, high, low_fn,
                                 LOW_LEN), EFI_SUCCESS);
    biolow_counted(&b);
    assert_int_equal(b.calls - a.calls, 3);
    assert_int_equal(b.bytes - a.bytes, 3u * LOW_LEN);
    assert_int_equal(b.ticks - a.ticks, 3u * 35u);

    /* A call the firmware fails still cost what it cost. */
    a = b;
    f.fail_at = f.calls + 1u;
    assert_int_equal(biolow_read(&f.bio, 0, BS, g_lowmem, low_fn, LOW_LEN),
                     EFI_DEVICE_ERROR);
    biolow_counted(&b);
    assert_int_equal(b.calls - a.calls, 1);
    assert_int_equal(b.bytes - a.bytes, BS);

    /* Without a clock the calls and bytes are still counted. */
    biolow_set_clock((UINT64 (*)(void))0);
    a = b;
    f.fail_at = 0;
    assert_int_equal(biolow_read(&f.bio, 0, BS, g_lowmem, low_fn, LOW_LEN),
                     EFI_SUCCESS);
    biolow_counted(&b);
    assert_int_equal(b.calls - a.calls, 1);
    assert_int_equal(b.ticks, a.ticks);
    munmap(high, MAP_LEN);
}

int run_biolow_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_fake_refuses_high),
        cmocka_unit_test(test_low_buffer_direct),
        cmocka_unit_test(test_high_buffer_bounced),
        cmocka_unit_test(test_buffer_across_the_line),
        cmocka_unit_test(test_chunk_rounds_to_blocks),
        cmocka_unit_test(test_no_low_memory),
        cmocka_unit_test(test_error_mid_read),
        cmocka_unit_test(test_counted),
    };

    return cmocka_run_group_tests_name("biolow", tests, setup, teardown);
}
