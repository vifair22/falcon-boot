/* Tests for falcon-core/src/fs/fat.c against real mkfs.vfat volumes
 * (falcon-core/tools/mk-test-fat.sh, docs/notes/iso-reader-route.md stage B).
 *
 * Real images rather than synthesised ones, because the things most likely to
 * be wrong here are the things a hand-built fixture would get wrong in the
 * same direction as the driver: the exact cluster-count thresholds that pick
 * the FAT width, the layout of an LFN run and its checksum, and the lower-case
 * flags Windows records for a name that is 8.3 but not upper case. mkfs.vfat
 * and mtools are the second opinion.
 *
 * Every assertion runs against all three widths, since FAT12, FAT16 and FAT32
 * differ only in how a chain is followed and a driver that picks the wrong
 * width does not fail, it reads the wrong clusters.
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

#include <fs/fat.h>
#include <fs/fs.h>

#define IMG12 "build/test-fat12.img"
#define IMG16 "build/test-fat16.img"
#define IMG32 "build/test-fat32.img"

#define BIG_SIZE (100u * 1024u)

typedef struct {
    const char *path;
    fat_type    expect;
} fixture;

static const fixture g_fixtures[] = {
    { IMG12, FAT_TYPE_12 },
    { IMG16, FAT_TYPE_16 },
    { IMG32, FAT_TYPE_32 }
};

#define FIXTURE_N (sizeof g_fixtures / sizeof g_fixtures[0])

static FILE *g_fp;

static int img_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    FILE *f = ctx;

    if (fseek(f, (long)off, SEEK_SET) != 0)
        return -1;
    return fread(buf, 1, (size_t)len, f) == len ? 0 : -1;
}

static FILE *open_img(const char *path)
{
    FILE *f = fopen(path, "rb");

    assert_non_null(f);
    return f;
}

/* UCS-2 against ASCII, which is all these names are. */
static int name_is(const fat_dirent *e, const char *want)
{
    size_t i;

    for (i = 0; want[i] != '\0'; i++) {
        if (e->name[i] != (uint16_t)(unsigned char)want[i])
            return 0;
    }
    return e->name[i] == 0 && e->name_len == i;
}

static int find_in_dir(fat_vol *v, fat_dir *d, const char *want,
                       fat_dirent *out)
{
    fat_dirent e;
    int rc;

    (void)v;
    while ((rc = fat_dir_next(d, &e)) == 1) {
        if (name_is(&e, want)) {
            *out = e;
            return 1;
        }
    }
    assert_int_equal(rc, 0);
    return 0;
}

static int find_in_root(fat_vol *v, const char *want, fat_dirent *out)
{
    fat_dir d;

    assert_int_equal(fat_dir_open_root(v, &d), FAT_OK);
    return find_in_dir(v, &d, want, out);
}

/* ---- the tests, each run once per width ---- */

/* The width is derived from the cluster count, not recorded. If this is wrong
 * nothing else in the file can be right, so it goes first. */
static void test_fat_type_and_label(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        assert_int_equal((int)v.type, (int)g_fixtures[i].expect);
        assert_string_equal(v.label, "FALCON-FAT");
        assert_true(v.sector_size >= 512u);
        assert_true(v.cluster_size >= v.sector_size);
        assert_true(v.clusters > 0);
        fclose(g_fp);
    }
}

/* The four name shapes in the fixture, which between them cover the short
 * name path, the lower-case flags and a full LFN run. */
static void test_fat_names(void **state)
{
    static const char *want[] = {
        "README.TXT", "lower.txt", "a-long-file-name.txt", "NoExtension",
        "big.bin", "sub"
    };
    size_t i, k;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        for (k = 0; k < sizeof want / sizeof want[0]; k++) {
            fat_dirent e;

            assert_int_equal(find_in_root(&v, want[k], &e), 1);
        }
        fclose(g_fp);
    }
}

/* "." and ".." are this directory and its parent, and the volume label is the
 * volume's name; none of the three is a file and none may be produced. */
static void test_fat_hidden_entries(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;
        fat_dirent sub;
        fat_dir d;
        fat_dirent e;
        int rc;
        int saw = 0;

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);

        /* The root of a FAT12/16 volume carries the label entry. */
        assert_int_equal(fat_dir_open_root(&v, &d), FAT_OK);
        while ((rc = fat_dir_next(&d, &e)) == 1) {
            assert_false(name_is(&e, "FALCON-FAT"));
            assert_false(name_is(&e, "."));
            assert_false(name_is(&e, ".."));
        }
        assert_int_equal(rc, 0);

        /* A subdirectory carries "." and "..". */
        assert_int_equal(find_in_root(&v, "sub", &sub), 1);
        assert_true(sub.is_dir);
        assert_int_equal(fat_dir_open(&v, &d, &sub), FAT_OK);
        while ((rc = fat_dir_next(&d, &e)) == 1) {
            assert_false(name_is(&e, "."));
            assert_false(name_is(&e, ".."));
            if (name_is(&e, "nested.txt") || name_is(&e, "deeper"))
                saw++;
        }
        assert_int_equal(rc, 0);
        assert_int_equal(saw, 2);
        fclose(g_fp);
    }
}

