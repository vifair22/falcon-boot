/* Synthetic-volume tests for src/exfat/exfat.c.
 *
 * Every volume here is built byte by byte in memory, which is the only way to
 * reach the driver's rejection paths: mkfs.exfat will not produce a bad boot
 * signature, a BAD FAT entry, or a directory set missing its stream
 * extension. The read callback doubles as a fault injector so the I/O error
 * paths are exercised too.
 *
 * Convention in this file: the "standard" volume is 512-byte sectors with
 * 2048-byte clusters (64 directory entries per cluster), used wherever a
 * directory fits in one cluster. Tests that need a cluster boundary use the
 * "tight" volume, whose cluster equals one sector (16 entries), so 17 entries
 * are enough to span two clusters.
 */

#include "synth.h"
#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include <exfat/exfat.h>

#define STD_BPS 9u
#define STD_SPC 2u
#define STD_CLUSTERS 32u

/* 0x03 has the in-use bit clear, so the driver must skip it without treating
 * it as end-of-directory. Used as filler to control entry alignment. */
#define FILLER_TYPE 0x03

static void mount_std(synth_vol *sv, exfat_vol *v, unsigned spc)
{
    synth_init(sv, STD_BPS, spc, STD_CLUSTERS);
    assert_int_equal(exfat_mount(v, synth_read, sv), EXFAT_OK);
}

/* Reads every entry of d into names/flags; returns the terminating status
 * (0 for a clean end of directory, negative for an error). */
static int drain(exfat_dir *d, char names[][64], exfat_dirent *ents,
                 unsigned cap, unsigned *n)
{
    exfat_dirent e;
    int rc;

    *n = 0;
    for (;;) {
        rc = exfat_dir_next(d, &e);
        if (rc != 1)
            return rc;
        assert_true(*n < cap);
        if (names != NULL)
            ucs2_to_ascii(e.name, names[*n], 64);
        if (ents != NULL)
            ents[*n] = e;
        (*n)++;
    }
}

/* ---------------------------------------------------------------- mount */

static void test_mount_ok(void **state)
{
    synth_vol sv;
    exfat_vol v;

    (void)state;
    synth_init(&sv, STD_BPS, STD_SPC, STD_CLUSTERS);
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    assert_int_equal(v.bps_shift, STD_BPS);
    assert_int_equal(v.spc_shift, STD_SPC);
    assert_int_equal(v.fat_offset, sv.fat_offset);
    assert_int_equal(v.cluster_heap_offset, sv.heap_offset);
    assert_int_equal(v.cluster_count, STD_CLUSTERS);
    assert_int_equal(v.root_cluster, 2);
    assert_ptr_equal(v.read_ctx, &sv);
    /* The FAT cache must start invalid, or the first chain lookup would read
     * a stale sector 0. */
    assert_true(v.fat_cache_off == ~(uint64_t)0);
    /* Exactly one read: the whole VBR the driver needs is in sector 0. */
    assert_int_equal(sv.reads, 1);
    synth_free(&sv);
}

static void test_mount_bad_arg(void **state)
{
    synth_vol sv;
    exfat_vol v;

    (void)state;
    synth_init(&sv, STD_BPS, STD_SPC, STD_CLUSTERS);
    assert_int_equal(exfat_mount(NULL, synth_read, &sv), EXFAT_ERR_BAD_ARG);
    assert_int_equal(exfat_mount(&v, NULL, &sv), EXFAT_ERR_BAD_ARG);
    assert_int_equal(sv.reads, 0);
    synth_free(&sv);
}

static void test_mount_io_error(void **state)
{
    synth_vol sv;
    exfat_vol v;

    (void)state;
    synth_init(&sv, STD_BPS, STD_SPC, STD_CLUSTERS);
    sv.fail_on_read = 1;
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_IO);
    assert_int_equal(sv.reads, 1);
    synth_free(&sv);
}

static void test_mount_bad_fs_name(void **state)
{
    synth_vol sv;
    exfat_vol v;

    (void)state;
    synth_init(&sv, STD_BPS, STD_SPC, STD_CLUSTERS);
    sv.buf[3] = 'X'; /* FileSystemName no longer reads "EXFAT   " */
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    /* A FAT32 volume: correct 0x55AA, wrong name. */
    memcpy(sv.buf + 3, "MSDOS5.0", 8);
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    synth_free(&sv);
}

/* The driver identifies a volume by the FileSystemName at offset 3, not by
 * the JumpBoot instruction at offset 0. Pinned here so the choice is visible:
 * if a JumpBoot check is ever added, this test fails and the addition is a
 * deliberate decision rather than a silent behaviour change. */
static void test_mount_ignores_jump_boot(void **state)
{
    synth_vol sv;
    exfat_vol v;

    (void)state;
    synth_init(&sv, STD_BPS, STD_SPC, STD_CLUSTERS);
    sv.buf[0] = 0x00;
    sv.buf[1] = 0x00;
    sv.buf[2] = 0x00;
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    synth_free(&sv);
}

static void test_mount_bad_boot_signature(void **state)
{
    synth_vol sv;
    exfat_vol v;

    (void)state;
    synth_init(&sv, STD_BPS, STD_SPC, STD_CLUSTERS);
    sv.buf[510] = 0x00;
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    sv.buf[510] = 0x55;
    sv.buf[511] = 0x00;
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    sv.buf[511] = 0xAA;
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    synth_free(&sv);
}

