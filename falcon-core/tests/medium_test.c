/* Tests for falcon-core/src/media/medium.c: a medium as the set of
 * filesystems in it (docs/notes/iso-reader-route.md stage C).
 *
 * These run against the ISO and FAT fixtures the other media suites build,
 * because the thing being tested is assembly rather than parsing: the parsers
 * have their own suites, and what can go wrong here is the precedence order,
 * the offset shim, and a medium that is none of the expected shapes.
 *
 * The offset shim is the part worth being careful about. Every volume past
 * the first reads through a base offset, and a shim that dropped or
 * mis-applied it would still mount, because a FAT volume mounted at the wrong
 * base usually fails its BPB check rather than reading somebody else's bytes
 * as a directory. Reading a file's contents back is what actually proves the
 * offset is being applied.
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

#include <media/medium.h>
#include <fs/fs.h>

#define ISO_RR   "build/test-iso-rr.iso"
#define ISO_PLAIN "build/test-iso-plain.iso"
#define IMG_FAT12 "build/test-fat12.img"
#define IMG_GPT   "build/test/payload-gpt.img"

/* The medium is ~17 KB and must not move once opened, which is exactly the
 * reason it is a static here rather than a local. */
static medium g_m;
static FILE  *g_fp;

static int img_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    FILE *f = ctx;

    if (fseek(f, (long)off, SEEK_SET) != 0)
        return -1;
    return fread(buf, 1, (size_t)len, f) == len ? 0 : -1;
}

static uint64_t file_size(FILE *f)
{
    long end;

    if (fseek(f, 0, SEEK_END) != 0)
        return 0;
    end = ftell(f);
    return end < 0 ? 0 : (uint64_t)end;
}

static FILE *open_img(const char *path)
{
    FILE *f = fopen(path, "rb");

    assert_non_null(f);
    return f;
}

