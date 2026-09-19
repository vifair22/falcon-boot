/* Tests for falcon-core/src/media/hookcmd.c, the cmdline of a hooked boot.
 *
 * The inputs are the corpus's own CD entries (tools/coverage/corpus.tsv), as
 * falcon-core/src/media/bootcfg.c reads them.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <media/hookcmd.h>

static char g_out[512];

static void expect(const char *base, const char *want)
{
    assert_int_equal(hookcmd_build(base, g_out, sizeof g_out), HOOKCMD_OK);
    assert_string_equal(g_out, want);
}

static void test_hookcmd_appends_the_hook(void **state)
{
    (void)state;
    expect("quiet rhgb root=live:CDLABEL=Fedora-WS-Live-44 rd.live.image",
           "quiet rhgb root=live:CDLABEL=Fedora-WS-Live-44 rd.live.image "
           "rdinit=/falcon/hook");
    /* Whitespace is normalised; the words and their order are not touched. */
    expect("  --- \tquiet  splash ", "--- quiet splash rdinit=/falcon/hook");
    expect("", "rdinit=/falcon/hook");
}

static void test_hookcmd_moves_rdinit(void **state)
{
    (void)state;
    expect("console=ttyS0 rdinit=/sbin/init quiet",
           "console=ttyS0 falcon.init=/sbin/init quiet rdinit=/falcon/hook");
    /* An empty rdinit= names nothing to move; it stays and is overridden by
     * the later rdinit=, which is the one the kernel takes. */
    expect("rdinit= quiet", "rdinit= quiet rdinit=/falcon/hook");
    /* A word that only begins like the key is not it. */
    expect("rdinitx=1", "rdinitx=1 rdinit=/falcon/hook");
}

static void test_hookcmd_live_boot_nudge(void **state)
{
    (void)state;
    expect("boot=live components quiet splash",
           "boot=live components quiet splash "
           "live-media=/dev/mapper/falcon-iso rdinit=/falcon/hook");
    /* The entry's own live-media= wins. */
    expect("boot=live live-media=/dev/sr0",
           "boot=live live-media=/dev/sr0 rdinit=/falcon/hook");
    /* Only the whole word triggers it. */
    expect("boot=livex", "boot=livex rdinit=/falcon/hook");
    expect("boot=casper quiet", "boot=casper quiet rdinit=/falcon/hook");
}

static void test_hookcmd_before_init_args(void **state)
{
    (void)state;
    /* Mint's entry: what follows "--" is init's. */
    expect("boot=casper quiet splash --",
           "boot=casper quiet splash rdinit=/falcon/hook --");
    expect("boot=live quiet -- single rdinit=/x",
           "boot=live quiet live-media=/dev/mapper/falcon-iso "
           "rdinit=/falcon/hook -- single rdinit=/x");
    /* Ubuntu's "---" is not the separator. */
    expect("--- quiet", "--- quiet rdinit=/falcon/hook");
    assert_int_equal(hookcmd_build("a --", g_out, 22), HOOKCMD_ERR_NOSPACE);
}

static void test_hookcmd_refusals(void **state)
{
    char small[24];
    size_t cap;

    (void)state;
    assert_int_equal(hookcmd_build(NULL, g_out, sizeof g_out),
                     HOOKCMD_ERR_BAD_ARG);
    assert_int_equal(hookcmd_build("", NULL, sizeof g_out),
                     HOOKCMD_ERR_BAD_ARG);
    assert_int_equal(hookcmd_build("", g_out, 0), HOOKCMD_ERR_BAD_ARG);

    /* Exactly enough, then one byte short, at each place the output grows. */
    assert_int_equal(hookcmd_build("", small, 20), HOOKCMD_OK);
    assert_int_equal(hookcmd_build("", small, 19), HOOKCMD_ERR_NOSPACE);
    assert_int_equal(hookcmd_build("abcdefghijklmnopqrstuvwxyz", small,
                                   sizeof small), HOOKCMD_ERR_NOSPACE);
    for (cap = 1; cap < 16; cap++) {
        assert_int_equal(hookcmd_build("rdinit=/init", small, cap),
                         HOOKCMD_ERR_NOSPACE);
        assert_int_equal(hookcmd_build("boot=live", small, cap),
                         HOOKCMD_ERR_NOSPACE);
    }
    assert_int_equal(hookcmd_build("boot=live", g_out, 42),
                     HOOKCMD_ERR_NOSPACE);
    assert_int_equal(hookcmd_build("x rdinit=/init", small, 18),
                     HOOKCMD_ERR_NOSPACE);
}

