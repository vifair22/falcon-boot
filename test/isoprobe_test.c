/* Tests for src/cli/sign/isoprobe.c, the bless tool's view of an ISO.
 *
 * The view is the union of two listings, one per namespace: bsdtar's, which
 * is ISO9660 plus Rock Ridge, and 7-Zip's, which is UDF on any image that has
 * one. Microsoft builds Windows install media with `oscdimg -u1 -udfver102`,
 * whose ISO9660 side is a stub holding one README while every real file lives
 * only in UDF, so a view that consults one namespace classifies that medium as
 * unknown. That is the bug these tests pin.
 *
 * Everything here goes through iso_view_from_listings, which builds the view
 * from listing text instead of from a helper process. That is deliberate: the
 * parts worth testing are the parsing of two unrelated output formats, the
 * union, and the case folding, none of which need a subprocess or a
 * multi-gigabyte image to exercise, and all of which are where the sharp
 * edges are.
 *
 * Three things are pinned:
 *
 *   namespaces   a UDF-only tree is found, an ISO9660-only tree is found, and
 *                a name in both resolves to the ISO9660 entry, because that is
 *                the reader every Linux image has always been extracted with
 *                and the union must not change which one is used.
 *
 *   parsing      7-Zip's -slt output is records of "key = value" lines and
 *                only the Path key names an entry; bsdtar's is bare paths with
 *                inconsistent separators. Both are normalised to the one
 *                convention family.h specifies.
 *
 *   case         the fingerprints in src/family are written in each family's
 *                own case, and the two namespaces of a single image disagree
 *                about case with each other. Matching folds both sides.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <cli/sign/sign.h>
#include <cli/sign/isoprobe.h>

/* ------------------------------------------------------------ fixtures */

/* What bsdtar prints for a Windows image built by oscdimg: the image root and
 * the one file the ISO9660 stub carries. This is the whole bug in four
 * lines. */
static const char k_win_iso9660[] =
    ".\n"
    "README.TXT\n";

/* What 7-Zip -ba -slt prints for the same image, trimmed to the entries that
 * matter and keeping the surrounding properties so the parser is tested
 * against the real record shape rather than a list of paths. oscdimg writes
 * the UDF names in lower case; the ISO9660 stub beside them is upper. */
static const char k_win_udf[] =
    "Path = autorun.inf\n"
    "Folder = -\n"
    "Size = 128\n"
    "Packed Size = 2048\n"
    "\n"
    "Path = efi\n"
    "Folder = +\n"
    "Size = \n"
    "\n"
    "Path = efi/microsoft/boot/bcd\n"
    "Folder = -\n"
    "Size = 16384\n"
    "\n"
    "Path = sources\n"
    "Folder = +\n"
    "\n"
    "Path = sources/install.wim\n"
    "Folder = -\n"
    "Size = 5749051392\n"
    "\n"
    "Path = sources/boot.wim\n"
    "Folder = -\n"
    "Size = 576716800\n";

static const char k_arch_iso9660[] =
    ".\n"
    "arch\n"
    "arch/boot\n"
    "arch/boot/x86_64\n"
    "arch/boot/x86_64/initramfs-linux.img\n"
    "arch/boot/x86_64/vmlinuz-linux\n"
    "arch/version\n"
    "EFI\n"
    "EFI/BOOT\n"
    "EFI/BOOT/BOOTx64.EFI\n";

/* ------------------------------------------------------------ helpers */

static iso_view *open_listings(const char *iso9660, const char *udf)
{
    iso_view *v = NULL;

    assert_int_equal(iso_view_from_listings(iso9660, udf, &v), SGN_OK);
    assert_non_null(v);
    return v;
}

/* ------------------------------------------------------------ the bug */

static void test_isoprobe_udf_only_windows(void **state)
{
    iso_view *v = open_listings(k_win_iso9660, k_win_udf);
    const char *match = NULL;
    family_probe probe;

    (void)state;

    /* Before the fix the view held README.TXT and nothing else. */
    assert_int_equal(iso_view_count(v, ISO_READER_ISO9660), 1u);
    assert_int_equal(iso_view_count(v, ISO_READER_7Z), 6u);

    assert_true(iso_view_find(v, "/sources/install.wim", &match));
    assert_string_equal(match, "sources/install.wim");
    assert_int_equal(iso_view_reader_of(v, match), ISO_READER_7Z);

    iso_view_family_probe(v, &probe);
    assert_int_equal(family_detect(&probe)->id, FAMILY_WINDOWS);
    iso_view_close(v);
}

/* The ISO9660-only path is the one that worked before and must be untouched:
 * same listing, no UDF namespace at all, same answer. */
static void test_isoprobe_iso9660_only_still_classifies(void **state)
{
    iso_view *v = open_listings(k_arch_iso9660, NULL);
    family_probe probe;

    (void)state;
    assert_int_equal(iso_view_count(v, ISO_READER_7Z), 0u);
    assert_null(iso_view_udf_reader(v));

    iso_view_family_probe(v, &probe);
    assert_int_equal(family_detect(&probe)->id, FAMILY_ARCH);
    assert_string_equal(iso_kernel_path(v, FAMILY_ARCH),
                        "arch/boot/x86_64/vmlinuz-linux");
    iso_view_close(v);
}

