/* Tests for falcon-core/src/manifest/blesslist.c, the list the loader hands
 * the signing appliance. The appliance signs what it names, so the edges that
 * matter are the paths it refuses. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <manifest/blesslist.h>
#include <manifest/manifest.h>

static void test_blesslist_paths(void **state)
{
    char longp[MANIFEST_PATH_MAX + 8];

    (void)state;
    assert_true(blesslist_path_ok("ubuntu.iso"));
    assert_true(blesslist_path_ok("Linux/arch mini.ISO"));
    assert_true(blesslist_path_ok("tools/memtest.efi"));
    assert_true(blesslist_path_ok("a/b/disk.img"));
    assert_true(blesslist_path_ok("falconry/x.iso"));

    assert_false(blesslist_path_ok(NULL));
    assert_false(blesslist_path_ok(""));
    assert_false(blesslist_path_ok("/abs.iso"));
    assert_false(blesslist_path_ok("dir/"));
    assert_false(blesslist_path_ok("a//b.iso"));
    assert_false(blesslist_path_ok("./a.iso"));
    assert_false(blesslist_path_ok("a/../b.iso"));
    assert_false(blesslist_path_ok("../b.iso"));
    assert_false(blesslist_path_ok("falcon/blessed/x.iso"));
    assert_false(blesslist_path_ok("FALCON/x.iso"));
    assert_false(blesslist_path_ok("a#b.iso"));
    assert_false(blesslist_path_ok("a\\b.iso"));
    assert_false(blesslist_path_ok("tab\there.iso"));
    assert_false(blesslist_path_ok("caf\xc3\xa9.iso"));
    assert_false(blesslist_path_ok("readme.txt"));
    assert_false(blesslist_path_ok(".iso"));
    memset(longp, 'a', sizeof longp);
    memcpy(longp + MANIFEST_PATH_MAX - 4, ".iso", 5);
    assert_false(blesslist_path_ok(longp));
    memcpy(longp + MANIFEST_PATH_MAX - 5, ".iso", 5);
    assert_true(blesslist_path_ok(longp));
}

static void test_blesslist_round_trip(void **state)
{
    char buf[64], out[MANIFEST_PATH_MAX];
    size_t len = 0, pos = 0;

    (void)state;
    assert_int_equal(blesslist_add(buf, sizeof buf, &len, "a.iso"),
                     BLESSLIST_OK);
    assert_int_equal(blesslist_add(buf, sizeof buf, &len, "Linux/b.efi"),
                     BLESSLIST_OK);
    assert_int_equal(blesslist_add(buf, sizeof buf, &len, "../c.iso"),
                     BLESSLIST_ERR_PATH);
    assert_int_equal(len, 18);
    assert_memory_equal(buf, "a.iso\nLinux/b.efi\n", 18);

    assert_int_equal(blesslist_next(buf, len, &pos, out, sizeof out), 1);
    assert_string_equal(out, "a.iso");
    assert_int_equal(blesslist_next(buf, len, &pos, out, sizeof out), 1);
    assert_string_equal(out, "Linux/b.efi");
    assert_int_equal(blesslist_next(buf, len, &pos, out, sizeof out), 0);

    /* Full: refused, length untouched. */
    len = 60;
    assert_int_equal(blesslist_add(buf, sizeof buf, &len, "abcd.iso"),
                     BLESSLIST_ERR_NOSPACE);
    assert_int_equal(len, 60);
    len = 70;
    assert_int_equal(blesslist_add(buf, sizeof buf, &len, "a.iso"),
                     BLESSLIST_ERR_NOSPACE);
    assert_int_equal(blesslist_add(NULL, 1, &len, "a.iso"),
                     BLESSLIST_ERR_BAD_ARG);
    assert_int_equal(blesslist_add(buf, 1, NULL, "a.iso"),
                     BLESSLIST_ERR_BAD_ARG);
    assert_int_equal(blesslist_add(buf, 1, &len, NULL),
                     BLESSLIST_ERR_BAD_ARG);
}

/* What crossed a boot is read with the same rules, line by line. */
static void test_blesslist_hostile(void **state)
{
    static const char list[] = "ok.iso\n/etc/shadow\n\nfalcon/x.iso\n"
                               "a.iso\r\nlast.img";
    char out[MANIFEST_PATH_MAX], tiny[4];
    size_t pos = 0;

    (void)state;
    assert_int_equal(blesslist_next(list, sizeof list - 1u, &pos, out,
                                    sizeof out), 1);
    assert_string_equal(out, "ok.iso");
    assert_int_equal(blesslist_next(list, sizeof list - 1u, &pos, out,
                                    sizeof out), BLESSLIST_ERR_PATH);
    assert_int_equal(blesslist_next(list, sizeof list - 1u, &pos, out,
                                    sizeof out), BLESSLIST_ERR_PATH);
    assert_int_equal(blesslist_next(list, sizeof list - 1u, &pos, out,
                                    sizeof out), BLESSLIST_ERR_PATH);
    assert_int_equal(blesslist_next(list, sizeof list - 1u, &pos, out,
                                    sizeof out), BLESSLIST_ERR_PATH);
    assert_int_equal(blesslist_next(list, sizeof list - 1u, &pos, out,
                                    sizeof out), 1);
    assert_string_equal(out, "last.img");
    assert_int_equal(blesslist_next(list, sizeof list - 1u, &pos, out,
                                    sizeof out), 0);

    pos = 0;
    assert_int_equal(blesslist_next(list, sizeof list - 1u, &pos, tiny,
                                    sizeof tiny), BLESSLIST_ERR_PATH);
    assert_int_equal(pos, 7);
    assert_int_equal(blesslist_next(NULL, 1, &pos, out, sizeof out),
                     BLESSLIST_ERR_BAD_ARG);
    assert_int_equal(blesslist_next(list, 1, NULL, out, sizeof out),
                     BLESSLIST_ERR_BAD_ARG);
    assert_int_equal(blesslist_next(list, 1, &pos, NULL, sizeof out),
                     BLESSLIST_ERR_BAD_ARG);
    assert_int_equal(blesslist_next(list, 1, &pos, out, 0),
                     BLESSLIST_ERR_BAD_ARG);
}

int run_blesslist_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_blesslist_paths),
        cmocka_unit_test(test_blesslist_round_trip),
        cmocka_unit_test(test_blesslist_hostile),
    };

    return cmocka_run_group_tests_name("blesslist", tests, NULL, NULL);
}