static void test_mount_bps_shift_range(void **state)
{
    synth_vol sv;
    exfat_vol v;

    (void)state;
    synth_init(&sv, STD_BPS, STD_SPC, STD_CLUSTERS);
    sv.buf[108] = 8; /* 256-byte sectors: below the spec minimum */
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    /* 13 would mean 8 KB sectors, which would overrun exfat_vol::fat_cache. */
    sv.buf[108] = 13;
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    sv.buf[108] = 12;
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    assert_int_equal(v.bps_shift, 12);
    synth_free(&sv);
}

static void test_mount_spc_shift_cap(void **state)
{
    synth_vol sv;
    exfat_vol v;

    (void)state;
    synth_init(&sv, STD_BPS, STD_SPC, STD_CLUSTERS);
    /* bps_shift 9, so the spec's 32 MB cluster ceiling allows up to 16. */
    sv.buf[109] = 17;
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    sv.buf[109] = 16;
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    /* With 4 KB sectors the ceiling drops to 13. */
    sv.buf[108] = 12;
    sv.buf[109] = 14;
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    sv.buf[109] = 13;
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    synth_free(&sv);
}

static void test_mount_zero_geometry(void **state)
{
    synth_vol sv;
    exfat_vol v;

    (void)state;
    synth_init(&sv, STD_BPS, STD_SPC, STD_CLUSTERS);
    synth_put_le32(sv.buf + 80, 0); /* FatOffset */
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    synth_put_le32(sv.buf + 80, sv.fat_offset);

    synth_put_le32(sv.buf + 88, 0); /* ClusterHeapOffset */
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    synth_put_le32(sv.buf + 88, sv.heap_offset);

    synth_put_le32(sv.buf + 92, 0); /* ClusterCount */
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    synth_put_le32(sv.buf + 92, sv.cluster_count);

    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    synth_free(&sv);
}

static void test_mount_root_cluster_range(void **state)
{
    synth_vol sv;
    exfat_vol v;

    (void)state;
    synth_init(&sv, STD_BPS, STD_SPC, STD_CLUSTERS);
    /* Clusters 0 and 1 do not exist; the heap starts at 2. */
    synth_put_le32(sv.buf + 96, 0);
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    synth_put_le32(sv.buf + 96, 1);
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    /* One past the last cluster. */
    synth_put_le32(sv.buf + 96, STD_CLUSTERS + 2u);
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_ERR_BAD_SUPER);
    /* The last valid cluster is accepted. */
    synth_put_le32(sv.buf + 96, STD_CLUSTERS + 1u);
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    assert_int_equal(v.root_cluster, STD_CLUSTERS + 1u);
    synth_free(&sv);
}

/* ------------------------------------------------------------- dir open */

static void test_dir_open_bad_arg(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;

    (void)state;
    mount_std(&sv, &v, STD_SPC);

    assert_int_equal(exfat_dir_open_root(NULL, &d), EXFAT_ERR_BAD_ARG);
    assert_int_equal(exfat_dir_open_root(&v, NULL), EXFAT_ERR_BAD_ARG);

    memset(&e, 0, sizeof(e));
    e.is_dir = 1;
    e.first_cluster = 2;
    assert_int_equal(exfat_dir_open(NULL, &d, &e), EXFAT_ERR_BAD_ARG);
    assert_int_equal(exfat_dir_open(&v, NULL, &e), EXFAT_ERR_BAD_ARG);
    assert_int_equal(exfat_dir_open(&v, &d, NULL), EXFAT_ERR_BAD_ARG);

    /* A plain file is not openable as a directory. */
    e.is_dir = 0;
    assert_int_equal(exfat_dir_open(&v, &d, &e), EXFAT_ERR_BAD_ARG);

    assert_int_equal(exfat_dir_next(NULL, &e), EXFAT_ERR_BAD_ARG);
    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(exfat_dir_next(&d, NULL), EXFAT_ERR_BAD_ARG);
    synth_free(&sv);
}

static void test_dir_open_root_state(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_ptr_equal(d.vol, &v);
    assert_int_equal(d.first_cluster, 2);
    assert_int_equal(d.cur_cluster, 2);
    assert_int_equal(d.no_fat_chain, 0);
    /* The root has no DataLength of its own; it runs until the end marker. */
    assert_true(d.size == ~(uint64_t)0);
    assert_int_equal(d.pos, 0);
    assert_int_equal(d.cluster_pos, 0);
    assert_true(d.sec_buf_off == ~(uint64_t)0);
    synth_free(&sv);
}

static void test_dir_open_cluster_out_of_range(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    memset(&e, 0, sizeof(e));
    e.is_dir = 1;
    e.size = 2048;
    e.first_cluster = STD_CLUSTERS + 2u;
    assert_int_equal(exfat_dir_open(&v, &d, &e), EXFAT_ERR_CORRUPT);
    e.first_cluster = 1;
    assert_int_equal(exfat_dir_open(&v, &d, &e), EXFAT_ERR_CORRUPT);
    synth_free(&sv);
}

/* A zero first cluster means no allocation at all, which is a legal empty
 * directory. Iteration must end immediately without touching the device. */