static int have(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

/* An ISO yields its name space, and every volume it yields is mounted and
 * usable through the registry. */
static void test_medium_iso(void **state)
{
    unsigned i;
    unsigned kinds = 0;

    (void)state;
    g_fp = open_img(ISO_RR);
    assert_int_equal(medium_open(&g_m, img_read, g_fp, file_size(g_fp)),
                     MEDIUM_OK);
    assert_true(medium_count(&g_m) >= 1u);

    for (i = 0; i < medium_count(&g_m); i++) {
        fs_vol *v = medium_vol_at(&g_m, i);

        assert_non_null(v);
        /* Mounted means the vtable answers, which is what a consumer needs
         * rather than any particular driver having been chosen. */
        assert_non_null(v->ops);
        kinds |= 1u << (unsigned)medium_kind_at(&g_m, i);
    }
    /* The ISO9660 tree must be among them; the fixture has no UDF. */
    assert_true((kinds & (1u << (unsigned)MEDIUM_VOL_ISO9660)) != 0);
    fclose(g_fp);
}

/* Reading a real file back through a volume the medium mounted, which is the
 * assembly working end to end rather than merely a successful mount. */
static void test_medium_reads_through_volume(void **state)
{
    unsigned i;
    int found = 0;

    (void)state;
    g_fp = open_img(ISO_RR);
    assert_int_equal(medium_open(&g_m, img_read, g_fp, file_size(g_fp)),
                     MEDIUM_OK);
    /* Iterating the root is what a consumer does; fs_lookup has no dirent to
     * give for "/" because the root is not an entry in anything. */
    for (i = 0; i < medium_count(&g_m) && !found; i++) {
        fs_vol *v = medium_vol_at(&g_m, i);
        fs_dir d;
        fs_dirent ent;

        if (fs_dir_open_root(v, &d) != FS_OK)
            continue;
        while (fs_dir_next(&d, &ent) == 1) {
            found = 1;
            break;
        }
    }
    assert_int_equal(found, 1);
    fclose(g_fp);
}

/* A bare filesystem image is not an ISO: no name space at offset zero and no
 * catalog, so the whole-medium fallback is what answers. */
static void test_medium_whole_image(void **state)
{
    fs_vol *v;
    fs_dir scratch;
    fs_dirent ent;

    (void)state;
    if (!have(IMG_FAT12)) {
        print_message("  (no FAT fixture; skipping)\n");
        return;
    }
    g_fp = open_img(IMG_FAT12);
    assert_int_equal(medium_open(&g_m, img_read, g_fp, file_size(g_fp)),
                     MEDIUM_OK);
    assert_int_equal(medium_count(&g_m), 1);
    assert_int_equal((int)medium_kind_at(&g_m, 0), (int)MEDIUM_VOL_WHOLE);
    assert_int_equal(medium_base_at(&g_m, 0), 0);
    assert_null(medium_boot_vol(&g_m));

    /* The fixture's tree, reached through the medium rather than the driver. */
    v = medium_vol_at(&g_m, 0);
    assert_non_null(v);
    assert_int_equal(fs_lookup(v, "/sub/deeper/leaf.txt", &scratch, &ent),
                     FS_OK);
    assert_int_equal(ent.size, 5);
    fclose(g_fp);
}

/* The offset shim, proven by content rather than by a successful mount.
 *
 * The FAT fixture is copied into the middle of a larger file and the medium is
 * opened on the whole thing with a base that is not zero. A shim that dropped
 * the base would not mount at all; one that applied it to the mount but not to
 * later reads would mount and then return the wrong bytes. Reading a known
 * file back catches both.
 */
static void test_medium_offset_shim(void **state)
{
    static const uint64_t pad = 1u << 20;   /* 1 MiB of leading rubbish */
    medium_src src;
    fs_vol vol;
    fs_dir scratch;
    fs_dirent ent;
    fs_file file;
    const fs_ops *ops;
    char buf[8];
    FILE *big;
    FILE *src_f;
    unsigned char chunk[65536];
    size_t got;
    uint64_t i;

    (void)state;
    if (!have(IMG_FAT12)) {
        print_message("  (no FAT fixture; skipping)\n");
        return;
    }
    big = tmpfile();
    assert_non_null(big);
    /* Leading rubbish that is not zeroes, so a dropped offset reads something
     * that looks like data rather than something that looks like a hole. */
    memset(chunk, 0x5A, sizeof chunk);
    for (i = 0; i < pad; i += sizeof chunk)
        assert_int_equal(fwrite(chunk, 1, sizeof chunk, big), sizeof chunk);
    src_f = open_img(IMG_FAT12);
    while ((got = fread(chunk, 1, sizeof chunk, src_f)) > 0)
        assert_int_equal(fwrite(chunk, 1, got, big), got);
    fclose(src_f);
    fflush(big);

    /* Drive the shim directly, which is what medium_open does internally for
     * an El Torito boot image at a nonzero LBA. */
    src.parent = img_read;
    src.parent_ctx = big;
    src.base = pad;
    ops = fs_by_name("fat");
    assert_non_null(ops);
    assert_int_equal(fs_mount(&vol, ops, medium_src_read, &src), FS_OK);
    assert_int_equal(fs_lookup(&vol, "/sub/deeper/leaf.txt", &scratch, &ent),
                     FS_OK);
    assert_int_equal(fs_file_open(&vol, &file, &ent), FS_OK);
    memset(buf, 0, sizeof buf);
    assert_int_equal(fs_file_read(&file, 0, 5, buf), FS_OK);
    assert_memory_equal(buf, "leaf\n", 5);
    fclose(big);
}

/* A partitioned disk image. Nothing mounts at offset zero and there is no
 * boot catalog, so the GPT is what answers, and the partition it finds is at
 * a nonzero base like an El Torito image is. This is the shape the loader's
 * disk flavour serves and the one the firmware's partition driver finds
 * today. */
static void test_medium_gpt_image(void **state)
{
    fs_vol *v;
    fs_dir d;
    fs_dirent ent;
    int found = 0;

    (void)state;
    if (!have(IMG_GPT)) {
        print_message("  (no GPT fixture; build with make test-image)\n");
        return;
    }
    g_fp = open_img(IMG_GPT);
    assert_int_equal(medium_open(&g_m, img_read, g_fp, file_size(g_fp)),
                     MEDIUM_OK);
    assert_true(medium_count(&g_m) >= 1u);
    assert_int_equal((int)medium_kind_at(&g_m, 0), (int)MEDIUM_VOL_PART);
    /* A partition does not start at byte zero, so this also says the base
     * reached the volume rather than being computed and dropped. */
    assert_true(medium_base_at(&g_m, 0) > 0);

    v = medium_vol_at(&g_m, 0);
    assert_non_null(v);
    assert_int_equal(fs_dir_open_root(v, &d), FS_OK);
    while (fs_dir_next(&d, &ent) == 1) {
        found = 1;
        break;
    }
    assert_int_equal(found, 1);
    fclose(g_fp);
}

/* A file that is an image of nothing is an ordinary answer, not damage. */
static void test_medium_nothing(void **state)
{
    unsigned char zero[65536];
    FILE *f;

    (void)state;
    memset(zero, 0, sizeof zero);
    f = tmpfile();
    assert_non_null(f);
    assert_int_equal(fwrite(zero, 1, sizeof zero, f), sizeof zero);
    fflush(f);
    assert_int_equal(medium_open(&g_m, img_read, f, sizeof zero),
                     MEDIUM_ERR_NOTHING);
    assert_int_equal(medium_count(&g_m), 0);
    assert_null(medium_vol_at(&g_m, 0));
    assert_null(medium_boot_vol(&g_m));
    fclose(f);
}

static void test_medium_arg_errors(void **state)
{
    (void)state;
    assert_int_equal(medium_open(NULL, img_read, NULL, 0), MEDIUM_ERR_ARG);
    assert_int_equal(medium_open(&g_m, NULL, NULL, 0), MEDIUM_ERR_ARG);
    assert_int_equal(medium_count(&g_m), 0);
    assert_int_equal(medium_count(NULL), 0);
    assert_null(medium_vol_at(NULL, 0));
    assert_null(medium_boot_vol(NULL));
    assert_int_equal(medium_base_at(NULL, 0), 0);
    assert_int_equal((int)medium_kind_at(NULL, 0), (int)MEDIUM_VOL_WHOLE);
}

int run_medium_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_medium_iso),
        cmocka_unit_test(test_medium_reads_through_volume),
        cmocka_unit_test(test_medium_whole_image),
        cmocka_unit_test(test_medium_offset_shim),
        cmocka_unit_test(test_medium_gpt_image),
        cmocka_unit_test(test_medium_nothing),
        cmocka_unit_test(test_medium_arg_errors),
    };

    if (!have(ISO_RR)) {
        printf("\n[  SKIPPED ] medium: %s not found.\n"
               "             Build it with: make test-iso9660.\n", ISO_RR);
        return test_skip_rc();
    }
    return cmocka_run_group_tests_name("medium", tests, NULL, NULL);
}
