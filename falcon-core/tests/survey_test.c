/* Tests for falcon-core/src/media/survey.c.
 *
 * Two halves. The view, the sort and the path arithmetic are pure and run on
 * a list built here, so every shape can be asked for directly: a folder
 * holding only folders, a folder holding both, two files whose names differ
 * only in case, a path that does not fit.
 *
 * The walk runs against build/test-exfat.img, which
 * falcon-core/tools/mk-test-exfat.sh writes for this scanner: images at the
 * root and one and three levels down, a directory past the depth cap, a
 * dotted directory, a directory with no image under it, and a file with the
 * wrong extension. Skipped with a message when the image is absent.
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

#include <fs/fs.h>
#include <media/survey.h>

#ifndef FIXTURE_IMG
#define FIXTURE_IMG "build/test-exfat.img"
#endif

#define TEST_ENTRIES 64u
#define TEST_ROWS    64u

static survey_entry g_entries[TEST_ENTRIES];
static survey_list  g_list;
static survey_row   g_rows[TEST_ROWS];

/* ---- helpers ---- */

static void wide(uint16_t *dst, const char *s)
{
    size_t i;

    for (i = 0; s[i] != '\0'; i++)
        dst[i] = (uint16_t)(unsigned char)s[i];
    dst[i] = 0;
}

static void narrow(char *dst, size_t cap, const uint16_t *s)
{
    size_t i;

    for (i = 0; s[i] != 0 && i + 1u < cap; i++)
        dst[i] = (s[i] < 128u) ? (char)s[i] : '?';
    dst[i] = '\0';
}

/* Append an entry as the walk would have: `path` is the whole label and
 * dir_len is taken from the last separator in it. */
static void add(const char *path)
{
    survey_entry *e = &g_entries[g_list.count++];
    size_t i, last = 0;

    memset(e, 0, sizeof *e);
    wide(e->label, path);
    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/')
            last = i + 1u;
    }
    e->dir_len = last;
    e->shape = SURVEY_SHAPE_MEDIA;
    e->media = SURVEY_MEDIA_CD;
    wide(e->ent.name, path + last);
}

static size_t view(const char *cur)
{
    uint16_t w[SURVEY_PATH_MAX];

    wide(w, cur);
    return survey_view(&g_list, w, strlen(cur), g_rows, TEST_ROWS);
}

static void assert_row(size_t i, survey_row_kind kind, const char *label)
{
    char buf[256];

    narrow(buf, sizeof buf, g_rows[i].label);
    assert_int_equal(g_rows[i].kind, kind);
    assert_string_equal(buf, label);
}

/* ---- the view ---- */

static void test_view_root(void **state)
{
    (void)state;
    survey_init(&g_list, g_entries, TEST_ENTRIES, SURVEY_ACCEPT_ISO);
    add("arch.iso");
    add("Linux/fedora.iso");
    add("Linux/rescue/gparted.iso");
    add("Windows/win11.iso");
    survey_sort(&g_list);

    /* Folders first, then the files directly in this folder. A folder shows
     * once however many entries are under it. */
    assert_int_equal(view(""), 3);
    assert_row(0, SURVEY_ROW_DIR, "Linux/");
    assert_row(1, SURVEY_ROW_DIR, "Windows/");
    assert_row(2, SURVEY_ROW_FILE, "arch.iso");
}

static void test_view_descend(void **state)
{
    uint16_t cur[SURVEY_PATH_MAX];
    size_t cur_len = 0;
    char buf[256];

    (void)state;
    survey_init(&g_list, g_entries, TEST_ENTRIES, SURVEY_ACCEPT_ISO);
    add("Linux/fedora.iso");
    add("Linux/ubuntu.iso");
    add("Linux/rescue/gparted.iso");
    survey_sort(&g_list);

    cur[0] = 0;
    assert_int_equal(view(""), 1);
    assert_row(0, SURVEY_ROW_DIR, "Linux/");

    /* The row's label is the folder name with its separator, which is what
     * the path wants appended. */
    assert_int_equal(survey_enter(cur, &cur_len, g_rows[0].label), 0);
    assert_int_equal(cur_len, 6);
    narrow(buf, sizeof buf, cur);
    assert_string_equal(buf, "Linux/");

    assert_int_equal(view("Linux/"), 3);
    assert_row(0, SURVEY_ROW_DIR, "rescue/");
    assert_row(1, SURVEY_ROW_FILE, "fedora.iso");
    assert_row(2, SURVEY_ROW_FILE, "ubuntu.iso");

    /* A folder whose only contents are another folder. */
    assert_int_equal(survey_enter(cur, &cur_len, g_rows[0].label), 0);
    assert_int_equal(view("Linux/rescue/"), 1);
    assert_row(0, SURVEY_ROW_FILE, "gparted.iso");

    survey_up(cur, &cur_len);
    narrow(buf, sizeof buf, cur);
    assert_string_equal(buf, "Linux/");
    survey_up(cur, &cur_len);
    assert_int_equal(cur_len, 0);
    narrow(buf, sizeof buf, cur);
    assert_string_equal(buf, "");
    /* Up from the root is where the caller stops, not where this wanders. */
    survey_up(cur, &cur_len);
    assert_int_equal(cur_len, 0);
}