static int has(const unsigned char *buf, size_t len, const void *needle,
               size_t n)
{
    size_t i;

    for (i = 0; i + n <= len; i++) {
        if (memcmp(buf + i, needle, n) == 0)
            return 1;
    }
    return 0;
}

/* The archive: exactly the size promised, the directory then the
 * executable, and refusals. */
static void test_hookcmd_archive(void **state)
{
    static const unsigned char bin[] = { 0x7f, 'E', 'L', 'F', 1, 2, 3 };
    static unsigned char buf[1024];
    size_t need = hookcmd_archive_size(sizeof bin, 0), len = 0;

    (void)state;
    assert_true(need > 0 && need <= sizeof buf && (need & 3u) == 0);
    assert_int_equal(hookcmd_archive(bin, sizeof bin, NULL, 0, buf,
                                     sizeof buf, &len), HOOKCMD_OK);
    assert_int_equal(len, need);
    assert_memory_equal(buf, "070701", 6);
    /* newc header: mode at 14, 8 hex digits. */
    assert_memory_equal(buf + 14, "000041ED", 8);
    assert_memory_equal(buf + 110, "falcon", 7);
    assert_true(has(buf, len, "falcon/hook", 12));
    assert_true(has(buf, len, "000081ED", 8));
    assert_true(has(buf, len, bin, sizeof bin));
    assert_true(has(buf, len, "TRAILER!!!", 10));

    assert_int_equal(hookcmd_archive(bin, sizeof bin, NULL, 0, buf,
                                     need - 1u, &len), HOOKCMD_ERR_NOSPACE);
    assert_int_equal(hookcmd_archive(NULL, sizeof bin, NULL, 0, buf,
                                     sizeof buf, &len), HOOKCMD_ERR_BAD_ARG);
    assert_int_equal(hookcmd_archive(bin, 0, NULL, 0, buf, sizeof buf,
                                     &len), HOOKCMD_ERR_BAD_ARG);
    assert_int_equal(hookcmd_archive(bin, sizeof bin, NULL, 0, NULL,
                                     sizeof buf, &len), HOOKCMD_ERR_BAD_ARG);
    assert_int_equal(hookcmd_archive(bin, sizeof bin, NULL, 0, buf,
                                     sizeof buf, NULL), HOOKCMD_ERR_BAD_ARG);
}

/* With the map: a third entry, the bytes as given, and a map length with no
 * map is refused. */
static void test_hookcmd_archive_map(void **state)
{
    static const unsigned char bin[] = { 0x7f, 'E', 'L', 'F' };
    static const unsigned char map[] = "FLCNXT01-map-bytes";
    static unsigned char buf[1024];
    size_t need = hookcmd_archive_size(sizeof bin, sizeof map), len = 0;

    (void)state;
    assert_true(need > hookcmd_archive_size(sizeof bin, 0));
    assert_int_equal(hookcmd_archive(bin, sizeof bin, map, sizeof map, buf,
                                     sizeof buf, &len), HOOKCMD_OK);
    assert_int_equal(len, need);
    assert_true(has(buf, len, "falcon/map", 11));
    assert_true(has(buf, len, map, sizeof map));
    assert_int_equal(hookcmd_archive(bin, sizeof bin, NULL, 4, buf,
                                     sizeof buf, &len), HOOKCMD_ERR_BAD_ARG);
}

int run_hookcmd_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_hookcmd_appends_the_hook),
        cmocka_unit_test(test_hookcmd_moves_rdinit),
        cmocka_unit_test(test_hookcmd_live_boot_nudge),
        cmocka_unit_test(test_hookcmd_before_init_args),
        cmocka_unit_test(test_hookcmd_refusals),
        cmocka_unit_test(test_hookcmd_archive),
        cmocka_unit_test(test_hookcmd_archive_map),
    };

    return cmocka_run_group_tests_name("hookcmd", tests, NULL, NULL);
}
