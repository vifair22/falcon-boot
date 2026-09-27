/* Tests for falcon-cli/src/mkstick/nextio.c: the next-boot marker sector as
 * `next-boot` finds and writes it, over image files written here.
 *
 * Each image is an MBR disk of 512- or 4096-byte sectors with a FAT32
 * partition carrying a `boot` record (marker at sector 8, as mkstick create
 * writes it), so the target is found by the same records_read and
 * records_boot the command uses. Two such images at once are the --disk
 * case: naming one must reach that one and never the other.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ident/ident.h>
#include <media/nextboot.h>
#include <mkstick/nextcore.h>
#include <mkstick/nextio.h>
#include <mkstick/records.h>

#define IMG_BYTES   (3u * 1024u * 1024u)
#define P1_BYTES    (1024u * 1024u)       /* p1 starts at 1 MiB */
#define P1_LEN      (2u * 1024u * 1024u)
#define RESERVED    40u
#define MARKER      8u
#define STAMP       1790000000u

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* An MBR disk of `ss`-byte sectors, p1 a FAT32 volume of the same sector
 * size as far as its boot sectors go, with a boot record whose install ID
 * is all `fill` when `fill` is nonzero. The path goes in `path`. */
static void make_disk(char *path, size_t cap, uint32_t ss, uint8_t fill)
{
    uint8_t sec[512];
    uint8_t *e = sec + 446;
    ident_rec rec;
    int fd;

    (void)snprintf(path, cap, "/tmp/falcon-nextio-test.XXXXXX");
    fd = mkstemp(path);
    assert_true(fd >= 0);
    assert_int_equal(ftruncate(fd, (off_t)IMG_BYTES), 0);

    memset(sec, 0, sizeof sec);
    e[4] = 0x0C;
    put_le32(e + 8, P1_BYTES / ss);
    put_le32(e + 12, P1_LEN / ss);
    sec[440] = fill;
    sec[510] = 0x55;
    sec[511] = 0xAA;
    assert_int_equal(pwrite(fd, sec, sizeof sec, 0), 512);

    memset(sec, 0, sizeof sec);
    sec[0] = 0xEB;
    sec[1] = 0x58;
    sec[2] = 0x90;
    sec[0x0B] = (uint8_t)(ss & 0xFFu);
    sec[0x0C] = (uint8_t)(ss >> 8);
    sec[0x0D] = 1;
    sec[0x0E] = RESERVED;
    sec[0x10] = 2;
    sec[0x30] = 1;
    sec[0x32] = 6;
    sec[510] = 0x55;
    sec[511] = 0xAA;
    assert_int_equal(pwrite(fd, sec, sizeof sec, (off_t)P1_BYTES), 512);
    assert_int_equal(pwrite(fd, sec, sizeof sec, (off_t)(P1_BYTES + 6u * ss)),
                     512);
    (void)close(fd);

    if (fill == 0u)
        return;
    memset(&rec, 0, sizeof rec);
    rec.version = IDENT_VERSION;
    rec.role = IDENT_ROLE_BOOT;
    rec.stage3_sector = 32;
    rec.marker_sector = MARKER;
    memset(rec.install_id, fill, sizeof rec.install_id);
    assert_int_equal(records_write_boot(path, P1_BYTES, &rec), CLI_OK);
}

static void read_at(const char *path, uint64_t off, void *buf, size_t len)
{
    int fd = open(path, O_RDONLY);

    assert_true(fd >= 0);
    assert_int_equal(pread(fd, buf, len, (off_t)off), (ssize_t)len);
    (void)close(fd);
}

static void write_at(const char *path, uint64_t off, const void *buf,
                     size_t len)
{
    int fd = open(path, O_RDWR);

    assert_true(fd >= 0);
    assert_int_equal(pwrite(fd, buf, len, (off_t)off), (ssize_t)len);
    (void)close(fd);
}

static int all_zero(const uint8_t *p, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (p[i] != 0u)
            return 0;
    }
    return 1;
}

/* A 4Kn disk: the marker is the whole 4096-byte sector 8 of p1, counted in
 * 4096-byte sectors, and nothing either side of it is touched. */