/* Two folders whose names differ only in case are two folders. The sort
 * folds case so they land together; the view must not fold it and merge
 * them, because the walk took both names off the same volume. */
static void test_view_case_is_a_difference(void **state)
{
    (void)state;
    survey_init(&g_list, g_entries, TEST_ENTRIES, SURVEY_ACCEPT_ISO);
    add("linux/a.iso");
    add("Linux/b.iso");
    survey_sort(&g_list);

    assert_int_equal(view(""), 2);
    /* One row each, not one row with both under it. */
    assert_int_equal(g_rows[0].kind, SURVEY_ROW_DIR);
    assert_int_equal(g_rows[1].kind, SURVEY_ROW_DIR);
    assert_int_equal(view("Linux/"), 1);
    assert_int_equal(view("linux/"), 1);
}

static void test_sort_is_case_insensitive(void **state)
{
    char buf[256];

    (void)state;
    survey_init(&g_list, g_entries, TEST_ENTRIES, SURVEY_ACCEPT_ISO);
    add("zulu.iso");
    add("Alpha.iso");
    add("mike.iso");
    survey_sort(&g_list);

    narrow(buf, sizeof buf, g_entries[0].label);
    assert_string_equal(buf, "Alpha.iso");
    narrow(buf, sizeof buf, g_entries[1].label);
    assert_string_equal(buf, "mike.iso");
    narrow(buf, sizeof buf, g_entries[2].label);
    assert_string_equal(buf, "zulu.iso");
}

static void test_view_bounds(void **state)
{
    uint16_t cur[SURVEY_PATH_MAX];
    size_t cur_len = 0;
    uint16_t big[SURVEY_PATH_MAX];
    size_t i;

    (void)state;
    survey_init(&g_list, g_entries, TEST_ENTRIES, SURVEY_ACCEPT_ISO);
    add("a.iso");
    add("b.iso");
    add("c.iso");
    survey_sort(&g_list);

    /* A cap shorter than the rows available stops, it does not overrun. */
    {
        uint16_t w[2];

        w[0] = 0;
        assert_int_equal(survey_view(&g_list, w, 0, g_rows, 2), 2);
    }

    /* Nothing under a folder that does not exist. */
    assert_int_equal(view("Nope/"), 0);

    /* The boundary both ways: a name and its terminator that exactly fill
     * the buffer go in, and the next one is refused rather than truncated. */
    for (i = 0; i + 1u < SURVEY_PATH_MAX; i++)
        big[i] = (uint16_t)'x';
    big[SURVEY_PATH_MAX - 1u] = 0;
    cur[0] = 0;
    assert_int_equal(survey_enter(cur, &cur_len, big), 0);
    assert_int_equal(cur_len, SURVEY_PATH_MAX - 1u);
    assert_int_equal(survey_enter(cur, &cur_len, big), -1);
    assert_int_equal(cur_len, SURVEY_PATH_MAX - 1u);
    cur[0] = 0;
    cur_len = 0;

    assert_int_equal(survey_view(NULL, cur, 0, g_rows, TEST_ROWS), 0);
    assert_int_equal(survey_enter(NULL, &cur_len, big), -1);
    survey_up(NULL, &cur_len);
}

/* A list with no storage behind it answers empty rather than writing. */
static void test_no_storage(void **state)
{
    survey_list l;

    (void)state;
    survey_init(&l, NULL, 8, SURVEY_ACCEPT_ISO);
    assert_int_equal(l.cap, 0);
    assert_int_equal(survey_volume(&l, NULL, 0), -1);
    survey_sort(&l);
    assert_int_equal(survey_view(&l, NULL, 0, g_rows, TEST_ROWS), 0);
    survey_init(NULL, g_entries, 8, 0);
}

/* ---- the walk, over the exFAT fixture ---- */

