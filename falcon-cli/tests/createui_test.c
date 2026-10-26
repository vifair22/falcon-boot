/* Tests for falcon-cli/src/mkstick/createui.c: interactive `create`'s
 * questions, answered from a script, and the command line they add up to.
 *
 * The command line is the product: it is what create then parses and runs,
 * and what the summary prints. Each case checks it word for word.
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
#include <sys/stat.h>
#include <unistd.h>

#include <mkstick/createui.h>

static drv_entry g_drives[2];
static char g_dir[64];
static char g_said[65536];

static int setup(void **state)
{
    (void)state;
    memset(g_drives, 0, sizeof g_drives);
    (void)snprintf(g_drives[0].name, sizeof g_drives[0].name, "sdb");
    (void)snprintf(g_drives[0].path, sizeof g_drives[0].path, "/dev/sdb");
    (void)snprintf(g_drives[0].model, sizeof g_drives[0].model,
                   "SanDisk Ultra");
    g_drives[0].size = (uint64_t)16 << 30;
    g_drives[0].usb = 1;
    g_drives[0].n_parts = 1;
    (void)snprintf(g_drives[0].part[0], sizeof g_drives[0].part[0], "sdb1");
    g_drives[0].part_size[0] = (uint64_t)16 << 30;
    (void)snprintf(g_drives[1].name, sizeof g_drives[1].name, "nvme0n1");
    (void)snprintf(g_drives[1].path, sizeof g_drives[1].path,
                   "/dev/nvme0n1");
    g_drives[1].size = (uint64_t)2 << 40;
    (void)snprintf(g_drives[1].busy, sizeof g_drives[1].busy,
                   "this machine's system disk");

    (void)snprintf(g_dir, sizeof g_dir, "/tmp/falcon-cui-test.XXXXXX");
    return mkdtemp(g_dir) == NULL ? -1 : 0;
}

static int teardown(void **state)
{
    char cmd[128];

    (void)state;
    (void)snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    return system(cmd) == 0 ? 0 : -1;
}

static void touch(const char *name, char *path, size_t cap)
{
    FILE *f;

    (void)snprintf(path, cap, "%s/%s", g_dir, name);
    f = fopen(path, "w");
    assert_non_null(f);
    assert_int_equal(fclose(f), 0);
}

/* Answers `script`, then builds; the words joined by spaces in words. */
static cli_status run(const char *script, size_t n_drives, cui_plan *p,
                      char *words, size_t cap)
{
    static const char *const extra[] = { "--shim", "/s" };
    char key_dir[128];
    cui_io io;
    cli_status rc;
    int i;

    (void)snprintf(key_dir, sizeof key_dir, "%s/keys", g_dir);
    io.in = fmemopen((void *)(uintptr_t)script, strlen(script), "r");
    io.out = fmemopen(g_said, sizeof g_said, "w");
    assert_non_null(io.in);
    assert_non_null(io.out);
    io.drives = g_drives;
    io.n_drives = n_drives;
    io.prog = "mkstick";
    io.key_dir = key_dir;
    rc = cui_ask(&io, p);
    (void)fclose(io.in);
    (void)fclose(io.out);
    words[0] = '\0';
    if (rc != CLI_OK)
        return rc;
    assert_int_equal(cui_build(p, extra, 2, NULL), CLI_OK);
    for (i = 0; i < p->argc; i++) {
        if (i > 0)
            (void)strncat(words, " ", cap - strlen(words) - 1u);
        (void)strncat(words, p->argv[i], cap - strlen(words) - 1u);
    }
    return rc;
}

/* Every default: the full stick, both extra loaders. */
static void test_defaults(void **state)
{
    static cui_plan p;
    char w[2048];

    (void)state;
    assert_int_equal(run("1\nsdb\n\n\n\n\n\n\n\n", 2, &p, w, sizeof w),
                     CLI_OK);
    assert_string_equal(w, "--force --shim /s /dev/sdb");
    assert_ptr_equal(p.drive, &g_drives[0]);
    assert_true(p.has_p3 && p.has_p4 && p.aa64 && p.bios);
    assert_int_equal(p.bios_argc, 2);
    assert_string_equal(p.bios_argv[0], "--no-backup");
    assert_string_equal(p.bios_argv[1], "/dev/sdb");
    /* The busy disk is listed, not numbered. */
    assert_non_null(strstr(g_said, "in use: this machine's system disk"));
    assert_non_null(strstr(g_said, "Type sdb to go on"));
}

