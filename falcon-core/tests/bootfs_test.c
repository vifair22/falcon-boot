/* Tests for falcon-core/src/media/bootfs.c and fs_lookup, the path walk under
 * it: an ISO's boot configuration and boot files read through the registry,
 * the way the loader's direct boot reads them.
 *
 * The fixtures are falcon-core/tools/mk-test-iso9660.sh's, whose tree carries
 * a /boot/grub/grub.cfg naming /boot/vmlinuz and /boot/initrd.img. The Rock
 * Ridge image has the names as typed; the plain one has what an ISO9660
 * level 3 writer made of them, upper case, which the lookup must still find.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <fs/fs.h>
#include <media/bootcfg.h>
#include <media/bootfs.h>

#ifndef MEDIA_FIXTURE_DIR
#define MEDIA_FIXTURE_DIR "build"
#endif

static int g_fd = -1;
static fs_vol g_vol;
static bootfs_ctx g_ctx;
static bootcfg_result g_res;
static char g_scratch[16384];

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

static void mount_fixture(const char *name)
{
    char path[512];
    const fs_ops *ops;

    snprintf(path, sizeof path, "%s/%s", MEDIA_FIXTURE_DIR, name);
    if (g_fd >= 0)
        close(g_fd);
    g_fd = open(path, O_RDONLY);
    assert_true(g_fd >= 0);
    ops = fs_identify(img_read, &g_fd, FS_HINT_NONE);
    assert_non_null(ops);
    assert_int_equal(fs_mount(&g_vol, ops, img_read, &g_fd), FS_OK);
}

static int teardown(void **state)
{
    (void)state;
    if (g_fd >= 0)
        close(g_fd);
    g_fd = -1;
    return 0;
}

static void check_entry(void)
{
    bootcfg_src src;
    const bootcfg_entry *e;
    fs_file f;
    uint64_t size;

    bootfs_src(&g_ctx, &g_vol, &src);
    assert_int_equal(bootcfg_read(&src, BOOTCFG_CD, g_scratch,
                                  sizeof g_scratch, &g_res), BOOTCFG_OK);
    assert_true(g_res.pick < g_res.n);
    e = &g_res.entry[g_res.pick];
    assert_string_equal(e->title, "Fixture Live");
    assert_string_equal(e->kernel, "/boot/vmlinuz");
    assert_int_equal(e->n_initrd, 1);
    assert_string_equal(e->initrd[0], "/boot/initrd.img");

    assert_int_equal(bootfs_open(&g_ctx, e->kernel, &f, &size), FS_OK);
    assert_int_equal(size, 5000);
    assert_int_equal(bootfs_open(&g_ctx, e->initrd[0], &f, &size), FS_OK);
    assert_int_equal(size, 3001);
}

static void test_bootfs_rock_ridge(void **state)
{
    (void)state;
    mount_fixture("test-iso-rr.iso");
    check_entry();
}

static void test_bootfs_plain_names(void **state)
{
    (void)state;
    mount_fixture("test-iso-plain.iso");
    check_entry();
}

static int g_seen;
static int g_stop_after;

static int count_cb(void *arg, const char *name)
{
    (void)arg;
    (void)name;
    g_seen++;
    return g_seen == g_stop_after;
}

static int name_cb(void *arg, const char *name)
{
    *(int *)arg = (strcmp(name, "grub.cfg") == 0);
    return 0;
}

static void test_bootfs_callbacks(void **state)
{
    bootcfg_src src;
    char buf[8];
    size_t len = 0;
    int hit = 0;

    (void)state;
    mount_fixture("test-iso-rr.iso");
    bootfs_src(&g_ctx, &g_vol, &src);

    assert_int_equal(src.list(src.ctx, "/boot/grub", name_cb, &hit), 0);
    assert_int_equal(hit, 1);
    /* Directories are not listed, only files; a stop is honoured. */
    g_seen = 0;
    g_stop_after = 0;
    assert_int_equal(src.list(src.ctx, "/boot", count_cb, NULL), 0);
    assert_int_equal(g_seen, 2);
    g_seen = 0;
    g_stop_after = 7;
    assert_int_equal(src.list(src.ctx, "/many", count_cb, NULL), 0);
    assert_int_equal(g_seen, 7);
    assert_int_equal(src.list(src.ctx, "/readme.txt", count_cb, NULL), -1);
    assert_int_equal(src.list(src.ctx, "/nothing", count_cb, NULL), -1);

    assert_true(src.exists(src.ctx, "/README.TXT"));
    assert_false(src.exists(src.ctx, "/boot"));
    assert_false(src.exists(src.ctx, "/link-to-s1"));
    assert_false(src.exists(src.ctx, "/missing"));

    assert_int_equal(src.read(src.ctx, "/readme.txt", buf, sizeof buf, &len),
                     0);
    assert_int_equal(len, 7);
    assert_memory_equal(buf, "readme\n", 7);
    assert_int_equal(src.read(src.ctx, "/readme.txt", buf, 6, &len), -1);
    assert_int_equal(src.read(src.ctx, "/sizes/s0.bin", buf, sizeof buf, &len),
                     0);
    assert_int_equal(len, 0);
    assert_int_equal(src.read(src.ctx, "/boot", buf, sizeof buf, &len), -1);
}

