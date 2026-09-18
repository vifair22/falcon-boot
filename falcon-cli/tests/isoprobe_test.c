/* Tests for falcon-cli/src/sign/isoprobe.c, the bless tool's view of an ISO.
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
 *   case         the fingerprints in falcon-core/src/media/family.c are written
 *                in each family's own case, and the two namespaces of a single
 *                image disagree about case with each other. Matching folds both
 *                sides.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#include <sign/sign.h>
#include <sign/isoprobe.h>
#include <media/family.h>

#ifndef ISOVIEW_IMG
#define ISOVIEW_IMG "build/test-isoview.iso"
#endif

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

/* ---------------------------------------------------------------- size */

/* Preparation needs the byte length the ISO's own directory entry records, so
 * that the range it locates and serves is the one the filesystem describes
 * rather than whatever the extractor happened to produce. 7-Zip prints it as a
 * property of the record above it. */
static void test_isoprobe_size_from_the_udf_listing(void **state)
{
    iso_view *v = open_listings(k_win_iso9660, k_win_udf);
    const char *match = NULL;
    uint64_t size = 1;

    (void)state;
    assert_true(iso_view_find(v, "/sources/boot.wim", &match));
    assert_true(iso_view_size(v, match, &size));
    assert_int_equal(size, 576716800u);

    assert_true(iso_view_find(v, "/sources/install.wim", &match));
    assert_true(iso_view_size(v, match, &size));
    assert_int_equal(size, 5749051392u);
    iso_view_close(v);
}

/* "Packed Size" is the cluster-rounded allocation, not the file length, and a
 * prefix match on "Size = " would silently take it for the wrong entry. A
 * directory's empty Size leaves that entry with none at all. */
static void test_isoprobe_size_ignores_packed_and_empty(void **state)
{
    static const char udf[] =
        "Path = sources/boot.wim\n"
        "Folder = -\n"
        "Size = 593121462\n"
        "Packed Size = 593123328\n"
        "\n"
        "Path = efi\n"
        "Folder = +\n"
        "Size = \n"
        "Packed Size = \n";
    iso_view *v = open_listings(NULL, udf);
    const char *match = NULL;
    uint64_t size = 0;

    (void)state;
    assert_true(iso_view_find(v, "/sources/boot.wim", &match));
    assert_true(iso_view_size(v, match, &size));
    assert_int_equal(size, 593121462u);

    assert_true(iso_view_find(v, "/efi", &match));
    size = 42;
    assert_false(iso_view_size(v, match, &size));
    assert_int_equal(size, 0);
    iso_view_close(v);
}

/* bsdtar lists names and no sizes, so an ISO9660 entry answers "no length
 * here" rather than zero-as-a-length, and the caller has to say which. */
static void test_isoprobe_size_absent_for_iso9660(void **state)
{
    iso_view *v = open_listings(k_arch_iso9660, NULL);
    const char *match = NULL;
    uint64_t size = 7;

    (void)state;
    assert_true(iso_view_find(v, "/arch/boot/x86_64/vmlinuz-linux", &match));
    assert_false(iso_view_size(v, match, &size));
    assert_int_equal(size, 0);

    assert_false(iso_view_size(v, "not-a-member-pointer", &size));
    assert_false(iso_view_size(NULL, match, &size));
    assert_false(iso_view_size(v, match, NULL));
    iso_view_close(v);
}

/* ---------------------------------------------------------------- case */

/* The two namespaces of one image disagree about case, and the fingerprints in
 * falcon-core/src/media/family.c are written in whichever case that family's
 * media uses. Both directions have to fold. */
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


/* ------------------------------------------------- the real image */

/* Everything above builds a view from listing text, which is right for the
 * parsing and the union and covers none of the plumbing: spawning the two
 * helpers, picking a 7-Zip binary from the three names it can have, building
 * an extraction argv per namespace, reading a member back through a pipe and
 * writing one out to a file. That is most of the module, and none of it can
 * be reached without a real image and the real helpers.
 *
 * falcon-cli/tools/mk-test-isoview.sh builds a 500 KB ISO for it. ISO9660 only:
 * no xorriso here carries UDF support and neither genisoimage nor mkisofs is
 * installed, but 7-Zip reads ISO9660 when an image has no UDF, so both helpers
 * list this one and both reader paths run. */