static int g_fd = -1;

static int img_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    int fd = *(const int *)ctx;
    uint8_t *p = buf;

    while (len > 0) {
        ssize_t n = pread(fd, p, (size_t)len, (off_t)off);

        if (n <= 0)
            return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

static int found(const char *path)
{
    char buf[SURVEY_PATH_MAX];
    size_t i;

    for (i = 0; i < g_list.count; i++) {
        narrow(buf, sizeof buf, g_entries[i].label);
        if (strcmp(buf, path) == 0)
            return 1;
    }
    return 0;
}

static int fixture_setup(void **state)
{
    (void)state;
    g_fd = open(FIXTURE_IMG, O_RDONLY);
    return 0;
}

static int fixture_teardown(void **state)
{
    (void)state;
    if (g_fd >= 0)
        (void)close(g_fd);
    g_fd = -1;
    return 0;
}

static void test_walk_fixture(void **state)
{
    static fs_vol vol;
    const fs_ops *ops;

    (void)state;
    if (g_fd < 0) {
        printf("  (skipped: no %s; run make test-fixtures)\n", FIXTURE_IMG);
        return;
    }
    ops = fs_identify(img_read, &g_fd, FS_HINT_NONE);
    assert_non_null(ops);
    assert_int_equal(fs_mount(&vol, ops, img_read, &g_fd), FS_OK);

    survey_init(&g_list, g_entries, TEST_ENTRIES,
                SURVEY_ACCEPT_ISO | SURVEY_ACCEPT_IMG);
    assert_int_equal(survey_volume(&g_list, &vol, 3), 0);
    survey_sort(&g_list);
    assert_true(g_list.count > 0);

    /* The root, one level down, and the level the depth cap still reaches. */
    assert_true(found("archlinux-fake.iso"));
    assert_true(found("Linux/ubuntu-fake.iso"));
    assert_true(found("Linux/rescue/gparted-fake.iso"));
    assert_true(found("deep/a/b/justdeepenough-fake.iso"));

    /* Past the cap, wrong extension, and a dotted directory. */
    assert_false(found("deep/a/b/c/toodeep-fake.iso"));
    assert_false(found("notaniso.txt"));
    assert_false(found(".hiddendir/hidden-fake.iso"));

    /* The volume index is recorded and never interpreted. */
    assert_int_equal(g_entries[0].vol, 3);

    /* notes/ holds one .txt and nothing else, so it contributes no entry and
     * the folder view never offers it. */
    {
        size_t n = view("");
        size_t i;

        for (i = 0; i < n; i++) {
            char buf[256];

            narrow(buf, sizeof buf, g_rows[i].label);
            assert_string_not_equal(buf, "notes/");
        }
        assert_true(n > 0);
    }
}

/* .efi is a payload, not media, and a caller that did not ask for it does
 * not get it: falcon-bios passes ISO|IMG because nothing on a BIOS machine
 * runs a PE. */
static void test_accept_mask(void **state)
{
    static fs_vol vol;
    const fs_ops *ops;

    (void)state;
    if (g_fd < 0) {
        printf("  (skipped: no %s; run make test-fixtures)\n", FIXTURE_IMG);
        return;
    }
    ops = fs_identify(img_read, &g_fd, FS_HINT_NONE);
    assert_non_null(ops);
    assert_int_equal(fs_mount(&vol, ops, img_read, &g_fd), FS_OK);

    survey_init(&g_list, g_entries, TEST_ENTRIES, SURVEY_ACCEPT_IMG);
    assert_int_equal(survey_volume(&g_list, &vol, 0), 0);
    {
        size_t i;

        for (i = 0; i < g_list.count; i++) {
            assert_int_equal(g_entries[i].shape, SURVEY_SHAPE_MEDIA);
            assert_int_equal(g_entries[i].media, SURVEY_MEDIA_DISK);
        }
    }
    assert_false(found("archlinux-fake.iso"));
}

int run_survey_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_view_root),
        cmocka_unit_test(test_view_descend),
        cmocka_unit_test(test_view_case_is_a_difference),
        cmocka_unit_test(test_sort_is_case_insensitive),
        cmocka_unit_test(test_view_bounds),
        cmocka_unit_test(test_no_storage),
        cmocka_unit_test_setup_teardown(test_walk_fixture, fixture_setup,
                                        fixture_teardown),
        cmocka_unit_test_setup_teardown(test_accept_mask, fixture_setup,
                                        fixture_teardown),
    };

    return cmocka_run_group_tests_name("media survey", tests, NULL, NULL);
}
