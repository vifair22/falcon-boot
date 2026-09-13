/* Tests for src/manifest, the blessed-artifact manifest of docs/DESIGN.md
 * Section 8.1.
 *
 * The module is pure text in, records out, with no I/O of any kind, so the
 * tests are the format's specification made executable. Four things are
 * pinned, in rough order of how much damage getting them wrong would do:
 *
 *   totality     the parser runs in the loader against a file on writable,
 *                unverified exFAT. There must be no input that makes it
 *                report an error the loader has to handle, read out of
 *                bounds, or truncate a path into one that matches the wrong
 *                file. Every malformed shape gets a case: a record missing
 *                each required key in turn, a size that is not a number, a
 *                size past 2^63, values longer than their fields, non-ASCII
 *                bytes, a truncated final record, a line with no newline at
 *                the end, and a megabyte-long line.
 *
 *   tolerance    the same file is hand-edited on three operating systems, so
 *                CRLF, comments, unknown keys and stray whitespace are all
 *                normal input and none of them may cost a record.
 *
 *   round trip   render() then parse() must return what went in. A re-bless
 *                is parse, upsert, render, replace-file; any field the round
 *                trip mangled would silently point the loader at the wrong
 *                artifact, and the mangling would only show up at boot.
 *
 *   matching     the Section 8.1 rule is path AND size: a size mismatch is
 *                stale-blessed, which boots as unblessed, and getting the
 *                path comparison wrong in either direction means either a
 *                blessed ISO that will not boot blessed or, worse, one ISO
 *                booting another's signed cmdline.
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

#include <manifest/manifest.h>

#define CAP 16u

/* A well-formed two-record manifest in the exact shape DESIGN Section 8.1
 * documents, trailing comments and all. */
static const char g_two[] =
    "# falcon-boot manifest\n"
    "\n"
    "iso=Linux/archlinux-2026.01.01-x86_64.iso   # path relative to p2 root\n"
    "size=1538670592                            # byte size at bless time\n"
    "sha256=1111111111111111111111111111111111111111111111111111111111111111\n"
    "uki=archlinux-2026.01.01-x86_64.uki\n"
    "family=arch\n"
    "\n"
    "iso=kubuntu-26.04-desktop-amd64.iso\n"
    "size=5068847104\n"
    "sha256=2222222222222222222222222222222222222222222222222222222222222222\n"
    "uki=kubuntu-26.04.uki\n"
    "family=casper\n";

static size_t parse_ok(const char *text, manifest_record *out, size_t cap)
{
    size_t n = 0;

    assert_int_equal(manifest_parse(text, strlen(text), out, cap, &n),
                     MANIFEST_OK);
    return n;
}

/* ------------------------------------------------------------- parsing */

static void test_manifest_two_records(void **state)
{
    manifest_record r[CAP];
    size_t n;

    (void)state;
    n = parse_ok(g_two, r, CAP);
    assert_int_equal(n, 2);

    assert_string_equal(r[0].iso, "Linux/archlinux-2026.01.01-x86_64.iso");
    assert_int_equal(r[0].size, 1538670592u);
    assert_string_equal(r[0].uki, "archlinux-2026.01.01-x86_64.uki");
    assert_string_equal(r[0].family, "arch");
    assert_int_equal(r[0].has_sha256, 1);
    assert_int_equal(strlen(r[0].sha256), MANIFEST_SHA256_HEX);

    assert_string_equal(r[1].iso, "kubuntu-26.04-desktop-amd64.iso");
    assert_int_equal(r[1].size, 5068847104u);
    assert_string_equal(r[1].uki, "kubuntu-26.04.uki");
    assert_string_equal(r[1].family, "casper");
}

static void test_manifest_empty(void **state)
{
    manifest_record r[CAP];
    size_t n = 99;

    (void)state;
    assert_int_equal(manifest_parse("", 0, r, CAP, &n), MANIFEST_OK);
    assert_int_equal(n, 0);
    /* A NULL buffer with a zero length is the "file did not exist" call the
     * loader makes, and it must behave exactly like an empty one. */
    n = 99;
    assert_int_equal(manifest_parse(NULL, 0, r, CAP, &n), MANIFEST_OK);
    assert_int_equal(n, 0);
}