static void test_fat_read_small_file(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;
        fat_file f;
        fat_dirent e;
        char buf[32];

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        assert_int_equal(find_in_root(&v, "a-long-file-name.txt", &e), 1);
        assert_false(e.is_dir);
        assert_true(e.is_regular);
        assert_int_equal(e.size, 19);
        assert_int_equal(fat_file_open(&v, &f, &e), FAT_OK);
        memset(buf, 0, sizeof buf);
        assert_int_equal(fat_file_read(&f, 0, e.size, buf), FAT_OK);
        assert_memory_equal(buf, "long name contents\n", 19);
        fclose(g_fp);
    }
}

/* 100 KiB spans several clusters on every width, so this is the chain walk.
 * Byte n is (n % 251), which makes a chain followed wrongly show up as a
 * value out of sequence rather than as a block of zeros. */
static void test_fat_read_multi_cluster(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;
        fat_file f;
        fat_dirent e;
        unsigned char *buf = malloc(BIG_SIZE);
        unsigned k;

        assert_non_null(buf);
        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        assert_int_equal(find_in_root(&v, "big.bin", &e), 1);
        assert_int_equal(e.size, BIG_SIZE);
        assert_int_equal(fat_file_open(&v, &f, &e), FAT_OK);
        memset(buf, 0xAA, BIG_SIZE);
        assert_int_equal(fat_file_read(&f, 0, BIG_SIZE, buf), FAT_OK);
        for (k = 0; k < BIG_SIZE; k++)
            assert_int_equal(buf[k], (unsigned char)(k % 251u));
        free(buf);
        fclose(g_fp);
    }
}

/* Unaligned and partial reads, which is where the bounce buffer lives. Read
 * the same file in odd-sized pieces at odd offsets and it must agree with the
 * whole-file read byte for byte. */
static void test_fat_read_unaligned(void **state)
{
    static const uint32_t offs[] = { 1u, 511u, 512u, 513u, 4095u, 40000u };
    size_t i, k;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;
        fat_file f;
        fat_dirent e;
        unsigned char buf[1000];

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        assert_int_equal(find_in_root(&v, "big.bin", &e), 1);
        assert_int_equal(fat_file_open(&v, &f, &e), FAT_OK);
        for (k = 0; k < sizeof offs / sizeof offs[0]; k++) {
            uint32_t off = offs[k];
            uint32_t len = 997u;
            uint32_t j;

            memset(buf, 0, sizeof buf);
            assert_int_equal(fat_file_read(&f, off, len, buf), FAT_OK);
            for (j = 0; j < len; j++)
                assert_int_equal(buf[j], (unsigned char)((off + j) % 251u));
        }
        /* Wholly inside the file or nothing. */
        assert_int_equal(fat_file_read(&f, BIG_SIZE - 1u, 2, buf),
                         FAT_ERR_RANGE);
        assert_int_equal(fat_file_read(&f, BIG_SIZE + 1u, 1, buf),
                         FAT_ERR_RANGE);
        assert_int_equal(fat_file_read(&f, BIG_SIZE, 0, buf), FAT_OK);
        fclose(g_fp);
    }
}

static void test_fat_nested_directory(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;
        fat_dir d;
        fat_dirent sub, deeper, leaf;
        fat_file f;
        char buf[16];

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        assert_int_equal(find_in_root(&v, "sub", &sub), 1);
        assert_int_equal(fat_dir_open(&v, &d, &sub), FAT_OK);
        assert_int_equal(find_in_dir(&v, &d, "deeper", &deeper), 1);
        assert_true(deeper.is_dir);
        assert_int_equal(fat_dir_open(&v, &d, &deeper), FAT_OK);
        assert_int_equal(find_in_dir(&v, &d, "leaf.txt", &leaf), 1);
        assert_int_equal(leaf.size, 5);
        assert_int_equal(fat_file_open(&v, &f, &leaf), FAT_OK);
        memset(buf, 0, sizeof buf);
        assert_int_equal(fat_file_read(&f, 0, leaf.size, buf), FAT_OK);
        assert_memory_equal(buf, "leaf\n", 5);
        fclose(g_fp);
    }
}

/* The extents cover the file's size exactly, and each run is inside the
 * volume. mkfs.vfat writes these files contiguously, so one extent is the
 * expected answer, but the assertion is on the total rather than the count. */