static void test_dir_unallocated_is_empty(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;
    unsigned reads_before;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    memset(&e, 0, sizeof(e));
    e.is_dir = 1;
    e.first_cluster = 0;
    e.size = 0;
    assert_int_equal(exfat_dir_open(&v, &d, &e), EXFAT_OK);
    assert_int_equal(d.first_cluster, 0);
    reads_before = sv.reads;
    assert_int_equal(exfat_dir_next(&d, &e), 0);
    assert_int_equal(sv.reads, reads_before);
    assert_int_equal(d.pos, 0);
    synth_free(&sv);
}

/* An allocated but empty directory: the end marker is the first entry. */
static void test_dir_empty_end_marker_first(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    synth_put_end(synth_cluster(&sv, 4));
    memset(&e, 0, sizeof(e));
    e.is_dir = 1;
    e.first_cluster = 4;
    e.size = synth_cluster_bytes(&sv);
    assert_int_equal(exfat_dir_open(&v, &d, &e), EXFAT_OK);
    assert_int_equal(exfat_dir_next(&d, &e), 0);
    /* One entry consumed: the marker itself. */
    assert_int_equal(d.pos, SYNTH_ENT);
    synth_free(&sv);
}

/* ------------------------------------------------------------ dir walks */

static void test_dir_basic_walk(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent ents[8];
    char names[8][64];
    uint8_t *p;
    unsigned n;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    p = synth_cluster(&sv, 2);
    p += SYNTH_ENT * synth_put_set(p, "readme.txt", 0, SYNTH_FLAG_ALLOC,
                                   5, 1234);
    p += SYNTH_ENT * synth_put_set(p, "SUBDIR", SYNTH_ATTR_DIR,
                                   SYNTH_FLAG_ALLOC | SYNTH_FLAG_NOFAT,
                                   6, 2048);
    synth_put_end(p);
    /* Must never be reached: it sits past the end-of-directory marker. */
    synth_put_set(p + SYNTH_ENT, "ghost.iso", 0, SYNTH_FLAG_ALLOC, 7, 99);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(drain(&d, names, ents, 8, &n), 0);
    assert_int_equal(n, 2);

    assert_string_equal(names[0], "readme.txt");
    assert_int_equal(ents[0].name_len, 10);
    assert_int_equal(ents[0].is_dir, 0);
    assert_int_equal(ents[0].no_fat_chain, 0);
    assert_int_equal(ents[0].first_cluster, 5);
    assert_true(ents[0].size == 1234);

    assert_string_equal(names[1], "SUBDIR");
    assert_int_equal(ents[1].name_len, 6);
    assert_int_equal(ents[1].is_dir, 1);
    assert_int_equal(ents[1].no_fat_chain, 1);
    assert_int_equal(ents[1].first_cluster, 6);
    assert_true(ents[1].size == 2048);
    synth_free(&sv);
}

/* exfat_dir_next stops at the end marker but leaves the stream positioned
 * just past it, so a caller that keeps calling after a 0 walks into whatever
 * follows. The loader stops on the first 0, so this is harmless there;
 * asserted so the contract ("0 means stop, do not call again") is explicit. */
static void test_dir_next_after_end_is_not_sticky(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;
    char name[64];
    uint8_t *p;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    p = synth_cluster(&sv, 2);
    p += SYNTH_ENT * synth_put_set(p, "first.iso", 0, SYNTH_FLAG_ALLOC, 5, 1);
    synth_put_end(p);
    synth_put_set(p + SYNTH_ENT, "ghost.iso", 0, SYNTH_FLAG_ALLOC, 7, 2);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(exfat_dir_next(&d, &e), 1);
    assert_int_equal(exfat_dir_next(&d, &e), 0);
    assert_int_equal(exfat_dir_next(&d, &e), 1);
    ucs2_to_ascii(e.name, name, sizeof(name));
    assert_string_equal(name, "ghost.iso");
    synth_free(&sv);
}

/* Everything reports DataLength as its size (spec 7.6.3: that is the file's
 * length); files additionally carry ValidDataLength, clamped to size, and
 * the gap between the two reads as zeros. A sparse copy onto the stick is
 * the case where they differ (fix of 2026-09-13). */
static void test_dir_size_field_selection(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent ents[4];
    unsigned n;
    uint8_t *p;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    p = synth_cluster(&sv, 2);

    synth_put_file(p, 2, 0);
    synth_put_stream(p + SYNTH_ENT, SYNTH_FLAG_ALLOC, 4, 5, 100, 2048);
    synth_put_name(p + 2 * SYNTH_ENT, "f.gz", 4);
    p += 3 * SYNTH_ENT;

    synth_put_file(p, 2, SYNTH_ATTR_DIR);
    synth_put_stream(p + SYNTH_ENT, SYNTH_FLAG_ALLOC, 3, 6, 100, 2048);
    synth_put_name(p + 2 * SYNTH_ENT, "dir", 3);
    p += 3 * SYNTH_ENT;

    synth_put_end(p);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(drain(&d, NULL, ents, 4, &n), 0);
    assert_int_equal(n, 2);
    assert_true(ents[0].size == 2048);
    assert_true(ents[0].valid_len == 100);
    assert_true(ents[1].size == 2048);
    assert_true(ents[1].valid_len == 2048);
    synth_free(&sv);
}

/* The valid_len..size tail is served as zeros, never as the stale device
 * bytes that sit in the allocated clusters. */