/* A hybrid image lists the same tree twice. The ISO9660 entry must win, so
 * that adding the UDF pass cannot move a Linux image onto the other reader. */
static void test_isoprobe_iso9660_wins_when_both_have_it(void **state)
{
    static const char udf[] =
        "Path = arch/boot/x86_64/vmlinuz-linux\n"
        "Folder = -\n";
    iso_view *v = open_listings(k_arch_iso9660, udf);
    const char *match = NULL;

    (void)state;
    assert_true(iso_view_find(v, "/arch/boot/x86_64/vmlinuz-linux", &match));
    assert_int_equal(iso_view_reader_of(v, match), ISO_READER_ISO9660);
    iso_view_close(v);
}

/* ------------------------------------------------------------- parsing */

/* Only the Path key names an entry. Every other property is a value that can
 * contain anything at all, including something shaped like a path. */
static void test_isoprobe_slt_reads_only_path_properties(void **state)
{
    static const char udf[] =
        "Path = sources/install.wim\n"
        "Folder = -\n"
        "Comment = Path = not/an/entry\n"
        "Method = Copy\n"
        "\n"
        "Path = sources/boot.wim\n"
        "Folder = -\n";
    iso_view *v = open_listings(NULL, udf);

    (void)state;
    assert_int_equal(iso_view_count(v, ISO_READER_7Z), 2u);
    assert_false(iso_view_find(v, "/not/an/entry", NULL));
    assert_true(iso_view_find(v, "/sources/boot.wim", NULL));
    iso_view_close(v);
}

/* Separators: a leading '/', a leading "./", a trailing '/' on a directory and
 * a CRLF line ending all mean the same entry. None of the four is a name. */
static void test_isoprobe_normalises_separators(void **state)
{
    static const char tar[] =
        ".\n"
        "./casper/\r\n"
        "/casper/vmlinuz\n"
        "\n"
        "/\n";
    iso_view *v = open_listings(tar, NULL);
    const char *match = NULL;

    (void)state;
    assert_int_equal(iso_view_count(v, ISO_READER_ISO9660), 2u);
    assert_true(iso_view_find(v, "/casper", &match));
    assert_string_equal(match, "casper");
    assert_true(iso_view_find(v, "/casper/vmlinuz", &match));
    assert_string_equal(match, "casper/vmlinuz");
    iso_view_close(v);
}

/* A listing whose final line has no newline still names its last entry. */
static void test_isoprobe_unterminated_last_line(void **state)
{
    iso_view *v = open_listings("casper\ncasper/vmlinuz", NULL);

    (void)state;
    assert_int_equal(iso_view_count(v, ISO_READER_ISO9660), 2u);
    assert_true(iso_view_find(v, "/casper/vmlinuz", NULL));
    iso_view_close(v);
}

/* ---------------------------------------------------------------- case */

/* The two namespaces of one image disagree about case, and the fingerprints
 * in src/family are written in whichever case that family's media uses. Both
 * directions have to fold. */
static void test_isoprobe_case_insensitive_both_ways(void **state)
{
    static const char tar[] = "SOURCES\nSOURCES/INSTALL.WIM\n";
    static const char udf[] = "Path = efi/microsoft/boot/bcd\n";
    iso_view *v = open_listings(tar, udf);

    (void)state;
    /* lower-case fingerprint against an upper-case ISO9660 name */
    assert_true(iso_view_find(v, "/sources/install.wim", NULL));
    /* upper-case query against a lower-case UDF name */
    assert_true(iso_view_find(v, "/EFI/Microsoft/Boot/BCD", NULL));
    assert_false(iso_view_find(v, "/sources/install.esd", NULL));
    iso_view_close(v);
}

/* A query for a bare directory matches on a path component boundary and never
 * on a bare prefix, or "/live" would match "/livecd-something". */
static void test_isoprobe_directory_matches_on_component_boundary(void **state)
{
    iso_view *v = open_listings("livecd-tools/readme\nlive/vmlinuz-6.1\n",
                                NULL);

    (void)state;
    assert_true(iso_view_find(v, "/live", NULL));
    assert_false(iso_view_find(v, "/liv", NULL));
    iso_view_close(v);
}

/* The trailing '*' the per-family kernel table uses for versioned names. */
static void test_isoprobe_wildcard_suffix(void **state)
{
    iso_view *v = open_listings("live\nlive/vmlinuz-6.1.0-40-amd64\n", NULL);
    const char *match = NULL;

    (void)state;
    assert_false(iso_view_find(v, "/live/vmlinuz", NULL));
    assert_true(iso_view_find(v, "/live/vmlinuz*", &match));
    assert_string_equal(match, "live/vmlinuz-6.1.0-40-amd64");
    assert_string_equal(iso_kernel_path(v, FAMILY_DEBIAN_LIVE), match);
    iso_view_close(v);
}

