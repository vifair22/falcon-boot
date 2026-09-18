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

int run_hookcmd_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_hookcmd_appends_the_hook),
        cmocka_unit_test(test_hookcmd_moves_rdinit),
        cmocka_unit_test(test_hookcmd_live_boot_nudge),
        cmocka_unit_test(test_hookcmd_before_init_args),
        cmocka_unit_test(test_hookcmd_refusals),
    };

    return cmocka_run_group_tests_name("hookcmd", tests, NULL, NULL);
}