static void test_file_sparse_tail_reads_zero(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;
    exfat_file f;
    uint8_t buf[192];
    unsigned i;
    uint8_t *p;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    p = synth_cluster(&sv, 2);

    /* One cluster of allocation (cluster 4), 128 bytes valid of 192 total;
     * the cluster's bytes are a nonzero pattern so a leak is visible. */
    memset(synth_cluster(&sv, 4), 0xAB, 512);
    synth_put_file(p, 2, 0);
    synth_put_stream(p + SYNTH_ENT, SYNTH_FLAG_ALLOC | SYNTH_FLAG_NOFAT,
                     6, 4, 128, 192);
    synth_put_name(p + 2 * SYNTH_ENT, "sparse", 6);
    synth_put_end(p + 3 * SYNTH_ENT);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(exfat_dir_next(&d, &e), 1);
    assert_true(e.size == 192 && e.valid_len == 128);
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);

    /* Whole file: pattern up to valid_len, zeros to size. */
    memset(buf, 0x55, sizeof(buf));
    assert_int_equal(exfat_file_read(&f, 0, 192, buf), EXFAT_OK);
    for (i = 0; i < 128; i++)
        assert_true(buf[i] == 0xAB);
    for (i = 128; i < 192; i++)
        assert_true(buf[i] == 0x00);

    /* A read wholly inside the tail touches the device not at all and is
     * all zeros; one straddling the boundary splits correctly. */
    memset(buf, 0x55, sizeof(buf));
    assert_int_equal(exfat_file_read(&f, 130, 62, buf), EXFAT_OK);
    for (i = 0; i < 62; i++)
        assert_true(buf[i] == 0x00);
    memset(buf, 0x55, sizeof(buf));
    assert_int_equal(exfat_file_read(&f, 120, 16, buf), EXFAT_OK);
    for (i = 0; i < 8; i++)
        assert_true(buf[i] == 0xAB);
    for (i = 8; i < 16; i++)
        assert_true(buf[i] == 0x00);

    /* Past DataLength still refuses. */
    assert_int_equal(exfat_file_read(&f, 192, 1, buf), EXFAT_ERR_RANGE);
    synth_free(&sv);
}

/* A name longer than 15 UTF-16 units spans several name entries; the driver
 * must stitch them back together byte for byte. */
static void test_dir_long_name_round_trip(void **state)
{
    static const char *const longname =
        "a-really-long-filename-over-15.bin"; /* 34 chars -> 3 name entries */
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;
    char name[64];
    uint8_t *p;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    p = synth_cluster(&sv, 2);
    p += SYNTH_ENT * synth_put_set(p, longname, 0, SYNTH_FLAG_ALLOC, 5, 7);
    synth_put_end(p);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(exfat_dir_next(&d, &e), 1);
    ucs2_to_ascii(e.name, name, sizeof(name));
    assert_string_equal(name, longname);
    assert_int_equal(e.name_len, strlen(longname));
    assert_int_equal(e.name[e.name_len], 0);
    assert_int_equal(exfat_dir_next(&d, &e), 0);
    synth_free(&sv);
}

/* The spec's 255-unit maximum: 17 name entries, and the terminator lands on
 * the last slot of exfat_dirent::name. */
static void test_dir_max_length_name(void **state)
{
    char longname[256];
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;
    char name[512];
    uint8_t *p;
    size_t i;

    (void)state;
    for (i = 0; i < 255; i++)
        longname[i] = (char)('a' + (int)(i % 26));
    longname[255] = '\0';

    mount_std(&sv, &v, STD_SPC);
    p = synth_cluster(&sv, 2);
    p += SYNTH_ENT * synth_put_set(p, longname, 0, SYNTH_FLAG_ALLOC, 5, 7);
    synth_put_end(p);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(exfat_dir_next(&d, &e), 1);
    assert_int_equal(e.name_len, 255);
    assert_int_equal(e.name[255], 0);
    ucs2_to_ascii(e.name, name, sizeof(name));
    assert_string_equal(name, longname);
    synth_free(&sv);
}

/* SecondaryCount below 2 cannot describe a valid set (stream + one name), so
 * the file entry is skipped and iteration resumes with the next entry. */
static void test_dir_short_secondary_count_skipped(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    char names[4][64];
    unsigned n;
    uint8_t *p;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    p = synth_cluster(&sv, 2);
    synth_put_file(p, 1, 0);
    synth_put_stream(p + SYNTH_ENT, SYNTH_FLAG_ALLOC, 3, 5, 1, 1);
    synth_put_name(p + 2 * SYNTH_ENT, "bad", 3);
    p += 3 * SYNTH_ENT;
    p += SYNTH_ENT * synth_put_set(p, "good.iso", 0, SYNTH_FLAG_ALLOC, 6, 8);
    synth_put_end(p);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(drain(&d, names, NULL, 4, &n), 0);
    assert_int_equal(n, 1);
    assert_string_equal(names[0], "good.iso");
    synth_free(&sv);
}

/* The stream extension must immediately follow the file entry. When it does
 * not, the driver drops the set and keeps walking rather than failing the
 * whole directory. */