static void test_fs_lookup(void **state)
{
    char longc[FS_NAME_MAX + 3];
    fs_dirent e;
    fs_file f;
    uint64_t size;

    (void)state;
    mount_fixture("test-iso-rr.iso");

    assert_int_equal(fs_lookup(&g_vol, "/nested/a/b/c/d/leaf.txt",
                               &g_ctx.walk, &e), FS_OK);
    assert_int_equal(e.size, 5);
    /* Repeated and trailing separators name the same thing. */
    assert_int_equal(fs_lookup(&g_vol, "//nested//a/", &g_ctx.walk, &e),
                     FS_OK);
    assert_true(e.is_dir);
    assert_int_equal(fs_lookup(&g_vol, "/readme.txt/x", &g_ctx.walk, &e),
                     FS_ERR_NOT_FOUND);
    assert_int_equal(fs_lookup(&g_vol, "/nested/zz", &g_ctx.walk, &e),
                     FS_ERR_NOT_FOUND);
    assert_int_equal(fs_lookup(&g_vol, "/", &g_ctx.walk, &e), FS_ERR_BAD_ARG);
    assert_int_equal(fs_lookup(&g_vol, "", &g_ctx.walk, &e), FS_ERR_BAD_ARG);
    memset(longc, 'x', sizeof longc);
    longc[0] = '/';
    longc[sizeof longc - 1] = '\0';
    assert_int_equal(fs_lookup(&g_vol, longc, &g_ctx.walk, &e),
                     FS_ERR_BAD_ARG);
    assert_int_equal(fs_lookup(NULL, "/a", &g_ctx.walk, &e), FS_ERR_BAD_ARG);
    assert_int_equal(fs_lookup(&g_vol, NULL, &g_ctx.walk, &e), FS_ERR_BAD_ARG);
    assert_int_equal(fs_lookup(&g_vol, "/a", NULL, &e), FS_ERR_BAD_ARG);
    assert_int_equal(fs_lookup(&g_vol, "/a", &g_ctx.walk, NULL),
                     FS_ERR_BAD_ARG);

    g_ctx.vol = &g_vol;
    assert_int_equal(bootfs_open(&g_ctx, "/nested", &f, &size),
                     FS_ERR_NOT_FOUND);
    assert_int_equal(bootfs_open(&g_ctx, "/nope", &f, &size),
                     FS_ERR_NOT_FOUND);
    assert_int_equal(bootfs_open(NULL, "/a", &f, &size), FS_ERR_BAD_ARG);
    assert_int_equal(bootfs_open(&g_ctx, NULL, &f, &size), FS_ERR_BAD_ARG);
    assert_int_equal(bootfs_open(&g_ctx, "/a", NULL, &size), FS_ERR_BAD_ARG);
    assert_int_equal(bootfs_open(&g_ctx, "/a", &f, NULL), FS_ERR_BAD_ARG);
}

/* A read failure at every depth of the walk comes back as the driver's
 * status, never as a match. */
static int g_fail_after;
static int g_reads;

static int failing_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    if (g_reads++ >= g_fail_after)
        return -1;
    return img_read(ctx, off, len, buf);
}

static void test_fs_lookup_io_errors(void **state)
{
    const fs_ops *ops;
    fs_dirent e;
    int rc, ok = 0;

    (void)state;
    mount_fixture("test-iso-rr.iso");
    ops = g_vol.ops;
    for (g_fail_after = 0; g_fail_after < 200 && !ok; g_fail_after++) {
        g_reads = 0;
        if (fs_mount(&g_vol, ops, failing_read, &g_fd) != FS_OK)
            continue;
        rc = fs_lookup(&g_vol, "/nested/a/b/c/d/leaf.txt", &g_ctx.walk, &e);
        assert_true(rc == FS_OK || rc < 0);
        assert_true(rc != FS_ERR_NOT_FOUND);
        ok = (rc == FS_OK);
    }
    assert_true(ok);
}

int run_bootfs_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_bootfs_rock_ridge, teardown),
        cmocka_unit_test_teardown(test_bootfs_plain_names, teardown),
        cmocka_unit_test_teardown(test_bootfs_callbacks, teardown),
        cmocka_unit_test_teardown(test_fs_lookup, teardown),
        cmocka_unit_test_teardown(test_fs_lookup_io_errors, teardown),
    };

    return cmocka_run_group_tests_name("bootfs", tests, NULL, NULL);
}