static void test_manifest_comments_only(void **state)
{
    manifest_record r[CAP];

    (void)state;
    assert_int_equal(parse_ok("# nothing here\n#iso=a\nsize=1\n\n", r, CAP), 0);
}

static void test_manifest_crlf(void **state)
{
    static const char crlf[] =
        "# written on Windows\r\n"
        "iso=a.iso\r\n"
        "size=12\r\n"
        "uki=a.uki\r\n"
        "family=arch\r\n";
    manifest_record r[CAP];

    (void)state;
    assert_int_equal(parse_ok(crlf, r, CAP), 1);
    assert_string_equal(r[0].iso, "a.iso");
    assert_string_equal(r[0].uki, "a.uki");
    assert_string_equal(r[0].family, "arch");
    assert_int_equal(r[0].size, 12);
}

static void test_manifest_whitespace_and_unknown_keys(void **state)
{
    static const char text[] =
        "  iso  =  a.iso  \n"
        "flavour=strawberry\n"      /* a key from a future version */
        "\tsize\t=\t12\t\n"
        "uki = a.uki\n"
        "= orphan value\n"          /* no key at all */
        "no-equals-sign-at-all\n";
    manifest_record r[CAP];

    (void)state;
    assert_int_equal(parse_ok(text, r, CAP), 1);
    assert_string_equal(r[0].iso, "a.iso");
    assert_string_equal(r[0].uki, "a.uki");
    assert_int_equal(r[0].size, 12);
}

static void test_manifest_no_trailing_newline(void **state)
{
    manifest_record r[CAP];

    (void)state;
    assert_int_equal(parse_ok("iso=a.iso\nsize=1\nuki=a.uki", r, CAP), 1);
    assert_string_equal(r[0].uki, "a.uki");
}

static void test_manifest_keys_before_first_iso(void **state)
{
    manifest_record r[CAP];

    (void)state;
    /* Belong to no block; they must not become one, and must not poison the
     * block that follows. */
    assert_int_equal(parse_ok("size=5\nuki=stray.uki\n"
                              "iso=a.iso\nsize=1\nuki=a.uki\n", r, CAP), 1);
    assert_int_equal(r[0].size, 1);
    assert_string_equal(r[0].uki, "a.uki");
}

/* ------------------------------------------------------- bad records */

static void test_manifest_missing_keys_skipped(void **state)
{
    static const char *const bad[] = {
        "iso=a.iso\nsize=1\n",                  /* no uki */
        "iso=a.iso\nuki=a.uki\n",               /* no size */
        "iso=\nsize=1\nuki=a.uki\n",            /* empty path */
        "iso=a.iso\nsize=\nuki=a.uki\n",        /* empty size */
        "iso=a.iso\nsize=1\nuki=\n",            /* empty uki */
    };
    manifest_record r[CAP];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert_int_equal(parse_ok(bad[i], r, CAP), 0);
}

static void test_manifest_bad_size_skipped(void **state)
{
    static const char *const bad[] = {
        "iso=a.iso\nsize=nine\nuki=a.uki\n",
        "iso=a.iso\nsize=12x\nuki=a.uki\n",
        "iso=a.iso\nsize=-1\nuki=a.uki\n",
        "iso=a.iso\nsize=0x10\nuki=a.uki\n",
        "iso=a.iso\nsize=1 2\nuki=a.uki\n",
        /* 2^63, one past MANIFEST_SIZE_MAX */
        "iso=a.iso\nsize=9223372036854775808\nuki=a.uki\n",
        /* 2^64, past what the accumulator itself can hold */
        "iso=a.iso\nsize=18446744073709551616\nuki=a.uki\n",
        "iso=a.iso\nsize=99999999999999999999999999\nuki=a.uki\n",
    };
    manifest_record r[CAP];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert_int_equal(parse_ok(bad[i], r, CAP), 0);

    /* The boundary itself is still a valid record. */
    assert_int_equal(parse_ok("iso=a.iso\nsize=9223372036854775807\n"
                              "uki=a.uki\n", r, CAP), 1);
    assert_true(r[0].size == MANIFEST_SIZE_MAX);
}