static void test_dir_missing_stream_tolerated(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    char names[4][64];
    unsigned n;
    uint8_t *p;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    p = synth_cluster(&sv, 2);
    synth_put_file(p, 3, 0);
    /* Name entry where the stream extension belongs. */
    synth_put_name(p + SYNTH_ENT, "orphan", 6);
    synth_put_name(p + 2 * SYNTH_ENT, "orphan2", 7);
    p += 3 * SYNTH_ENT;
    p += SYNTH_ENT * synth_put_set(p, "good.iso", 0, SYNTH_FLAG_ALLOC, 6, 8);
    synth_put_end(p);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(drain(&d, names, NULL, 4, &n), 0);
    assert_int_equal(n, 1);
    assert_string_equal(names[0], "good.iso");
    synth_free(&sv);
}

/* NameLength 0 describes nothing nameable; the set is dropped. */
static void test_dir_nameless_set_skipped(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    char names[4][64];
    unsigned n;
    uint8_t *p;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    p = synth_cluster(&sv, 2);
    synth_put_file(p, 2, 0);
    synth_put_stream(p + SYNTH_ENT, SYNTH_FLAG_ALLOC, 0, 5, 1, 1);
    synth_put_name(p + 2 * SYNTH_ENT, "unused", 6);
    p += 3 * SYNTH_ENT;
    p += SYNTH_ENT * synth_put_set(p, "good.iso", 0, SYNTH_FLAG_ALLOC, 6, 8);
    synth_put_end(p);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(drain(&d, names, NULL, 4, &n), 0);
    assert_int_equal(n, 1);
    assert_string_equal(names[0], "good.iso");
    synth_free(&sv);
}

/* A secondary entry that is not a name entry (a vendor extension, say) ends
 * the name: what was collected so far is kept and the set is still reported. */
static void test_dir_non_name_secondary_truncates(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;
    char name[64];
    uint8_t *p;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    p = synth_cluster(&sv, 2);
    synth_put_file(p, 4, 0);
    synth_put_stream(p + SYNTH_ENT, SYNTH_FLAG_ALLOC, 30, 5, 9, 9);
    synth_put_name(p + 2 * SYNTH_ENT, "abcdefghijklmno", 15);
    memset(p + 3 * SYNTH_ENT, 0, SYNTH_ENT);
    p[3 * SYNTH_ENT] = 0xE0; /* vendor extension, in use, not a name entry */
    p += 4 * SYNTH_ENT;
    synth_put_end(p);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(exfat_dir_next(&d, &e), 1);
    ucs2_to_ascii(e.name, name, sizeof(name));
    assert_string_equal(name, "abcdefghijklmno");
    assert_int_equal(e.name_len, 15);
    synth_free(&sv);
}

/* DataLength bounds a subdirectory even with no end marker: the walk stops
 * when the next entry would run past the allocation. */
static void test_dir_data_length_bounds_walk(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;
    char names[4][64];
    unsigned n;
    uint8_t *p;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    p = synth_cluster(&sv, 4);
    (void)synth_put_set(p, "only.iso", 0, SYNTH_FLAG_ALLOC, 5, 3);
    /* Deliberately no end marker; the set below must stay invisible. */
    synth_put_set(p + 3 * SYNTH_ENT, "past-end.iso", 0, SYNTH_FLAG_ALLOC,
                  6, 4);

    memset(&e, 0, sizeof(e));
    e.is_dir = 1;
    e.first_cluster = 4;
    e.size = 3 * SYNTH_ENT; /* exactly the one set */
    assert_int_equal(exfat_dir_open(&v, &d, &e), EXFAT_OK);
    assert_int_equal(drain(&d, names, NULL, 4, &n), 0);
    assert_int_equal(n, 1);
    assert_string_equal(names[0], "only.iso");
    assert_int_equal(d.pos, 3 * SYNTH_ENT);
    synth_free(&sv);
}

/* The allocation ends in the middle of a set's name entries. */
static void test_dir_truncated_mid_name(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;
    uint8_t *p;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    p = synth_cluster(&sv, 4);
    synth_put_file(p, 3, 0);
    synth_put_stream(p + SYNTH_ENT, SYNTH_FLAG_ALLOC, 20, 5, 9, 9);
    synth_put_name(p + 2 * SYNTH_ENT, "abcdefghijklmno", 15);
    /* The second name entry would live here, but the allocation stops. */

    memset(&e, 0, sizeof(e));
    e.is_dir = 1;
    e.first_cluster = 4;
    e.size = 3 * SYNTH_ENT;
    assert_int_equal(exfat_dir_open(&v, &d, &e), EXFAT_OK);
    assert_int_equal(exfat_dir_next(&d, &e), 0);
    synth_free(&sv);
}

static void test_dir_open_nested(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dir sub;
    exfat_dirent ents[4];
    char names[4][64];
    unsigned n;
    uint8_t *p;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    p = synth_cluster(&sv, 2);
    p += SYNTH_ENT * synth_put_set(p, "Linux", SYNTH_ATTR_DIR,
                                   SYNTH_FLAG_ALLOC, 4,
                                   synth_cluster_bytes(&sv));
    synth_put_end(p);

    p = synth_cluster(&sv, 4);
    p += SYNTH_ENT * synth_put_set(p, "ubuntu-fake.iso", 0, SYNTH_FLAG_ALLOC,
                                   6, 4242);
    synth_put_end(p);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(drain(&d, names, ents, 4, &n), 0);
    assert_int_equal(n, 1);
    assert_int_equal(ents[0].is_dir, 1);

    assert_int_equal(exfat_dir_open(&v, &sub, &ents[0]), EXFAT_OK);
    assert_int_equal(sub.first_cluster, 4);
    assert_true(sub.size == synth_cluster_bytes(&sv));
    assert_int_equal(drain(&sub, names, ents, 4, &n), 0);
    assert_int_equal(n, 1);
    assert_string_equal(names[0], "ubuntu-fake.iso");
    assert_true(ents[0].size == 4242);
    synth_free(&sv);
}