static void test_isoview_open_lists_both_readers(void **state)
{
    iso_view *v = NULL;
    const char *match = NULL;

    (void)state;
    assert_int_equal(iso_view_open(ISOVIEW_IMG, &v), SGN_OK);
    assert_non_null(v);

    /* Both helpers ran and both produced entries, which is the whole reason
     * the view is a union rather than one listing. */
    assert_true(iso_view_count(v, ISO_READER_ISO9660) > 0);
    assert_true(iso_view_count(v, ISO_READER_7Z) > 0);
    assert_non_null(iso_view_udf_reader(v));
    /* By full path: 7-Zip finds its formats relative to argv[0], and the bare
     * name loaded none in the appliance (2026-09-18). */
    assert_int_equal(iso_view_udf_reader(v)[0], '/');

    /* A name from the tree, resolved through the real listings rather than a
     * crafted string. */
    assert_true(iso_view_find(v, "/readme.txt", &match));
    assert_non_null(match);

    /* Case folding against a real namespace. */
    match = NULL;
    assert_true(iso_view_find(v, "/mixedcase/thing.txt", &match));
    assert_non_null(match);

    assert_false(iso_view_find(v, "/nothing/here.bin", &match));

    iso_view_close(v);
}

static void test_isoview_open_refuses_a_missing_file(void **state)
{
    iso_view *v = (iso_view *)0x1;

    (void)state;
    assert_int_not_equal(iso_view_open("build/no-such-image.iso", &v), SGN_OK);
    assert_int_not_equal(iso_view_open(NULL, &v), SGN_OK);
    assert_int_not_equal(iso_view_open(ISOVIEW_IMG, NULL), SGN_OK);
}

/* Reading a member back through the helper's pipe, which is how the family
 * fingerprints are matched without unpacking the image. */
static void test_isoview_family_probe_reads_members(void **state)
{
    iso_view *v = NULL;
    family_probe fp;
    char buf[64];
    size_t got = 0;

    (void)state;
    assert_int_equal(iso_view_open(ISOVIEW_IMG, &v), SGN_OK);

    memset(&fp, 0, sizeof fp);
    iso_view_family_probe(v, &fp);
    assert_non_null(fp.exists);
    assert_non_null(fp.read);

    assert_true(fp.exists(fp.ctx, "/readme.txt") != 0);
    assert_true(fp.exists(fp.ctx, "/arch/boot/x86_64/vmlinuz-linux") != 0);
    assert_int_equal(fp.exists(fp.ctx, "/absent/file"), 0);

    /* Known bytes, so this is a test of the pipe and not only of the exit
     * status. */
    memset(buf, 0, sizeof buf);
    assert_int_equal(fp.read(fp.ctx, "/readme.txt", 0, buf, 27u, &got), 0);
    assert_int_equal(got, 27u);
    assert_memory_equal(buf, "falcon-boot isoview fixture", 27u);

    /* At an offset, which is the case the reader has to skip forward for
     * because a pipe cannot seek. */
    memset(buf, 0, sizeof buf);
    assert_int_equal(fp.read(fp.ctx, "/readme.txt", 7u, buf, 7u, &got), 0);
    assert_int_equal(got, 7u);
    assert_memory_equal(buf, "boot is", 7u);

    /* Past the end is a short read rather than a failure. */
    got = 1234u;
    assert_int_equal(fp.read(fp.ctx, "/readme.txt", 1000u, buf, sizeof buf,
                             &got), 0);
    assert_int_equal(got, 0);

    assert_int_not_equal(fp.read(fp.ctx, "/absent/file", 0, buf, 8u, &got), 0);

    iso_view_close(v);
}