/* ------------------------------------------------------------- degrade */

/* Whether the UDF namespace was read at all is a separate question from
 * whether a file was found in it, because the bless flow reports the two
 * differently: one is "this ISO has no such file", the other is "nothing here
 * could read the namespace that file would be in". */
static void test_isoprobe_udf_reader_reported(void **state)
{
    iso_view *with = open_listings(NULL, "Path = sources/install.wim\n");
    iso_view *without = open_listings(k_arch_iso9660, NULL);

    (void)state;
    assert_non_null(iso_view_udf_reader(with));
    assert_null(iso_view_udf_reader(without));
    assert_null(iso_view_udf_reader(NULL));
    iso_view_close(with);
    iso_view_close(without);
}

/* An empty view is a legal one: it answers no and does not crash. A view with
 * no image behind it reports content as unreadable rather than inventing it,
 * which family.h defines as a fingerprint that does not match. */
static void test_isoprobe_empty_and_null(void **state)
{
    iso_view *v = open_listings(NULL, NULL);
    family_probe probe;
    uint8_t buf[16];
    size_t got = 1;

    (void)state;
    assert_int_equal(iso_view_count(v, ISO_READER_ISO9660), 0u);
    assert_false(iso_view_find(v, "/anything", NULL));
    assert_false(iso_view_find(NULL, "/anything", NULL));
    assert_false(iso_view_find(v, NULL, NULL));
    /* A bare "/" and a bare "*" name nothing, and must not match everything. */
    assert_false(iso_view_find(v, "/", NULL));
    assert_false(iso_view_find(v, "*", NULL));
    assert_int_equal(family_detect(NULL)->id, FAMILY_UNKNOWN);

    iso_view_family_probe(v, &probe);
    assert_int_equal(probe.read(probe.ctx, "/whatever", 0, buf, sizeof(buf),
                                &got), 1);
    assert_int_equal(got, 0u);
    assert_int_equal(iso_view_extract(v, "sources/install.wim", "/dev/null"),
                     SGN_ERR_INTERNAL);
    iso_view_close(v);
    iso_view_close(NULL);
}

/* A member pointer the view never handed out has no reader, and the answer is
 * the one that preserves the behaviour from before UDF existed. */
static void test_isoprobe_reader_of_foreign_pointer(void **state)
{
    iso_view *v = open_listings(NULL, "Path = sources/install.wim\n");

    (void)state;
    assert_int_equal(iso_view_reader_of(v, "sources/install.wim"),
                     ISO_READER_ISO9660);
    assert_int_equal(iso_view_reader_of(v, NULL), ISO_READER_ISO9660);
    assert_int_equal(iso_view_reader_of(NULL, "x"), ISO_READER_ISO9660);
    iso_view_close(v);
}

/* The families with no kernel to wrap say so, and say which family they are,
 * rather than reporting a missing file. */
static void test_isoprobe_kernel_path_without_a_kernel(void **state)
{
    iso_view *v = open_listings(k_win_iso9660, k_win_udf);

    (void)state;
    assert_null(iso_kernel_path(v, FAMILY_WINDOWS));
    assert_int_equal(sgn_error_code(), SGN_ERR_FORMAT);
    assert_null(iso_kernel_path(NULL, FAMILY_ARCH));
    iso_view_close(v);
}

/* A family whose kernel table row exists but whose candidates are all absent
 * is a different failure from a family with no row at all. */
static void test_isoprobe_kernel_path_candidates_all_absent(void **state)
{
    iso_view *v = open_listings("casper\ncasper/filesystem.squashfs\n", NULL);

    (void)state;
    assert_null(iso_kernel_path(v, FAMILY_DEBIAN_CASPER));
    assert_int_equal(sgn_error_code(), SGN_ERR_FORMAT);
    iso_view_close(v);
}

/* ------------------------------------------------------------------ run */

int run_isoprobe_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_isoprobe_udf_only_windows),
        cmocka_unit_test(test_isoprobe_iso9660_only_still_classifies),
        cmocka_unit_test(test_isoprobe_iso9660_wins_when_both_have_it),
        cmocka_unit_test(test_isoprobe_slt_reads_only_path_properties),
        cmocka_unit_test(test_isoprobe_normalises_separators),
        cmocka_unit_test(test_isoprobe_unterminated_last_line),
        cmocka_unit_test(test_isoprobe_case_insensitive_both_ways),
        cmocka_unit_test(test_isoprobe_directory_matches_on_component_boundary),
        cmocka_unit_test(test_isoprobe_wildcard_suffix),
        cmocka_unit_test(test_isoprobe_udf_reader_reported),
        cmocka_unit_test(test_isoprobe_empty_and_null),
        cmocka_unit_test(test_isoprobe_reader_of_foreign_pointer),
        cmocka_unit_test(test_isoprobe_kernel_path_without_a_kernel),
        cmocka_unit_test(test_isoprobe_kernel_path_candidates_all_absent),
    };

    return cmocka_run_group_tests_name("ISO namespaces (bless tool)", tests,
                                       NULL, NULL);
}