static void test_marker_4k(void **state)
{
    static uint8_t sec[NEXTBOOT_SECTOR_MAX];
    static uint8_t want[NEXTBOOT_SECTOR_MAX];
    static uint8_t side[NEXTBOOT_SECTOR_MAX];
    char path[64];
    next_target t;
    next_queued q;
    uint64_t moff;

    (void)state;
    make_disk(path, sizeof path, 4096u, 0x4B);
    assert_true(nextio_probe(path, 4096u, IMG_BYTES, &t));
    assert_int_equal(t.sector, 4096u);
    assert_int_equal(t.marker_lba, P1_BYTES / 4096u + MARKER);
    assert_string_equal(t.path, path);
    moff = t.marker_lba * 4096u;

    /* Read as a 512-byte disk the table points elsewhere: no boot
     * partition, so no marker. */
    assert_false(nextio_probe(path, 512u, IMG_BYTES, &t));
    assert_false(nextio_probe(path, 1000u, IMG_BYTES, &t));
    assert_true(nextio_probe(path, 4096u, IMG_BYTES, &t));

    /* The reserved sectors either side, so a spill would show. */
    memset(side, 0x5A, sizeof side);
    write_at(path, moff - 4096u, side, sizeof side);
    write_at(path, moff + 4096u, side, sizeof side);

    assert_int_equal(nextio_read(&t, &q), CLI_OK);
    assert_int_equal(q.state, NEXTBOOT_SECTOR_NONE);

    assert_int_equal(nextio_write(&t, NEXTBOOT_PERSONAL, STAMP), CLI_OK);
    read_at(path, moff, sec, sizeof sec);
    assert_int_equal(nextboot_sector_write(want, sizeof want, STAMP,
                                           NEXTBOOT_PERSONAL), NEXTBOOT_OK);
    assert_memory_equal(sec, want, sizeof sec);
    assert_true(all_zero(sec + 512, sizeof sec - 512u));
    read_at(path, moff - 4096u, sec, sizeof sec);
    assert_memory_equal(sec, side, sizeof sec);
    read_at(path, moff + 4096u, sec, sizeof sec);
    assert_memory_equal(sec, side, sizeof sec);

    assert_int_equal(nextio_read(&t, &q), CLI_OK);
    assert_int_equal(q.state, NEXTBOOT_SECTOR_VALUE);
    assert_int_equal(q.stamp, STAMP);
    assert_string_equal(q.value, NEXTBOOT_PERSONAL);

    /* Overwriting a marker is allowed; the new one replaces it whole. */
    assert_int_equal(nextio_write(&t, "iso/arch.iso", STAMP + 5u), CLI_OK);
    assert_int_equal(nextio_read(&t, &q), CLI_OK);
    assert_string_equal(q.value, "iso/arch.iso");
    assert_int_equal(q.stamp, STAMP + 5u);

    /* Clearing zeros the whole sector. */
    assert_int_equal(nextio_write(&t, NULL, 0), CLI_OK);
    read_at(path, moff, sec, sizeof sec);
    assert_true(all_zero(sec, sizeof sec));

    /* A byte past the first 512, which a 512-byte writer would never have
     * seen: the sector is not empty and not a marker, so it is left alone. */
    memset(sec, 0, sizeof sec);
    sec[4000] = 0x77;
    write_at(path, moff, sec, sizeof sec);
    assert_int_not_equal(nextio_write(&t, NEXTBOOT_PERSONAL, STAMP), CLI_OK);
    assert_int_not_equal(nextio_write(&t, NULL, 0), CLI_OK);
    read_at(path, moff, want, sizeof want);
    assert_memory_equal(sec, want, sizeof sec);
    assert_int_equal(nextio_read(&t, &q), CLI_OK);
    assert_int_equal(q.state, NEXTBOOT_SECTOR_NONE);

    /* A marker with the same stray byte is one the loader clears and does
     * not act on. */
    assert_int_equal(nextboot_sector_write(sec, sizeof sec, STAMP, "x"),
                     NEXTBOOT_OK);
    sec[4000] = 0x77;
    write_at(path, moff, sec, sizeof sec);
    assert_int_equal(nextio_read(&t, &q), CLI_OK);
    assert_int_equal(q.state, NEXTBOOT_SECTOR_BAD);
    assert_int_equal(nextio_write(&t, NULL, 0), CLI_OK);
    read_at(path, moff, sec, sizeof sec);
    assert_true(all_zero(sec, sizeof sec));

    (void)unlink(path);
}