/* -------------------------------------------------- cluster boundaries */

/* Fills the tight volume's cluster 2 with five 3-entry sets plus the file
 * entry of a sixth set, so the sixth set straddles the cluster boundary.
 * Cluster 3 gets that set's stream and name entries, then the end marker. */
static void build_spanning_dir(synth_vol *sv)
{
    uint8_t *p = synth_cluster(sv, 2);
    unsigned i;

    for (i = 0; i < 5; i++) {
        char nm[16];

        snprintf(nm, sizeof(nm), "file%u.iso", i + 1);
        p += SYNTH_ENT * synth_put_set(p, nm, 0, SYNTH_FLAG_ALLOC,
                                       6u + i, 10u + i);
    }
    /* Entry 15 of 16: the file entry, alone at the end of cluster 2. */
    synth_put_file(p, 2, 0);

    p = synth_cluster(sv, 3);
    synth_put_stream(p, SYNTH_FLAG_ALLOC, 11, 20, 4444, 4444);
    synth_put_name(p + SYNTH_ENT, "spanner.bin", 11);
    synth_put_end(p + 2 * SYNTH_ENT);
}

static void test_dir_set_spans_cluster_boundary(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent ents[8];
    char names[8][64];
    unsigned n;

    (void)state;
    /* spc_shift 0: one cluster is one 512-byte sector, i.e. 16 entries. */
    synth_init(&sv, STD_BPS, 0, STD_CLUSTERS);
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    synth_fat_set(&sv, 2, 3);
    synth_fat_set(&sv, 3, 0xFFFFFFFFu);
    build_spanning_dir(&sv);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(drain(&d, names, ents, 8, &n), 0);
    assert_int_equal(n, 6);
    assert_string_equal(names[0], "file1.iso");
    assert_string_equal(names[4], "file5.iso");
    assert_string_equal(names[5], "spanner.bin");
    assert_true(ents[5].size == 4444);
    assert_int_equal(ents[5].first_cluster, 20);

    /* The iterator followed the FAT into cluster 3 and rebased its position
     * bookkeeping on the new cluster. */
    assert_int_equal(d.cur_cluster, 3);
    assert_int_equal(d.cluster_pos, 512);
    assert_int_equal(d.pos, 512 + 3 * SYNTH_ENT);
    synth_free(&sv);
}

/* Fills the tight volume's cluster 2 with five sets and one filler entry, so
 * the next read lands exactly on the cluster boundary. */
static void fill_tight_cluster(synth_vol *sv, uint32_t cl)
{
    uint8_t *p = synth_cluster(sv, cl);
    unsigned i;

    for (i = 0; i < 5; i++) {
        char nm[16];

        snprintf(nm, sizeof(nm), "file%u.iso", i + 1);
        p += SYNTH_ENT * synth_put_set(p, nm, 0, SYNTH_FLAG_ALLOC,
                                       6u + i, 10u + i);
    }
    memset(p, 0, SYNTH_ENT);
    p[0] = FILLER_TYPE;
}

/* Walks the root of a tight volume whose FAT entry for cluster 2 is `fat2`,
 * returns the terminating status and the entry count. */
static int walk_tight_root(synth_vol *sv, uint32_t fat2, unsigned *n)
{
    exfat_vol v;
    exfat_dir d;

    synth_init(sv, STD_BPS, 0, STD_CLUSTERS);
    assert_int_equal(exfat_mount(&v, synth_read, sv), EXFAT_OK);
    synth_fat_set(sv, 2, fat2);
    fill_tight_cluster(sv, 2);
    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    return drain(&d, NULL, NULL, 8, n);
}

static void test_dir_fat_eof_ends_walk(void **state)
{
    synth_vol sv;
    unsigned n;

    (void)state;
    assert_int_equal(walk_tight_root(&sv, 0xFFFFFFFFu, &n), 0);
    assert_int_equal(n, 5);
    synth_free(&sv);
}

static void test_dir_fat_bad_cluster_is_corrupt(void **state)
{
    synth_vol sv;
    unsigned n;

    (void)state;
    /* 0xFFFFFFF7 marks a bad cluster; it must never be followed. */
    assert_int_equal(walk_tight_root(&sv, 0xFFFFFFF7u, &n),
                     EXFAT_ERR_CORRUPT);
    assert_int_equal(n, 5);
    synth_free(&sv);
}

static void test_dir_fat_out_of_range_is_corrupt(void **state)
{
    synth_vol sv;
    unsigned n;

    (void)state;
    assert_int_equal(walk_tight_root(&sv, STD_CLUSTERS + 2u, &n),
                     EXFAT_ERR_CORRUPT);
    assert_int_equal(n, 5);
    synth_free(&sv);

    /* 0 is the free-cluster marker and 1 is reserved; neither is a chain
     * successor. */
    assert_int_equal(walk_tight_root(&sv, 0, &n), EXFAT_ERR_CORRUPT);
    synth_free(&sv);
    assert_int_equal(walk_tight_root(&sv, 1, &n), EXFAT_ERR_CORRUPT);
    synth_free(&sv);
}