static void test_manifest_non_ascii_path_skipped(void **state)
{
    /* A UTF-8 e-acute in the file name. The loader compares manifest paths
     * against exFAT names as bytes, which only works because every path here
     * is ASCII; a record that is not gets dropped, and the ISO is simply not
     * blessable until it is renamed. */
    manifest_record r[CAP];

    (void)state;
    assert_int_equal(parse_ok("iso=caf\xC3\xA9.iso\nsize=1\nuki=a.uki\n",
                              r, CAP), 0);
    /* Same rule on the other fields. */
    assert_int_equal(parse_ok("iso=a.iso\nsize=1\nuki=caf\xC3\xA9.uki\n",
                              r, CAP), 0);
    assert_int_equal(parse_ok("iso=a.iso\nsize=1\nuki=a.uki\n"
                              "family=ar\xC3\xA9" "ch\n", r, CAP), 0);
    /* A control character is out for the same reason: it is not printable
     * ASCII, and a tab or a NUL inside a path is nothing good. */
    assert_int_equal(parse_ok("iso=a\x01.iso\nsize=1\nuki=a.uki\n", r, CAP), 0);
}

static void test_manifest_overlong_values_skipped(void **state)
{
    char text[MANIFEST_PATH_MAX + 512u];
    manifest_record r[CAP];
    size_t i;
    size_t w;

    (void)state;
    /* Exactly one byte too long for the field: a truncating parser would
     * accept this and produce a path that matches a different file. */
    w = (size_t)snprintf(text, sizeof(text), "iso=");
    for (i = 0; i < MANIFEST_PATH_MAX; i++)
        text[w + i] = 'a';
    (void)snprintf(text + w + MANIFEST_PATH_MAX,
                   sizeof(text) - w - MANIFEST_PATH_MAX,
                   "\nsize=1\nuki=a.uki\n");
    assert_int_equal(parse_ok(text, r, CAP), 0);

    /* One byte shorter is the longest path the format allows. */
    w = (size_t)snprintf(text, sizeof(text), "iso=");
    for (i = 0; i < MANIFEST_PATH_MAX - 1u; i++)
        text[w + i] = 'a';
    (void)snprintf(text + w + MANIFEST_PATH_MAX - 1u,
                   sizeof(text) - w - MANIFEST_PATH_MAX + 1u,
                   "\nsize=1\nuki=a.uki\n");
    assert_int_equal(parse_ok(text, r, CAP), 1);
    assert_int_equal(strlen(r[0].iso), MANIFEST_PATH_MAX - 1u);
}