/* --disk: two disks carry a boot partition. Named, one is found and written
 * and the other is not read for it; unnamed, the two are an error. A named
 * disk with no boot partition is an error too, never a reason to take one
 * of the others. */
static void test_disk_only(void **state)
{
    static uint8_t sec[512];
    char a[64];
    char b[64];
    char c[64];
    const char *both[2];
    const char *ac[2];
    next_target t;
    next_queued q;
    uint64_t moff = P1_BYTES + MARKER * 512u;

    (void)state;
    make_disk(a, sizeof a, 512u, 0xAA);
    make_disk(b, sizeof b, 512u, 0xBB);
    make_disk(c, sizeof c, 512u, 0);       /* a FAT, and no records */
    both[0] = a;
    both[1] = b;
    ac[0] = a;
    ac[1] = c;

    assert_int_equal(nextio_resolve(NULL, both, 2, &t), CLI_ERR_USAGE);
    assert_int_equal(nextio_resolve(NULL, ac, 2, &t), CLI_OK);
    assert_string_equal(t.path, a);
    assert_int_equal(nextio_resolve(NULL, both, 0, &t), CLI_ERR_NOTFOUND);

    memset(&t, 0, sizeof t);
    assert_int_equal(nextio_resolve(b, both, 2, &t), CLI_OK);
    assert_string_equal(t.path, b);
    assert_int_equal(t.sector, 512u);
    assert_int_equal(t.marker_lba, P1_BYTES / 512u + MARKER);
    assert_int_equal(nextio_resolve(a, both, 2, &t), CLI_OK);
    assert_string_equal(t.path, a);

    /* Named and wrong: not found, though both others would have done. */
    memset(&t, 0, sizeof t);
    assert_int_equal(nextio_resolve(c, both, 2, &t), CLI_ERR_NOTFOUND);
    assert_string_equal(t.path, "");
    assert_int_equal(nextio_resolve("/nonexistent/falcon", both, 2, &t),
                     CLI_ERR_NOTFOUND);

    /* Written through the named one: b carries it, a is as it was. */
    assert_int_equal(nextio_resolve(b, both, 2, &t), CLI_OK);
    assert_int_equal(nextio_write(&t, NEXTBOOT_PERSONAL, STAMP), CLI_OK);
    read_at(b, moff, sec, sizeof sec);
    assert_true(nextboot_sector_is_marker(sec, sizeof sec));
    read_at(a, moff, sec, sizeof sec);
    assert_true(all_zero(sec, sizeof sec));
    assert_int_equal(nextio_resolve(a, both, 2, &t), CLI_OK);
    assert_int_equal(nextio_read(&t, &q), CLI_OK);
    assert_int_equal(q.state, NEXTBOOT_SECTOR_NONE);

    /* And cleared through it: a still untouched. */
    assert_int_equal(nextio_resolve(b, both, 2, &t), CLI_OK);
    assert_int_equal(nextio_write(&t, NULL, 0), CLI_OK);
    read_at(b, moff, sec, sizeof sec);
    assert_true(all_zero(sec, sizeof sec));

    (void)unlink(a);
    (void)unlink(b);
    (void)unlink(c);
}

/* nextcore_sector_ours over a whole 4096-byte sector: every byte counts. */
static void test_ours_4k(void **state)
{
    static uint8_t sec[NEXTBOOT_SECTOR_MAX];

    (void)state;
    memset(sec, 0, sizeof sec);
    assert_true(nextcore_sector_ours(sec, sizeof sec));
    sec[sizeof sec - 1u] = 1;
    assert_false(nextcore_sector_ours(sec, sizeof sec));
    assert_true(nextcore_sector_ours(sec, 512u));
    assert_int_equal(nextboot_sector_write(sec, sizeof sec, 1u, "x"),
                     NEXTBOOT_OK);
    assert_true(nextcore_sector_ours(sec, sizeof sec));
    assert_false(nextcore_sector_ours(sec, 3000u));
    assert_false(nextcore_sector_ours(NULL, sizeof sec));
}

int run_nextio_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_marker_4k),
        cmocka_unit_test(test_disk_only),
        cmocka_unit_test(test_ours_4k),
    };

    return cmocka_run_group_tests_name("mkstick next-boot", tests, NULL, NULL);
}