static void test_fat_extents(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;
        fat_dirent e;
        fat_extent ext[32];
        size_t n = 0;
        size_t k;
        uint64_t total = 0;

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        assert_int_equal(find_in_root(&v, "big.bin", &e), 1);
        assert_int_equal(fat_file_extents(&v, &e, ext, 32, &n), FAT_OK);
        assert_true(n > 0);
        for (k = 0; k < n; k++) {
            assert_true(ext[k].len > 0);
            assert_true(ext[k].vol_off >=
                        (uint64_t)v.data_off_sectors * v.sector_size);
            total += ext[k].len;
        }
        assert_int_equal(total, BIG_SIZE);

        /* A cap too small reports what was needed rather than truncating
         * silently, same contract as every other driver. */
        assert_int_equal(fat_file_extents(&v, &e, ext, 0, &n),
                         FAT_ERR_NOSPACE);
        assert_true(n > 0);
        fclose(g_fp);
    }
}

/* Through the registry, since that is how every consumer reaches a driver:
 * fs_identify must pick FAT for these images without being told. */
static void test_fat_through_registry(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        const fs_ops *ops;
        fs_vol vol;
        fs_file file;
        fs_dirent ent;
        fs_dir scratch;
        char buf[8];

        g_fp = open_img(g_fixtures[i].path);
        ops = fs_identify(img_read, g_fp, FS_HINT_NONE);
        assert_non_null(ops);
        assert_string_equal(ops->name, "fat");
        assert_int_equal(fs_mount(&vol, ops, img_read, g_fp), FS_OK);
        assert_string_equal(fs_vol_label(&vol), "FALCON-FAT");

        /* A path the registry resolves, which exercises dir_open through the
         * vtable rather than the driver's own entry points. */
        assert_int_equal(fs_lookup(&vol, "/sub/deeper/leaf.txt", &scratch, &ent),
                         FS_OK);
        assert_false(ent.is_dir);
        assert_int_equal(ent.size, 5);
        assert_int_equal(fs_file_open(&vol, &file, &ent), FS_OK);
        assert_int_equal(fs_file_size(&file), 5);
        memset(buf, 0, sizeof buf);
        assert_int_equal(fs_file_read(&file, 0, 5, buf), FS_OK);
        assert_memory_equal(buf, "leaf\n", 5);
        fclose(g_fp);
    }
}

/* A volume that is not FAT must be declined by probe rather than mounted into
 * nonsense. The image's own middle is a convenient non-BPB. */
static void test_fat_rejects_non_fat(void **state)
{
    fat_vol v;
    uint8_t zero[512];
    FILE *f;

    (void)state;
    memset(zero, 0, sizeof zero);
    f = tmpfile();
    assert_non_null(f);
    assert_int_equal(fwrite(zero, 1, sizeof zero, f), sizeof zero);
    fflush(f);
    assert_int_equal(fat_probe(img_read, f), FAT_ERR_NOT_FAT);
    assert_int_equal(fat_mount(&v, img_read, f), FAT_ERR_NOT_FAT);
    fclose(f);
}

static void test_fat_arg_errors(void **state)
{
    fat_vol v;
    fat_dir d;
    fat_file fl;
    fat_dirent e;

    (void)state;
    memset(&e, 0, sizeof e);
    assert_int_equal(fat_mount(NULL, img_read, NULL), FAT_ERR_BAD_ARG);
    assert_int_equal(fat_mount(&v, NULL, NULL), FAT_ERR_BAD_ARG);
    assert_int_equal(fat_probe(NULL, NULL), FAT_ERR_BAD_ARG);
    assert_int_equal(fat_dir_open_root(NULL, &d), FAT_ERR_BAD_ARG);
    assert_int_equal(fat_dir_next(NULL, &e), FAT_ERR_BAD_ARG);
    /* A directory is not a byte stream and a file is not a directory. */
    e.is_dir = 1;
    assert_int_equal(fat_file_open(&v, &fl, &e), FAT_ERR_BAD_ARG);
    e.is_dir = 0;
    assert_int_equal(fat_dir_open(&v, &d, &e), FAT_ERR_BAD_ARG);
}

int run_fat_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_fat_type_and_label),
        cmocka_unit_test(test_fat_names),
        cmocka_unit_test(test_fat_hidden_entries),
        cmocka_unit_test(test_fat_read_small_file),
        cmocka_unit_test(test_fat_read_multi_cluster),
        cmocka_unit_test(test_fat_read_unaligned),
        cmocka_unit_test(test_fat_nested_directory),
        cmocka_unit_test(test_fat_extents),
        cmocka_unit_test(test_fat_through_registry),
        cmocka_unit_test(test_fat_rejects_non_fat),
        cmocka_unit_test(test_fat_arg_errors),
    };
    FILE *f = fopen(IMG12, "rb");

    if (f == NULL) {
        printf("\n[  SKIPPED ] fat fixtures: %s not found.\n"
               "             Build them with: make test-fat "
               "(needs mkfs.vfat and mtools).\n", IMG12);
        return test_skip_rc();
    }
    fclose(f);

    return cmocka_run_group_tests_name("fat", tests, NULL, NULL);
}