static void test_manifest_bad_sha256_keeps_record(void **state)
{
    manifest_record r[CAP];

    (void)state;
    /* The loader never reads sha256 and the bless tool recomputes it, so a
     * mangled digest costs the field and not the record. */
    assert_int_equal(parse_ok("iso=a.iso\nsize=1\nuki=a.uki\nsha256=zzz\n",
                              r, CAP), 1);
    assert_int_equal(r[0].has_sha256, 0);
    assert_string_equal(r[0].sha256, "");

    /* Uppercase hex is accepted and normalised, because openssl and
     * sha256sum disagree about case and both end up in hand-edited files. */
    assert_int_equal(parse_ok("iso=a.iso\nsize=1\nuki=a.uki\n"
        "sha256=ABCDEF0123456789ABCDEF0123456789"
        "ABCDEF0123456789ABCDEF0123456789\n", r, CAP), 1);
    assert_int_equal(r[0].has_sha256, 1);
    assert_string_equal(r[0].sha256,
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
}

static void test_manifest_truncated_tail(void **state)
{
    manifest_record r[CAP];
    size_t n;
    size_t full = strlen(g_two);
    size_t cut;

    (void)state;
    /* Every prefix of a good file must parse without complaint, yielding
     * only the records that were complete. This is what a stick pulled
     * mid-write looks like. */
    for (cut = 0; cut < full; cut++) {
        assert_int_equal(manifest_parse(g_two, cut, r, CAP, &n), MANIFEST_OK);
        assert_true(n <= 2u);
    }
    /* And the specific case worth naming: the second block cut off after
     * its size line has no uki, so it is dropped and the first survives. */
    assert_int_equal(parse_ok(
        "iso=a.iso\nsize=1\nuki=a.uki\niso=b.iso\nsize=2\n", r, CAP), 1);
    assert_string_equal(r[0].iso, "a.iso");
}

static void test_manifest_huge_line(void **state)
{
    size_t huge = 1u << 20;
    char *text = malloc(huge + 256u);
    manifest_record r[CAP];
    size_t n;

    (void)state;
    assert_non_null(text);
    memset(text, 'x', huge);
    n = (size_t)snprintf(text + huge, 256u,
                         "\niso=a.iso\nsize=1\nuki=a.uki\n");
    /* A megabyte of junk on one line costs the scan and nothing else: the
     * parser holds no line buffer, so there is no size at which it starts
     * behaving differently. */
    assert_int_equal(manifest_parse(text, huge + n, r, CAP, &n), MANIFEST_OK);
    assert_int_equal(n, 1);
    assert_string_equal(r[0].iso, "a.iso");
    free(text);
}

static void test_manifest_no_nul_needed(void **state)
{
    /* The loader hands over a buffer the exFAT driver filled, which is not
     * NUL-terminated. Parsing must stop at len and not one byte later: the
     * poison byte here would join the last value if it did. */
    static const char text[] = "iso=a.iso\nsize=1\nuki=a.uki\nZZZ";
    manifest_record r[CAP];
    size_t n = 0;

    (void)state;
    assert_int_equal(manifest_parse(text, strlen(text) - 3u, r, CAP, &n),
                     MANIFEST_OK);
    assert_int_equal(n, 1);
    assert_string_equal(r[0].uki, "a.uki");
}

static void test_manifest_nospace(void **state)
{
    manifest_record one;
    size_t n = 0;

    (void)state;
    /* The caller's array is too small: the records that fit are still
     * written, and *count reports the real total so the caller knows what to
     * resize to. Same contract as exfat_dir_read. */
    assert_int_equal(manifest_parse(g_two, strlen(g_two), &one, 1u, &n),
                     MANIFEST_ERR_NOSPACE);
    assert_int_equal(n, 2);
    assert_string_equal(one.iso, "Linux/archlinux-2026.01.01-x86_64.iso");

    /* A zero-capacity parse is the legitimate way to ask how many records a
     * file holds before allocating for them. */
    n = 0;
    assert_int_equal(manifest_parse(g_two, strlen(g_two), NULL, 0, &n),
                     MANIFEST_ERR_NOSPACE);
    assert_int_equal(n, 2);
}

static void test_manifest_bad_args(void **state)
{
    manifest_record r[CAP];
    size_t n = 0;

    (void)state;
    assert_int_equal(manifest_parse("x", 1, r, CAP, NULL),
                     MANIFEST_ERR_BAD_ARG);
    assert_int_equal(manifest_parse(NULL, 4, r, CAP, &n),
                     MANIFEST_ERR_BAD_ARG);
    assert_int_equal(manifest_parse("x", 1, NULL, CAP, &n),
                     MANIFEST_ERR_BAD_ARG);
}

/* -------------------------------------------------------------- lookup */

static void test_manifest_path_eq(void **state)
{
    (void)state;
    assert_true(manifest_path_eq("a/b.iso", "a/b.iso"));
    /* exFAT is case-insensitive and the loader's paths come from it. */
    assert_true(manifest_path_eq("Linux/Arch.ISO", "linux/arch.iso"));
    /* The loader's path arrives backslash-separated from the exFAT walk;
     * the manifest is written with forward slashes by hand. */
    assert_true(manifest_path_eq("Linux\\arch.iso", "Linux/arch.iso"));
    assert_true(manifest_path_eq("/Linux/arch.iso", "Linux/arch.iso"));
    assert_true(manifest_path_eq("Linux//arch.iso", "Linux/arch.iso"));
    assert_true(manifest_path_eq("Linux/arch.iso/", "Linux/arch.iso"));

    assert_false(manifest_path_eq("Linuxarch.iso", "Linux/arch.iso"));
    assert_false(manifest_path_eq("Linux/arch.iso", "Linux/arch.is"));
    assert_false(manifest_path_eq("other/arch.iso", "Linux/arch.iso"));
    assert_false(manifest_path_eq(NULL, "a"));
    assert_false(manifest_path_eq("a", NULL));
}

static void test_manifest_find_and_lookup(void **state)
{
    manifest_record r[CAP];
    const manifest_record *hit = NULL;
    size_t n;

    (void)state;
    n = parse_ok(g_two, r, CAP);

    assert_non_null(manifest_find(r, n, "Linux/archlinux-2026.01.01-x86_64.iso"));
    assert_non_null(manifest_find(r, n,
                                  "\\linux\\ARCHLINUX-2026.01.01-X86_64.ISO"));
    assert_null(manifest_find(r, n, "Linux/nothing.iso"));
    assert_null(manifest_find(NULL, 0, "a"));
    assert_null(manifest_find(r, n, NULL));

    /* The Section 8.1 rule: path and size agree, or the entry is stale. */
    assert_int_equal(manifest_lookup(r, n, "kubuntu-26.04-desktop-amd64.iso",
                                     5068847104u, &hit),
                     MANIFEST_BLESS_FRESH);
    assert_non_null(hit);
    assert_string_equal(hit->uki, "kubuntu-26.04.uki");

    assert_int_equal(manifest_lookup(r, n, "kubuntu-26.04-desktop-amd64.iso",
                                     5068847105u, &hit),
                     MANIFEST_BLESS_STALE);
    /* Stale still reports the record, so the menu can say which UKI went
     * out of date rather than just that something did. */
    assert_non_null(hit);

    assert_int_equal(manifest_lookup(r, n, "never-blessed.iso", 1, &hit),
                     MANIFEST_BLESS_NONE);
    assert_null(hit);
}

static void test_manifest_path_from_ucs2(void **state)
{
    static const uint16_t ok[] = { 'L', 'i', 'n', 'u', 'x', '\\',
                                   'a', '.', 'i', 's', 'o', 0 };
    static const uint16_t accented[] = { 'c', 'a', 'f', 0x00E9, 0 };
    char buf[32];

    (void)state;
    assert_int_equal(manifest_path_from_ucs2(ok, buf, sizeof(buf)),
                     MANIFEST_OK);
    assert_string_equal(buf, "Linux\\a.iso");
    /* And it compares equal to the forward-slash form a manifest carries. */
    assert_true(manifest_path_eq(buf, "Linux/a.iso"));

    /* Non-ASCII is refused rather than transliterated: no manifest record
     * can name such a path, so the honest answer is "not blessed". */
    assert_int_equal(manifest_path_from_ucs2(accented, buf, sizeof(buf)),
                     MANIFEST_ERR_BAD_ARG);
    /* Too small a buffer is the same answer, never a truncated path. */
    assert_int_equal(manifest_path_from_ucs2(ok, buf, 4u),
                     MANIFEST_ERR_BAD_ARG);
    assert_int_equal(manifest_path_from_ucs2(NULL, buf, sizeof(buf)),
                     MANIFEST_ERR_BAD_ARG);
    assert_int_equal(manifest_path_from_ucs2(ok, buf, 0),
                     MANIFEST_ERR_BAD_ARG);
}

/* -------------------------------------------------------------- writer */

static void fill(manifest_record *r, const char *iso, uint64_t size,
                 const char *uki, const char *family, const char *sha)
{
    memset(r, 0, sizeof(*r));
    (void)snprintf(r->iso, sizeof(r->iso), "%s", iso);
    (void)snprintf(r->uki, sizeof(r->uki), "%s", uki);
    (void)snprintf(r->family, sizeof(r->family), "%s", family);
    r->size = size;
    if (sha != NULL) {
        (void)snprintf(r->sha256, sizeof(r->sha256), "%s", sha);
        r->has_sha256 = 1;
    }
}

#define SHA_A \
    "1111111111111111111111111111111111111111111111111111111111111111"
#define SHA_B \
    "2222222222222222222222222222222222222222222222222222222222222222"

static void test_manifest_round_trip(void **state)
{
    manifest_record in[3];
    manifest_record out[CAP];
    char text[4096];
    size_t used = 0;
    size_t n;
    size_t i;

    (void)state;
    fill(&in[0], "Linux/arch.iso", 1538670592u, "arch.uki", "arch", SHA_A);
    fill(&in[1], "kubuntu.iso", 5068847104u, "kubuntu.uki", "casper", SHA_B);
    /* No family, no hash: both fields are optional, and a round trip must
     * not invent them. */
    fill(&in[2], "sysresccd.iso", 1u, "sysresccd.uki", "", NULL);

    assert_int_equal(manifest_render(in, 3, text, sizeof(text), &used),
                     MANIFEST_OK);
    assert_true(used > 0 && used < sizeof(text));

    n = 0;
    assert_int_equal(manifest_parse(text, used, out, CAP, &n), MANIFEST_OK);
    assert_int_equal(n, 3);
    for (i = 0; i < 3; i++) {
        assert_string_equal(out[i].iso, in[i].iso);
        assert_string_equal(out[i].uki, in[i].uki);
        assert_string_equal(out[i].family, in[i].family);
        assert_int_equal(out[i].has_sha256, in[i].has_sha256);
        assert_string_equal(out[i].sha256, in[i].sha256);
        assert_true(out[i].size == in[i].size);
    }
}

static void test_manifest_render_empty_and_bounds(void **state)
{
    manifest_record r;
    char text[4096];
    char tiny[8];
    size_t used = 0;
    size_t n = 99;

    (void)state;
    /* Zero records still renders the header, and parses back to zero. The
     * output is not NUL-terminated (render counts bytes, it does not
     * terminate), which is exactly how the loader will hand it back, so the
     * length has to come from used and not from strlen. */
    assert_int_equal(manifest_render(NULL, 0, text, sizeof(text), &used),
                     MANIFEST_OK);
    assert_true(used > 0);
    assert_int_equal(manifest_parse(text, used, &r, 1u, &n), MANIFEST_OK);
    assert_int_equal(n, 0);

    /* Too small an output buffer reports what it would have needed rather
     * than writing a half-record. */
    fill(&r, "a.iso", 1, "a.uki", "arch", SHA_A);
    assert_int_equal(manifest_render(&r, 1, tiny, sizeof(tiny), &used),
                     MANIFEST_ERR_NOSPACE);
    assert_true(used > sizeof(tiny));

    assert_int_equal(manifest_render(&r, 1, text, sizeof(text), NULL),
                     MANIFEST_ERR_BAD_ARG);
}

static void test_manifest_record_valid(void **state)
{
    manifest_record r;

    (void)state;
    fill(&r, "a.iso", 1, "a.uki", "arch", SHA_A);
    assert_true(manifest_record_valid(&r));

    /* '#' would comment out the rest of the line and the newline would split
     * the record; both are caught before anything reaches the stick. */
    fill(&r, "a#b.iso", 1, "a.uki", "arch", SHA_A);
    assert_false(manifest_record_valid(&r));
    fill(&r, "a\nb.iso", 1, "a.uki", "arch", SHA_A);
    assert_false(manifest_record_valid(&r));
    fill(&r, "caf\xC3\xA9.iso", 1, "a.uki", "arch", SHA_A);
    assert_false(manifest_record_valid(&r));

    fill(&r, "", 1, "a.uki", "arch", SHA_A);
    assert_false(manifest_record_valid(&r));
    fill(&r, "a.iso", 1, "", "arch", SHA_A);
    assert_false(manifest_record_valid(&r));
    /* An empty family is allowed; an unset hash is too. */
    fill(&r, "a.iso", 1, "a.uki", "", NULL);
    assert_true(manifest_record_valid(&r));

    /* has_sha256 set but the digest is not one. */
    fill(&r, "a.iso", 1, "a.uki", "arch", "abc");
    assert_false(manifest_record_valid(&r));
    fill(&r, "a.iso", 1, "a.uki", "arch",
         "gggggggggggggggggggggggggggggggg"
         "gggggggggggggggggggggggggggggggg");
    assert_false(manifest_record_valid(&r));

    assert_false(manifest_record_valid(NULL));
}

/* The rejection paths the happy cases never reach. Each one is a way a
 * record could get onto the stick malformed, which would cost a boot to
 * discover; they are cheap to pin here. */
static void test_manifest_writer_rejections(void **state)
{
    manifest_record r;
    manifest_record pair[2];
    char text[4096];
    size_t used = 0;
    size_t n = 0;

    (void)state;
    /* A field with no terminator inside its array. Nothing in the tool can
     * produce this, which is exactly why the writer checks for it rather
     * than trusting the struct. */
    fill(&r, "a.iso", 1, "a.uki", "arch", SHA_A);
    memset(r.iso, 'a', sizeof(r.iso));
    assert_false(manifest_record_valid(&r));

    fill(&r, "a.iso", 1, "a.uki", "arch", SHA_A);
    memset(r.family, 'f', sizeof(r.family));
    assert_false(manifest_record_valid(&r));

    /* Past what the format's size field may hold. */
    fill(&r, "a.iso", 1, "a.uki", "arch", SHA_A);
    r.size = (uint64_t)MANIFEST_SIZE_MAX + 1u;
    assert_false(manifest_record_valid(&r));

    /* A 64-character digest that is the right length and not hex. */
    n = 0;
    assert_int_equal(manifest_parse(
        "iso=a.iso\nsize=1\nuki=a.uki\n"
        "sha256=zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"
        "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\n", 96u + 26u, &r, 1u, &n),
        MANIFEST_OK);
    assert_int_equal(n, 1);
    assert_int_equal(r.has_sha256, 0);

    /* render() refuses the whole file rather than writing a record it knows
     * is unreadable: a partial manifest is worse than an unchanged one. */
    fill(&pair[0], "a.iso", 1, "a.uki", "arch", SHA_A);
    fill(&pair[1], "b#.iso", 1, "b.uki", "arch", SHA_A);
    assert_int_equal(manifest_render(pair, 2, text, sizeof(text), &used),
                     MANIFEST_ERR_BAD_RECORD);

    /* A zero-byte ISO is nonsense but representable, and the decimal
     * renderer has to spell it. */
    fill(&pair[0], "empty.iso", 0, "empty.uki", "", NULL);
    assert_int_equal(manifest_render(pair, 1, text, sizeof(text), &used),
                     MANIFEST_OK);
    n = 0;
    assert_int_equal(manifest_parse(text, used, &r, 1u, &n), MANIFEST_OK);
    assert_int_equal(n, 1);
    assert_true(r.size == 0);
}

static void test_manifest_upsert_replaces_in_place(void **state)
{
    manifest_record recs[CAP];
    manifest_record fresh;
    size_t n;

    (void)state;
    n = parse_ok(g_two, recs, CAP);
    assert_int_equal(n, 2);

    /* Re-blessing the first ISO after it was replaced on the stick: same
     * path, new size, new hash, and a new UKI name. It must land on the
     * existing record and leave the second one exactly where it was. */
    fill(&fresh, "linux/ARCHLINUX-2026.01.01-X86_64.iso", 99u,
         "arch-rebuilt.uki", "arch", SHA_B);
    assert_int_equal(manifest_upsert(recs, &n, CAP, &fresh), MANIFEST_OK);
    assert_int_equal(n, 2);
    assert_string_equal(recs[0].uki, "arch-rebuilt.uki");
    assert_true(recs[0].size == 99u);
    assert_string_equal(recs[0].sha256, SHA_B);
    assert_string_equal(recs[1].uki, "kubuntu-26.04.uki");

    /* A different ISO appends. */
    fill(&fresh, "fedora.iso", 7u, "fedora.uki", "fedora", SHA_A);
    assert_int_equal(manifest_upsert(recs, &n, CAP, &fresh), MANIFEST_OK);
    assert_int_equal(n, 3);
    assert_string_equal(recs[2].iso, "fedora.iso");
}

static void test_manifest_upsert_bounds(void **state)
{
    manifest_record recs[2];
    manifest_record r;
    size_t n = 0;

    (void)state;
    fill(&r, "a.iso", 1, "a.uki", "arch", SHA_A);
    assert_int_equal(manifest_upsert(recs, &n, 2u, &r), MANIFEST_OK);
    fill(&r, "b.iso", 1, "b.uki", "arch", SHA_A);
    assert_int_equal(manifest_upsert(recs, &n, 2u, &r), MANIFEST_OK);
    fill(&r, "c.iso", 1, "c.uki", "arch", SHA_A);
    assert_int_equal(manifest_upsert(recs, &n, 2u, &r), MANIFEST_ERR_NOSPACE);
    assert_int_equal(n, 2);

    /* A full array still accepts a replacement of something already in it. */
    fill(&r, "b.iso", 5, "b2.uki", "arch", SHA_B);
    assert_int_equal(manifest_upsert(recs, &n, 2u, &r), MANIFEST_OK);
    assert_string_equal(recs[1].uki, "b2.uki");

    /* An unrepresentable record is refused before it can reach the file. */
    fill(&r, "d#.iso", 1, "d.uki", "arch", SHA_A);
    assert_int_equal(manifest_upsert(recs, &n, 2u, &r),
                     MANIFEST_ERR_BAD_RECORD);
    assert_int_equal(manifest_upsert(NULL, &n, 2u, &r), MANIFEST_ERR_BAD_ARG);
    assert_int_equal(manifest_upsert(recs, NULL, 2u, &r),
                     MANIFEST_ERR_BAD_ARG);
    assert_int_equal(manifest_upsert(recs, &n, 2u, NULL),
                     MANIFEST_ERR_BAD_ARG);
}

/* A re-bless end to end, in the shape the sign tool performs it: parse the
 * file, upsert, render, and parse the result. The record count must not
 * grow and the surviving record must be the new one. */
static void test_manifest_rebless_cycle(void **state)
{
    manifest_record recs[CAP];
    manifest_record after[CAP];
    manifest_record fresh;
    char text[8192];
    size_t used = 0;
    size_t n;
    size_t m = 0;

    (void)state;
    n = parse_ok(g_two, recs, CAP);
    fill(&fresh, "Linux/archlinux-2026.01.01-x86_64.iso", 4242u,
         "archlinux-2026.01.01-x86_64.uki", "arch", SHA_B);
    assert_int_equal(manifest_upsert(recs, &n, CAP, &fresh), MANIFEST_OK);
    assert_int_equal(manifest_render(recs, n, text, sizeof(text), &used),
                     MANIFEST_OK);
    assert_int_equal(manifest_parse(text, used, after, CAP, &m), MANIFEST_OK);

    assert_int_equal(m, 2);
    assert_true(after[0].size == 4242u);
    assert_string_equal(after[0].sha256, SHA_B);
    assert_string_equal(after[1].iso, "kubuntu-26.04-desktop-amd64.iso");

    /* And the old record is gone, not merely shadowed. */
    assert_int_equal(manifest_lookup(after, m,
                                     "Linux/archlinux-2026.01.01-x86_64.iso",
                                     1538670592u, NULL),
                     MANIFEST_BLESS_STALE);
}

/* ------------------------------------------------------------------ run */

int run_manifest_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_manifest_two_records),
        cmocka_unit_test(test_manifest_empty),
        cmocka_unit_test(test_manifest_comments_only),
        cmocka_unit_test(test_manifest_crlf),
        cmocka_unit_test(test_manifest_whitespace_and_unknown_keys),
        cmocka_unit_test(test_manifest_no_trailing_newline),
        cmocka_unit_test(test_manifest_keys_before_first_iso),
        cmocka_unit_test(test_manifest_missing_keys_skipped),
        cmocka_unit_test(test_manifest_bad_size_skipped),
        cmocka_unit_test(test_manifest_non_ascii_path_skipped),
        cmocka_unit_test(test_manifest_overlong_values_skipped),
        cmocka_unit_test(test_manifest_bad_sha256_keeps_record),
        cmocka_unit_test(test_manifest_truncated_tail),
        cmocka_unit_test(test_manifest_huge_line),
        cmocka_unit_test(test_manifest_no_nul_needed),
        cmocka_unit_test(test_manifest_nospace),
        cmocka_unit_test(test_manifest_bad_args),
        cmocka_unit_test(test_manifest_path_eq),
        cmocka_unit_test(test_manifest_find_and_lookup),
        cmocka_unit_test(test_manifest_path_from_ucs2),
        cmocka_unit_test(test_manifest_round_trip),
        cmocka_unit_test(test_manifest_render_empty_and_bounds),
        cmocka_unit_test(test_manifest_record_valid),
        cmocka_unit_test(test_manifest_writer_rejections),
        cmocka_unit_test(test_manifest_upsert_replaces_in_place),
        cmocka_unit_test(test_manifest_upsert_bounds),
        cmocka_unit_test(test_manifest_rebless_cycle),
    };

    return cmocka_run_group_tests_name("blessed-artifact manifest", tests,
                                       NULL, NULL);
}