/* The kernel-path table against a tree that actually holds one. */
static void test_isoview_kernel_path_found(void **state)
{
    iso_view *v = NULL;
    const char *k;

    (void)state;
    assert_int_equal(iso_view_open(ISOVIEW_IMG, &v), SGN_OK);

    k = iso_kernel_path(v, FAMILY_ARCH);
    assert_non_null(k);

    /* A family whose kernel this tree does not carry, and one with no
     * kernel in the table at all. */
    assert_null(iso_kernel_path(v, FAMILY_DEBIAN_CASPER));
    assert_null(iso_kernel_path(v, FAMILY_WINDOWS));

    iso_view_close(v);
}

static void test_isoview_size_and_extract(void **state)
{
    iso_view *v = NULL;
    const char *match = NULL;
    uint64_t size = 0;
    char dst[] = "/tmp/falcon-isoview-XXXXXX";
    int fd;
    char buf[64];
    ssize_t n;

    (void)state;
    assert_int_equal(iso_view_open(ISOVIEW_IMG, &v), SGN_OK);
    assert_true(iso_view_find(v, "/readme.txt", &match));

    /* 7-Zip's listing carries sizes; bsdtar's does not, so which namespace
     * owns the entry decides whether this can answer at all. */
    (void)iso_view_size(v, match, &size);

    fd = mkstemp(dst);
    assert_true(fd >= 0);
    close(fd);

    assert_int_equal(iso_view_extract(v, match, dst), SGN_OK);
    fd = open(dst, O_RDONLY);
    assert_true(fd >= 0);
    memset(buf, 0, sizeof buf);
    n = read(fd, buf, sizeof buf);
    close(fd);
    unlink(dst);
    assert_int_equal(n, 28);
    assert_memory_equal(buf, "falcon-boot isoview fixture\n", 28u);

    /* A member the view never listed. */
    assert_int_not_equal(iso_view_extract(v, "/absent/file", dst), SGN_OK);

    iso_view_close(v);
}

/* ------------------------------------------------------------ 7-Zip lookup */

/* A PATH holding only fake 7-Zips: shell scripts standing in for `7z i`, one
 * whose format table has the UDF row and one whose table is empty, which is
 * what a 7-Zip that could not load 7z.so prints. */
static char g_fake_dir[64];
static char *g_saved_path;

static void fake_7z(const char *name, const char *table)
{
    char path[128];
    FILE *f;

    (void)snprintf(path, sizeof(path), "%s/%s", g_fake_dir, name);
    f = fopen(path, "w");
    assert_non_null(f);
    (void)fprintf(f, "#!/bin/sh\nprintf 'Formats:\\n%s\\n'\n", table);
    assert_int_equal(fclose(f), 0);
    assert_int_equal(chmod(path, 0755), 0);
}

static int setup_fake_path(void **state)
{
    const char *p = getenv("PATH");

    (void)state;
    (void)snprintf(g_fake_dir, sizeof(g_fake_dir), "/tmp/falcon-7z.XXXXXX");
    if (mkdtemp(g_fake_dir) == NULL)
        return -1;
    g_saved_path = strdup(p != NULL ? p : "/usr/bin:/bin");
    return setenv("PATH", g_fake_dir, 1);
}

static int teardown_fake_path(void **state)
{
    char cmd[128];

    (void)state;
    (void)setenv("PATH", g_saved_path, 1);
    free(g_saved_path);
    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_fake_dir);
    return system(cmd) == 0 ? 0 : -1;
}

static void test_which_reports_the_full_path(void **state)
{
    char out[PATH_MAX];
    char want[PATH_MAX];
    char tiny[4];

    (void)state;
    fake_7z("7zz", "");
    (void)snprintf(want, sizeof(want), "%s/7zz", g_fake_dir);
    assert_int_equal(sgn_which("7zz", out, sizeof(out)), 1);
    assert_string_equal(out, want);
    assert_int_equal(sgn_have_program("7zz"), 1);

    /* A name with a slash is taken as the path itself. */
    assert_int_equal(sgn_which(want, out, sizeof(out)), 1);
    assert_string_equal(out, want);

    assert_int_equal(sgn_which("no-such-program", out, sizeof(out)), 0);
    assert_int_equal(sgn_which("7zz", tiny, sizeof(tiny)), 0);
    assert_int_equal(sgn_which(want, tiny, sizeof(tiny)), 0);
    assert_int_equal(sgn_which(NULL, out, sizeof(out)), 0);
    assert_int_equal(sgn_which("7zz", NULL, 0), 0);
    assert_int_equal(unlink(want), 0);
}