static void test_dir_no_fat_chain_is_contiguous(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;
    char names[8][64];
    unsigned n;
    uint8_t *p;

    (void)state;
    synth_init(&sv, STD_BPS, 0, STD_CLUSTERS);
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    fill_tight_cluster(&sv, 4);
    /* Poison the FAT: reaching cluster 5 must not consult it. */
    synth_fat_set(&sv, 4, 0xFFFFFFF7u);

    p = synth_cluster(&sv, 5);
    p += SYNTH_ENT * synth_put_set(p, "next-cluster.iso", 0,
                                   SYNTH_FLAG_ALLOC, 9, 77);
    synth_put_end(p);

    memset(&e, 0, sizeof(e));
    e.is_dir = 1;
    e.no_fat_chain = 1;
    e.first_cluster = 4;
    e.size = 1024; /* two 512-byte clusters */
    assert_int_equal(exfat_dir_open(&v, &d, &e), EXFAT_OK);
    assert_int_equal(d.no_fat_chain, 1);
    assert_int_equal(drain(&d, names, NULL, 8, &n), 0);
    assert_int_equal(n, 6);
    assert_string_equal(names[5], "next-cluster.iso");
    assert_int_equal(d.cur_cluster, 5);
    synth_free(&sv);
}

static void test_dir_no_fat_chain_past_last_cluster(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;
    unsigned n;

    (void)state;
    synth_init(&sv, STD_BPS, 0, STD_CLUSTERS);
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    fill_tight_cluster(&sv, STD_CLUSTERS + 1u); /* the last cluster */

    memset(&e, 0, sizeof(e));
    e.is_dir = 1;
    e.no_fat_chain = 1;
    e.first_cluster = STD_CLUSTERS + 1u;
    e.size = 1024; /* claims a second cluster that does not exist */
    assert_int_equal(exfat_dir_open(&v, &d, &e), EXFAT_OK);
    assert_int_equal(drain(&d, NULL, NULL, 8, &n), EXFAT_ERR_CORRUPT);
    assert_int_equal(n, 5);
    synth_free(&sv);
}

/* A set whose file entry is the last entry of a cluster, so fetching the
 * stream extension has to cross the boundary. With a BAD FAT link that
 * crossing fails, and the error must propagate out of exfat_dir_next rather
 * than being mistaken for an end of directory. */
static void test_dir_error_fetching_stream(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    unsigned n;
    uint8_t *p;
    unsigned i;

    (void)state;
    synth_init(&sv, STD_BPS, 0, STD_CLUSTERS);
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    synth_fat_set(&sv, 2, 0xFFFFFFF7u);

    p = synth_cluster(&sv, 2);
    for (i = 0; i < 5; i++) {
        char nm[16];

        snprintf(nm, sizeof(nm), "file%u.iso", i + 1);
        p += SYNTH_ENT * synth_put_set(p, nm, 0, SYNTH_FLAG_ALLOC, 6u + i, 1);
    }
    synth_put_file(p, 2, 0); /* entry 15: the stream lands in cluster 3 */

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(drain(&d, NULL, NULL, 8, &n), EXFAT_ERR_CORRUPT);
    assert_int_equal(n, 5);
    synth_free(&sv);
}

/* Same idea one entry later: the file entry and its stream extension both fit
 * in the cluster, and the first name entry is what falls off the end. */
static void test_dir_error_fetching_name(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    unsigned n;
    uint8_t *p;
    unsigned i;

    (void)state;
    synth_init(&sv, STD_BPS, 0, STD_CLUSTERS);
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    synth_fat_set(&sv, 2, 0xFFFFFFF7u);

    p = synth_cluster(&sv, 2);
    for (i = 0; i < 4; i++) {
        char nm[16];

        snprintf(nm, sizeof(nm), "file%u.iso", i + 1);
        p += SYNTH_ENT * synth_put_set(p, nm, 0, SYNTH_FLAG_ALLOC, 6u + i, 1);
    }
    for (i = 0; i < 2; i++) { /* entries 12 and 13 */
        memset(p, 0, SYNTH_ENT);
        p[0] = FILLER_TYPE;
        p += SYNTH_ENT;
    }
    synth_put_file(p, 2, 0);
    synth_put_stream(p + SYNTH_ENT, SYNTH_FLAG_ALLOC, 5, 9, 1, 1);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(drain(&d, NULL, NULL, 8, &n), EXFAT_ERR_CORRUPT);
    assert_int_equal(n, 4);
    synth_free(&sv);
}

/* ------------------------------------------------------------- I/O fail */

static void test_dir_cluster_read_io_error(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dirent e;

    (void)state;
    mount_std(&sv, &v, STD_SPC);
    synth_put_end(synth_cluster(&sv, 2));
    /* Fail only the read of the root directory's first sector. */
    sv.fail_off = (int64_t)((uint64_t)sv.heap_offset << sv.bps_shift);
    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(exfat_dir_next(&d, &e), EXFAT_ERR_IO);
    synth_free(&sv);
}

static void test_dir_fat_read_io_error(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    unsigned n;

    (void)state;
    synth_init(&sv, STD_BPS, 0, STD_CLUSTERS);
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);
    synth_fat_set(&sv, 2, 3);
    fill_tight_cluster(&sv, 2);
    /* Fail only the FAT sector read, which happens at the cluster boundary
     * after the five sets in cluster 2 have already been returned. */
    sv.fail_off = (int64_t)((uint64_t)sv.fat_offset << sv.bps_shift);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(drain(&d, NULL, NULL, 8, &n), EXFAT_ERR_IO);
    assert_int_equal(n, 5);
    synth_free(&sv);
}