static void test_refusals(void **state)
{
    static cui_plan p;
    char w[2048];

    (void)state;
    assert_int_equal(run("1\nsdc\n", 2, &p, w, sizeof w), CLI_ERR_PERM);
    assert_int_equal(run("1\nsdb\n1\n", 2, &p, w, sizeof w), CLI_ERR_IO);
    assert_int_equal(run("", 2, &p, w, sizeof w), CLI_ERR_IO);
    /* Only the busy disk: nothing to offer. */
    assert_int_equal(run("", 0, &p, w, sizeof w), CLI_ERR_NOTFOUND);
    /* Out of range and not a number are asked again. */
    assert_int_equal(run("2\nx\n1\nsdb\n4\n\n\n\n\n\n\n\n", 2, &p, w,
                         sizeof w), CLI_OK);
    assert_non_null(strstr(g_said, "Answer a number from 1 to 1."));
}

/* Neither p3 nor p4: the key directory, no split, no UKIs. */
static void test_no_p3(void **state)
{
    static cui_plan p;
    char w[2048];
    char want[512];

    (void)state;
    assert_int_equal(run("1\nsdb\n3\n\nn\nn\n\n\n", 2, &p, w, sizeof w),
                     CLI_OK);
    (void)snprintf(want, sizeof want, "--force --no-p3 --key-out %s/keys "
                   "--no-aa64 --shim /s /dev/sdb", g_dir);
    assert_string_equal(w, want);
    assert_int_equal(p.bios_argc, 0);
}

/* A key directory holding a key already: reused, so no MOK question. */
static void test_key_dir_reuse(void **state)
{
    static cui_plan p;
    char w[2048];
    char want[1024];
    char key[256];
    char cert[256];
    char dir[128];

    (void)state;
    (void)snprintf(dir, sizeof dir, "%s/old", g_dir);
    assert_int_equal(mkdir(dir, 0700), 0);
    touch("old/falcon-mok.key", key, sizeof key);
    touch("old/falcon-mok.crt", cert, sizeof cert);

    /* Declined first: asked for another directory. */
    (void)snprintf(want, sizeof want, "1\nsdb\n3\n%s\nn\n%s\ny\n\n\n\n",
                   dir, dir);
    assert_int_equal(run(want, 2, &p, w, sizeof w), CLI_OK);
    (void)snprintf(want, sizeof want, "--force --no-p3 --key-out %s --key %s "
                   "--cert %s --shim /s /dev/sdb", dir, key, cert);
    assert_string_equal(w, want);
    assert_null(strstr(g_said, "The MOK is the key"));
}

/* The shelf only, a reused MOK, a UKI and every advanced answer. */
static void test_shelf_advanced(void **state)
{
    static cui_plan p;
    char script[2048];
    char w[2048];
    char want[2048];
    char key[256];
    char cert[256];
    char uki[256];

    (void)state;
    touch("mok.key", key, sizeof key);
    touch("mok.crt", cert, sizeof cert);
    touch("my.efi", uki, sizeof uki);
    (void)snprintf(script, sizeof script,
                   "1\nsdb\n2\n"
                   "y\nn\n"
                   "2\n/missing.key\n%s\n%s\n"
                   "/missing.efi\n%s\n\n"
                   "y\n1K\nbig\n300M\nntfs\nfat32\n200M\nxfs\nbtrfs\n",
                   key, cert, uki);
    assert_int_equal(run(script, 2, &p, w, sizeof w), CLI_OK);
    (void)snprintf(want, sizeof want, "--force --no-p4 --p1-size 300M "
                   "--p2-fs fat32 --p3-size 200M --p3-fs btrfs --key %s "
                   "--cert %s --uki %s --shim /s /dev/sdb", key, cert, uki);
    assert_string_equal(w, want);
    assert_int_equal(p.bios_argc, 0);
    assert_non_null(strstr(g_said, "There is no file at /missing.key."));
    assert_non_null(strstr(g_said, "There is no file at /missing.efi."));
    assert_non_null(strstr(g_said, "It needs 16.0 MiB or more."));
    assert_non_null(strstr(g_said, "Answer a size: 64M, 2G."));
    assert_non_null(strstr(g_said, "Answer one of the names listed."));
    assert_non_null(strstr(g_said, "xfs needs a p3 of 320M or more."));
}

/* p2's size out of the split; out of range and not a size, asked again.
 * The space split is what the advanced sizes leave. */
