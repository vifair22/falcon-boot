/* Tests for the parts of falcon-cli/src/sign/util.c the hooked wrap uses:
 * where --cmdline-extra lands, and the initrd staging helpers that append
 * bytes and pad to the kernel's 4-byte alignment between archives. */

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

#include <sign/sign.h>

static void insert(const char *base, const char *extra, const char *want)
{
    char buf[128];

    snprintf(buf, sizeof buf, "%s", base);
    assert_int_equal(sgn_cmdline_insert(buf, sizeof buf, extra), SGN_OK);
    assert_string_equal(buf, want);
}

static void test_cmdline_insert(void **state)
{
    char small[16] = "quiet --";

    (void)state;
    insert("quiet splash", "console=ttyS0", "quiet splash console=ttyS0");
    insert("", "a=1", "a=1");
    insert("quiet splash --", "a=1", "quiet splash a=1 --");
    insert("-- x", "a=1", "a=1 -- x");
    /* Not the separator: "---", and "--" inside a word. */
    insert("--- quiet", "a=1", "--- quiet a=1");
    insert("x=--y z--", "a=1", "x=--y z-- a=1");
    insert("x=--y -- z", "a=1", "x=--y a=1 -- z");
    /* Full: refused, untouched. */
    assert_int_equal(sgn_cmdline_insert(small, sizeof small, "abcdefgh"),
                     SGN_ERR_NOSPACE);
    assert_string_equal(small, "quiet --");
    assert_int_equal(sgn_cmdline_insert(small, sizeof small, "abcdef"),
                     SGN_OK);
    assert_string_equal(small, "quiet abcdef --");
}

static long file_size(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void test_append_and_pad(void **state)
{
    char path[] = "/tmp/falcon-cli-util-XXXXXX";
    int fd = mkstemp(path);
    FILE *f;
    unsigned char buf[16];

    (void)state;
    assert_true(fd >= 0);
    close(fd);

    assert_int_equal(sgn_pad4_file(path), SGN_OK);
    assert_int_equal(file_size(path), 0);
    assert_int_equal(sgn_append_bytes(path, "abcde", 5), SGN_OK);
    assert_int_equal(sgn_pad4_file(path), SGN_OK);
    assert_int_equal(file_size(path), 8);
    assert_int_equal(sgn_pad4_file(path), SGN_OK);
    assert_int_equal(file_size(path), 8);
    assert_int_equal(sgn_append_bytes(path, "", 0), SGN_OK);
    assert_int_equal(sgn_append_bytes(path, "xyz", 3), SGN_OK);
    f = fopen(path, "rb");
    assert_non_null(f);
    assert_int_equal(fread(buf, 1, sizeof buf, f), 11);
    fclose(f);
    assert_memory_equal(buf, "abcde\0\0\0xyz", 11);
    unlink(path);

    /* A path that cannot be opened or stat'd. */
    assert_int_equal(sgn_append_bytes("/nonexistent/dir/f", "a", 1),
                     SGN_ERR_IO);
    assert_int_equal(sgn_pad4_file("/nonexistent/dir/f"), SGN_ERR_IO);
}

int run_util_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_cmdline_insert),
        cmocka_unit_test(test_append_and_pad),
    };

    return cmocka_run_group_tests_name("sign util", tests, NULL, NULL);
}