/* The volume-level FAT cache must serve a second lookup in the same sector
 * without a second device read. */
static void test_fat_cache_reused(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dir d;
    exfat_dir sub;
    exfat_dirent ents[8];
    exfat_dirent e;
    unsigned n;
    unsigned reads_after_first;
    uint8_t *p;

    (void)state;
    synth_init(&sv, STD_BPS, 0, STD_CLUSTERS);
    assert_int_equal(exfat_mount(&v, synth_read, &sv), EXFAT_OK);

    /* Root: cluster 2 -> 3. Subdirectory: cluster 6 -> 7. Both FAT entries
     * live in FAT sector 0, so the second chain walk must hit the cache. */
    synth_fat_set(&sv, 2, 3);
    synth_fat_set(&sv, 6, 7);

    fill_tight_cluster(&sv, 2);
    p = synth_cluster(&sv, 3);
    p += SYNTH_ENT * synth_put_set(p, "sub", SYNTH_ATTR_DIR,
                                   SYNTH_FLAG_ALLOC, 6, 1024);
    synth_put_end(p);

    fill_tight_cluster(&sv, 6);
    p = synth_cluster(&sv, 7);
    p += SYNTH_ENT * synth_put_set(p, "leaf.iso", 0, SYNTH_FLAG_ALLOC, 9, 5);
    synth_put_end(p);

    assert_int_equal(exfat_dir_open_root(&v, &d), EXFAT_OK);
    assert_int_equal(exfat_dir_next(&d, &e), 1); /* file1.iso */
    assert_int_equal(drain(&d, NULL, ents, 8, &n), 0);
    assert_int_equal(n, 5); /* file2..file5 plus the subdirectory */
    e = ents[4];
    assert_int_equal(e.is_dir, 1);
    assert_true(v.fat_cache_off == ((uint64_t)sv.fat_offset << STD_BPS));
    reads_after_first = sv.reads;

    assert_int_equal(exfat_dir_open(&v, &sub, &e), EXFAT_OK);
    assert_int_equal(drain(&sub, NULL, NULL, 8, &n), 0);
    assert_int_equal(n, 6);
    /* Two cluster-sector reads for the subdirectory and no FAT read: the
     * cached sector already held the 6 -> 7 link. */
    assert_int_equal(sv.reads - reads_after_first, 2);
    synth_free(&sv);
}

/* ------------------------------------------------------------------ run */

int run_synth_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_mount_ok),
        cmocka_unit_test(test_mount_bad_arg),
        cmocka_unit_test(test_mount_io_error),
        cmocka_unit_test(test_mount_bad_fs_name),
        cmocka_unit_test(test_mount_ignores_jump_boot),
        cmocka_unit_test(test_mount_bad_boot_signature),
        cmocka_unit_test(test_mount_bps_shift_range),
        cmocka_unit_test(test_mount_spc_shift_cap),
        cmocka_unit_test(test_mount_zero_geometry),
        cmocka_unit_test(test_mount_root_cluster_range),
        cmocka_unit_test(test_dir_open_bad_arg),
        cmocka_unit_test(test_dir_open_root_state),
        cmocka_unit_test(test_dir_open_cluster_out_of_range),
        cmocka_unit_test(test_dir_unallocated_is_empty),
        cmocka_unit_test(test_dir_empty_end_marker_first),
        cmocka_unit_test(test_dir_basic_walk),
        cmocka_unit_test(test_dir_next_after_end_is_not_sticky),
        cmocka_unit_test(test_dir_size_field_selection),
        cmocka_unit_test(test_file_sparse_tail_reads_zero),
        cmocka_unit_test(test_dir_long_name_round_trip),
        cmocka_unit_test(test_dir_max_length_name),
        cmocka_unit_test(test_dir_short_secondary_count_skipped),
        cmocka_unit_test(test_dir_missing_stream_tolerated),
        cmocka_unit_test(test_dir_nameless_set_skipped),
        cmocka_unit_test(test_dir_non_name_secondary_truncates),
        cmocka_unit_test(test_dir_data_length_bounds_walk),
        cmocka_unit_test(test_dir_truncated_mid_name),
        cmocka_unit_test(test_dir_open_nested),
        cmocka_unit_test(test_dir_set_spans_cluster_boundary),
        cmocka_unit_test(test_dir_fat_eof_ends_walk),
        cmocka_unit_test(test_dir_fat_bad_cluster_is_corrupt),
        cmocka_unit_test(test_dir_fat_out_of_range_is_corrupt),
        cmocka_unit_test(test_dir_no_fat_chain_is_contiguous),
        cmocka_unit_test(test_dir_no_fat_chain_past_last_cluster),
        cmocka_unit_test(test_dir_error_fetching_stream),
        cmocka_unit_test(test_dir_error_fetching_name),
        cmocka_unit_test(test_dir_cluster_read_io_error),
        cmocka_unit_test(test_dir_fat_read_io_error),
        cmocka_unit_test(test_fat_cache_reused),
    };

    return cmocka_run_group_tests_name("exfat synthetic", tests, NULL, NULL);
}