static void test_split(void **state)
{
    static cui_plan p;
    char w[2048];

    (void)state;
    assert_int_equal(run("1\nsdb\n1\n\n\n\n\n\n1K\n20G\nlots\n4G\n", 2, &p,
                         w, sizeof w), CLI_OK);
    assert_string_equal(w, "--force --iso-size 4G --shim /s /dev/sdb");
    assert_non_null(strstr(g_said, "p2 can be from"));
    assert_non_null(strstr(g_said, "Answer a size (32G) or half."));
    assert_int_equal(run("1\nsdb\n\n\n\n\n\n\nhalf\n", 2, &p, w, sizeof w),
                     CLI_OK);
    assert_string_equal(w, "--force --shim /s /dev/sdb");
    assert_non_null(strstr(g_said, "about 14.9 GiB"));
    /* A smaller p3 leaves more. */
    assert_int_equal(run("1\nsdb\n\n\n\n\n\ny\n\n\n100M\n\n15G\n", 2,
                         &p, w, sizeof w), CLI_OK);
    assert_non_null(strstr(g_said, "about 15.8 GiB"));
    assert_string_equal(w, "--force --iso-size 15G --p3-size 100M --shim /s "
                        "/dev/sdb");
}

/* The summary, the two command lines, quoted, and the answer. */
static void test_confirm(void **state)
{
    static cui_plan p;
    static const char *const extra[] = { "--cn", "my stick's key" };
    char w[2048];
    cui_io io;

    (void)state;
    assert_int_equal(run("1\nsdb\n\n\n\n\n\n\n\n", 2, &p, w, sizeof w),
                     CLI_OK);
    assert_int_equal(cui_build(&p, extra, 2, "/st 3"), CLI_OK);

    io.in = fmemopen((void *)(uintptr_t)"maybe\ny\n", 8, "r");
    io.out = fmemopen(g_said, sizeof g_said, "w");
    io.drives = g_drives;
    io.n_drives = 2;
    io.prog = "mkstick";
    io.key_dir = "/k";
    assert_int_equal(cui_confirm(&io, &p), 1);
    (void)fclose(io.in);
    (void)fclose(io.out);
    assert_non_null(strstr(g_said, "  mkstick create --force --cn "
                           "'my stick'\\''s key' /dev/sdb\n"));
    assert_non_null(strstr(g_said, "  mkstick bios --no-backup --stage3 "
                           "'/st 3' /dev/sdb\n"));
    assert_non_null(strstr(g_said, "  p4       personal OS, LUKS2, half the "
                           "rest\n"));
    assert_non_null(strstr(g_said, "Answer y or n."));

    /* The default is no, and so is the end of input. */
    io.in = fmemopen((void *)(uintptr_t)"\n", 1, "r");
    io.out = fmemopen(g_said, sizeof g_said, "w");
    assert_int_equal(cui_confirm(&io, &p), 0);
    (void)fclose(io.in);
    (void)fclose(io.out);
    io.in = fmemopen((void *)(uintptr_t)"x", 1, "r");
    io.out = fmemopen(g_said, sizeof g_said, "w");
    assert_int_equal(cui_confirm(&io, &p), 0);
    (void)fclose(io.in);
    (void)fclose(io.out);
}

static void test_first_boot(void **state)
{
    static cui_plan p;
    FILE *out;

    (void)state;
    memset(&p, 0, sizeof p);
    p.bios = 1;
    out = fmemopen(g_said, sizeof g_said, "w");
    assert_non_null(out);
    cui_first_boot(out, &p);
    (void)fclose(out);
    assert_non_null(strstr(g_said, "then FALCONESP, then falcon-boot.cer."));
    assert_non_null(strstr(g_said, "or on a BIOS machine"));
    assert_null(strstr(g_said, "enrolled this MOK"));

    (void)snprintf(p.key, sizeof p.key, "/k");
    p.bios = 0;
    out = fmemopen(g_said, sizeof g_said, "w");
    cui_first_boot(out, &p);
    (void)fclose(out);
    assert_non_null(strstr(g_said, "enrolled this MOK"));
}

int run_createui_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_defaults),
        cmocka_unit_test(test_refusals),
        cmocka_unit_test(test_no_p3),
        cmocka_unit_test(test_key_dir_reuse),
        cmocka_unit_test(test_shelf_advanced),
        cmocka_unit_test(test_split),
        cmocka_unit_test(test_confirm),
        cmocka_unit_test(test_first_boot),
    };

    return cmocka_run_group_tests_name("mkstick interactive create", tests,
                                       setup, teardown);
}
