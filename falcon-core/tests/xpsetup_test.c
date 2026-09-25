/* Tests for falcon-core/src/media/xpsetup.c: the txtsetup.sif edit that makes
 * Windows 2000/XP text-mode setup load falcon-vdisk as a boot-start driver.
 * What matters: the driver's line is added at the head of each of the three
 * sections, in the file's CRLF form, and a missing section is refused. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <media/xpsetup.h>

/* A txtsetup.sif in miniature: the three sections the edit touches, each with
 * one existing entry, plus an unrelated one, all CRLF. */
static const char SIF[] =
    "[Version]\r\n"
    "Signature = \"$Windows NT$\"\r\n"
    "\r\n"
    "[SCSI.Load]\r\n"
    "atapi = atapi.sys,4\r\n"
    "\r\n"
    "[SCSI]\r\n"
    "atapi = \"Standard IDE/ESDI Hard Disk Controller\"\r\n"
    "\r\n"
    "[SourceDisksFiles]\r\n"
    "atapi.sys = 100,,,,,,4_,4,0,0,,1,4\r\n";

static int contains(const char *hay, size_t n, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i;

    if (nl > n)
        return 0;
    for (i = 0; i + nl <= n; i++)
        if (memcmp(hay + i, needle, nl) == 0)
            return 1;
    return 0;
}

static void test_xpsetup_inject(void **state)
{
    char out[1024];
    size_t out_len = 0;

    (void)state;
    assert_int_equal(txtsetup_inject(SIF, sizeof SIF - 1u,
                                     "_falcon", "_falcon.sys",
                                     "Falcon Virtual CD",
                                     out, sizeof out, &out_len), XPSETUP_OK);

    /* Each of the three lines is present, in its section's form. */
    assert_true(contains(out, out_len, "\r\n_falcon = _falcon.sys,4\r\n"));
    assert_true(contains(out, out_len,
                         "\r\n_falcon = \"Falcon Virtual CD\"\r\n"));
    assert_true(contains(out, out_len,
                         "\r\n_falcon.sys = 100,,,,,,4_,4,0,0,,1,4\r\n"));
    /* Each new line sits right after its header, not elsewhere. */
    assert_true(contains(out, out_len, "[SCSI.Load]\r\n_falcon = "));
    assert_true(contains(out, out_len, "[SCSI]\r\n_falcon = \""));
    assert_true(contains(out, out_len, "[SourceDisksFiles]\r\n_falcon.sys = "));
    /* The original content survives, and the file grew by exactly the three
     * lines. */
    assert_true(contains(out, out_len, "atapi = atapi.sys,4\r\n"));
    assert_true(out_len > sizeof SIF - 1u);

    /* [SCSI] is matched exactly, not as a prefix of [SCSI.Load]: the two
     * lines differ. */
    assert_false(contains(out, out_len, "[SCSI.Load]\r\n_falcon = \"Falcon"));
}

static void test_xpsetup_missing_section(void **state)
{
    static const char no_scsi[] =
        "[SCSI.Load]\r\n"
        "atapi = atapi.sys,4\r\n"
        "[SourceDisksFiles]\r\n"
        "atapi.sys = 100,,,,,,4_,4,0,0,,1,4\r\n";
    char out[512];
    size_t out_len = 0;

    (void)state;
    assert_int_equal(txtsetup_inject(no_scsi, sizeof no_scsi - 1u,
                                     "_falcon", "_falcon.sys", "x",
                                     out, sizeof out, &out_len),
                     XPSETUP_ERR_NOSECTION);
}

static void test_xpsetup_nospace(void **state)
{
    char out[40];
    size_t out_len = 0;

    (void)state;
    assert_int_equal(txtsetup_inject(SIF, sizeof SIF - 1u,
                                     "_falcon", "_falcon.sys", "x",
                                     out, sizeof out, &out_len),
                     XPSETUP_ERR_NOSPACE);
}

int run_xpsetup_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_xpsetup_inject),
        cmocka_unit_test(test_xpsetup_missing_section),
        cmocka_unit_test(test_xpsetup_nospace),
    };

    return cmocka_run_group_tests_name("xpsetup", tests, NULL, NULL);
}
