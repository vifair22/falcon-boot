/* Tests for falcon-core/src/media/grubcfg.c: the grub.cfg served in place of
 * an ISO's own (D3b). The contract that matters is the length (exactly the
 * replaced file's, so no ISO record changes) and that nothing in the cmdline
 * reaches GRUB's parser as syntax. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <media/grubcfg.h>

static void entry(bootcfg_entry *e)
{
    memset(e, 0, sizeof *e);
    strcpy(e->kernel, "/live/vmlinuz");
    strcpy(e->initrd[0], "/live/initrd.img");
    e->n_initrd = 1;
}

static void test_grubcfg_shape(void **state)
{
    bootcfg_entry e;
    char out[1024];

    (void)state;
    entry(&e);
    assert_int_equal(grubcfg_write(&e, "boot=live rdinit=/falcon/hook",
                                   3945568u, 67u, 600, out, sizeof out),
                     GRUBCFG_OK);
    out[599] = '\0';   /* the file's final newline, for strstr */
    assert_non_null(strstr(out,
        "\tlinux /live/vmlinuz boot=live rdinit=/falcon/hook\n"));
    assert_non_null(strstr(out,
        "\tinitrd /live/initrd.img ($root)3945568+67\n}\n#"));
    assert_non_null(strstr(out, "set timeout=0\n"));
}

static void test_grubcfg_exact_length(void **state)
{
    bootcfg_entry e;
    char out[4096];
    size_t n;

    (void)state;
    entry(&e);
    for (n = 200; n < 4096; n += 997) {
        memset(out, 'x', sizeof out);
        assert_int_equal(grubcfg_write(&e, "quiet", 8u, 1u, n, out,
                                       sizeof out), GRUBCFG_OK);
        assert_int_equal(out[n - 1], '\n');
        assert_int_equal(out[n], 'x');   /* not a byte past it */
    }
}

static void test_grubcfg_quotes_syntax(void **state)
{
    bootcfg_entry e;
    char out[1024];

    (void)state;
    entry(&e);
    assert_int_equal(grubcfg_write(&e, "a=${x} b;c it's", 8u, 1u, 600, out,
                                   sizeof out), GRUBCFG_OK);
    out[599] = '\0';
    assert_non_null(strstr(out, " 'a=${x}' 'b;c' 'it'\\''s'\n"));
}

static void test_grubcfg_too_small(void **state)
{
    bootcfg_entry e;
    char out[64];

    (void)state;
    entry(&e);
    assert_int_equal(grubcfg_write(&e, "quiet", 8u, 1u, sizeof out, out,
                                   sizeof out), GRUBCFG_ERR_SPACE);
}

static void test_grubcfg_bad_args(void **state)
{
    bootcfg_entry e;
    char out[600];

    (void)state;
    entry(&e);
    assert_int_equal(grubcfg_write(NULL, "q", 8u, 1u, 600, out, sizeof out),
                     GRUBCFG_ERR_ARG);
    assert_int_equal(grubcfg_write(&e, NULL, 8u, 1u, 600, out, sizeof out),
                     GRUBCFG_ERR_ARG);
    assert_int_equal(grubcfg_write(&e, "q", 8u, 0u, 600, out, sizeof out),
                     GRUBCFG_ERR_ARG);
    assert_int_equal(grubcfg_write(&e, "q", 8u, 1u, 600, out, 10),
                     GRUBCFG_ERR_ARG);
    e.kernel[0] = '\0';
    assert_int_equal(grubcfg_write(&e, "q", 8u, 1u, 600, out, sizeof out),
                     GRUBCFG_ERR_ARG);
}

int run_grubcfg_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_grubcfg_shape),
        cmocka_unit_test(test_grubcfg_exact_length),
        cmocka_unit_test(test_grubcfg_quotes_syntax),
        cmocka_unit_test(test_grubcfg_too_small),
        cmocka_unit_test(test_grubcfg_bad_args),
    };

    return cmocka_run_group_tests_name("grubcfg", tests, NULL, NULL);
}