static void test_udf_reader_needs_the_udf_format(void **state)
{
    char which[PATH_MAX];
    char want[PATH_MAX];

    (void)state;
    /* None at all. */
    assert_int_equal(iso_udf_reader_ok(which, sizeof(which)), 0);
    assert_string_equal(which, "");

    /* Present, formats not loaded: the appliance's failure. */
    fake_7z("7za", "");
    (void)snprintf(want, sizeof(want), "%s/7za", g_fake_dir);
    assert_int_equal(iso_udf_reader_ok(which, sizeof(which)), 0);
    assert_string_equal(which, want);

    /* Present and able to read UDF; 7z is preferred over 7za. */
    fake_7z("7z", " 0  ....M.O...  Udf      udf iso img");
    (void)snprintf(want, sizeof(want), "%s/7z", g_fake_dir);
    assert_int_equal(iso_udf_reader_ok(which, sizeof(which)), 1);
    assert_string_equal(which, want);
    assert_int_equal(iso_udf_reader_ok(NULL, 0), 1);
}

int run_isoprobe_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_isoprobe_udf_only_windows),
        cmocka_unit_test(test_isoprobe_iso9660_only_still_classifies),
        cmocka_unit_test(test_isoprobe_iso9660_wins_when_both_have_it),
        cmocka_unit_test(test_isoprobe_slt_reads_only_path_properties),
        cmocka_unit_test(test_isoprobe_normalises_separators),
        cmocka_unit_test(test_isoprobe_unterminated_last_line),
        cmocka_unit_test(test_isoprobe_size_from_the_udf_listing),
        cmocka_unit_test(test_isoprobe_size_ignores_packed_and_empty),
        cmocka_unit_test(test_isoprobe_size_absent_for_iso9660),
        cmocka_unit_test(test_isoprobe_case_insensitive_both_ways),
        cmocka_unit_test(test_isoprobe_directory_matches_on_component_boundary),
        cmocka_unit_test(test_isoprobe_wildcard_suffix),
        cmocka_unit_test(test_isoprobe_udf_reader_reported),
        cmocka_unit_test(test_isoprobe_empty_and_null),
        cmocka_unit_test(test_isoprobe_reader_of_foreign_pointer),
        cmocka_unit_test(test_isoprobe_kernel_path_without_a_kernel),
        cmocka_unit_test(test_isoprobe_kernel_path_candidates_all_absent),
    };

    const struct CMUnitTest image_tests[] = {
        cmocka_unit_test(test_isoview_open_lists_both_readers),
        cmocka_unit_test(test_isoview_open_refuses_a_missing_file),
        cmocka_unit_test(test_isoview_family_probe_reads_members),
        cmocka_unit_test(test_isoview_kernel_path_found),
        cmocka_unit_test(test_isoview_size_and_extract),
    };
    const struct CMUnitTest lookup_tests[] = {
        cmocka_unit_test(test_which_reports_the_full_path),
        cmocka_unit_test(test_udf_reader_needs_the_udf_format),
    };
    int failed;
    int fd;

    failed = cmocka_run_group_tests_name("ISO namespaces (bless tool)", tests,
                                         NULL, NULL);
    failed += cmocka_run_group_tests_name("7-Zip lookup (bless tool)",
                                          lookup_tests, setup_fake_path,
                                          teardown_fake_path);

    fd = open(ISOVIEW_IMG, O_RDONLY);
    if (fd < 0) {
        printf("\n[  SKIPPED ] ISO view fixture: %s not found.\n"
               "             Build it with: make test-isoview (needs xorriso).\n",
               ISOVIEW_IMG);
        return failed + test_skip_rc();
    }
    close(fd);

    return failed + cmocka_run_group_tests_name("ISO view (real image)",
                                                image_tests, NULL, NULL);
}
