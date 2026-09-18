/* Tests for src/bless/manifest.c, the blessed-artifact manifest of docs/DESIGN.md
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
 *
 * The kind= extension adds a fifth, which is really the first one again from
 * a different direction: per-kind validity. A prepared record has no uki= and
 * must still become a record (the old one-rule-for-everyone parser dropped it
 * without a word), a blessed one must still be refused without it, an absent
 * kind= must still mean blessed so every manifest written before the key
 * existed keeps parsing, and a splice list that is too long, too short of
 * fields, or attached to the wrong kind must cost the record.
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

#include <bless/manifest.h>

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

#define SHA_A \
    "1111111111111111111111111111111111111111111111111111111111111111"
#define SHA_B \
    "2222222222222222222222222222222222222222222222222222222222222222"

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

/* ------------------------------------------------------------- kinds */

/* The shape a prepared Windows ISO gets, per DESIGN Sections 6.3 and 8.1:
 * no uki= anywhere, a hash that doubles as the sidecar folder's name, and a
 * byte map. Under the pre-kind parser this block produced nothing at all. */
static const char g_prepared[] =
    "iso=Windows/Win11_25H2_x64.iso\n"
    "kind=prepared\n"
    "size=7218589696\n"
    "sha256=3333333333333333333333333333333333333333333333333333333333333333\n"
    "splice=22095872:593121462:boot.wim\n"
    "family=windows\n";

static void test_manifest_prepared_record(void **state)
{
    manifest_record r[CAP];

    (void)state;
    assert_int_equal(parse_ok(g_prepared, r, CAP), 1);
    assert_int_equal(r[0].kind, MANIFEST_KIND_PREPARED);
    assert_string_equal(r[0].iso, "Windows/Win11_25H2_x64.iso");
    assert_true(r[0].size == 7218589696u);
    assert_int_equal(r[0].has_sha256, 1);
    assert_string_equal(r[0].family, "windows");
    /* No uki, and that is the whole point: the record exists anyway. */
    assert_string_equal(r[0].uki, "");
    assert_int_equal(r[0].splice_n, 1);
    assert_true(r[0].splice[0].iso_off == 22095872u);
    assert_true(r[0].splice[0].len == 593121462u);
    assert_string_equal(r[0].splice[0].name, "boot.wim");
}

static void test_manifest_absent_kind_is_blessed(void **state)
{
    manifest_record r[CAP];

    (void)state;
    /* Back-compatibility, stated as a test because it is the reason the key
     * is optional: every manifest written before kind= existed must parse to
     * exactly what it parsed to then. */
    assert_int_equal(parse_ok(g_two, r, CAP), 2);
    assert_int_equal(r[0].kind, MANIFEST_KIND_BLESSED);
    assert_int_equal(r[1].kind, MANIFEST_KIND_BLESSED);
    assert_int_equal(r[0].splice_n, 0);

    /* Spelled out, and spelled out in the wrong case, mean the same thing.
     * Hand-edited files come from three operating systems. */
    assert_int_equal(parse_ok("iso=a.iso\nkind=blessed\nsize=1\nuki=a.uki\n",
                              r, CAP), 1);
    assert_int_equal(r[0].kind, MANIFEST_KIND_BLESSED);
    assert_int_equal(parse_ok("iso=a.iso\nKIND=Prepared\nsize=1\n"
                              "sha256=" SHA_A "\n", r, CAP), 1);
    assert_int_equal(r[0].kind, MANIFEST_KIND_PREPARED);
}

static void test_manifest_kind_validity_is_per_kind(void **state)
{
    manifest_record r[CAP];

    (void)state;
    /* A prepared record needs no uki and must not be dropped for lacking
     * one; a blessed record still is. */
    assert_int_equal(parse_ok("iso=a.iso\nkind=prepared\nsize=1\n"
                              "sha256=" SHA_A "\n", r, CAP), 1);
    assert_int_equal(parse_ok("iso=a.iso\nkind=blessed\nsize=1\n"
                              "sha256=" SHA_A "\n", r, CAP), 0);

    /* sha256= names the prepared folder, so a prepared record without a
     * usable one has nowhere to resolve its splices and is dropped. A
     * blessed record with the same mangled hash survives, because nothing
     * reads it. */
    assert_int_equal(parse_ok("iso=a.iso\nkind=prepared\nsize=1\n", r, CAP), 0);
    assert_int_equal(parse_ok("iso=a.iso\nkind=prepared\nsize=1\n"
                              "sha256=zzz\n", r, CAP), 0);
    assert_int_equal(parse_ok("iso=a.iso\nsize=1\nuki=a.uki\nsha256=zzz\n",
                              r, CAP), 1);

    /* Both kinds still need the keys neither can do without. */
    assert_int_equal(parse_ok("iso=a.iso\nkind=prepared\n"
                              "sha256=" SHA_A "\n", r, CAP), 0);
    assert_int_equal(parse_ok("iso=\nkind=prepared\nsize=1\n"
                              "sha256=" SHA_A "\n", r, CAP), 0);

    /* A splice on a blessed record is a contradiction: there is no byte map
     * to apply it to, so it is malformed rather than ignored. The key order
     * must not matter, since kind= may arrive after the splice. */
    assert_int_equal(parse_ok("iso=a.iso\nsize=1\nuki=a.uki\n"
                              "splice=0:4:x.bin\n", r, CAP), 0);
    assert_int_equal(parse_ok("iso=a.iso\nsplice=0:4:x.bin\nsize=1\n"
                              "kind=prepared\nsha256=" SHA_A "\n", r, CAP), 1);

    /* A kind nobody knows is a record nobody should act on. */
    assert_int_equal(parse_ok("iso=a.iso\nkind=frobnicated\nsize=1\n"
                              "uki=a.uki\n", r, CAP), 0);
    assert_int_equal(parse_ok("iso=a.iso\nkind=\nsize=1\nuki=a.uki\n",
                              r, CAP), 0);
    /* "blessedish" must not match on a prefix. */
    assert_int_equal(parse_ok("iso=a.iso\nkind=blessedish\nsize=1\n"
                              "uki=a.uki\n", r, CAP), 0);
}

static void test_manifest_splices(void **state)
{
    char text[4096];
    manifest_record r[CAP];
    size_t w;
    size_t i;
    size_t n;

    (void)state;
    /* Several splices, in file order, which is the order a byte map has to
     * be applied in. */
    assert_int_equal(parse_ok("iso=a.iso\nkind=prepared\nsize=99\n"
                              "sha256=" SHA_A "\n"
                              "splice=0:1:first.bin\n"
                              "splice=4096:2048:second.bin\n", r, CAP), 1);
    assert_int_equal(r[0].splice_n, 2);
    assert_true(r[0].splice[0].len == 1u);
    assert_string_equal(r[0].splice[1].name, "second.bin");
    assert_true(r[0].splice[1].iso_off == 4096u);

    /* Exactly at the cap, then one past it. The record is dropped whole at
     * one past: an ISO served with part of its byte map applied is worse
     * than one served with none of it. */
    w = (size_t)snprintf(text, sizeof(text),
                         "iso=a.iso\nkind=prepared\nsize=99\nsha256=" SHA_A
                         "\n");
    for (i = 0; i < MANIFEST_SPLICE_MAX; i++)
        w += (size_t)snprintf(text + w, sizeof(text) - w,
                              "splice=%u:16:p%u.bin\n", (unsigned)(i * 16u),
                              (unsigned)i);
    assert_int_equal(parse_ok(text, r, CAP), 1);
    assert_int_equal(r[0].splice_n, MANIFEST_SPLICE_MAX);
    assert_string_equal(r[0].splice[MANIFEST_SPLICE_MAX - 1u].name, "p7.bin");

    (void)snprintf(text + w, sizeof(text) - w, "splice=999:16:over.bin\n");
    assert_int_equal(parse_ok(text, r, CAP), 0);

    /* The longest name the field holds, and one byte more. */
    w = (size_t)snprintf(text, sizeof(text),
                         "iso=a.iso\nkind=prepared\nsize=99\nsha256=" SHA_A
                         "\nsplice=0:16:");
    for (i = 0; i < MANIFEST_SPLICE_NAME_MAX - 1u; i++)
        text[w + i] = 'n';
    text[w + i] = '\n';
    assert_int_equal(manifest_parse(text, w + i + 1u, r, CAP, &n),
                     MANIFEST_OK);
    assert_int_equal(n, 1);
    assert_int_equal(strlen(r[0].splice[0].name), MANIFEST_SPLICE_NAME_MAX - 1u);

    text[w + MANIFEST_SPLICE_NAME_MAX - 1u] = 'n';
    text[w + MANIFEST_SPLICE_NAME_MAX] = '\n';
    assert_int_equal(manifest_parse(text, w + MANIFEST_SPLICE_NAME_MAX + 1u,
                                    r, CAP, &n), MANIFEST_OK);
    assert_int_equal(n, 0);
}

static void test_manifest_bad_splices_skipped(void **state)
{
    static const char *const bad[] = {
        "splice=22095872\n",                  /* no colon at all */
        "splice=22095872:593121462\n",        /* only one colon */
        "splice=\n",                          /* nothing whatever */
        "splice=0:16:\n",                     /* empty name */
        "splice=x:16:a.bin\n",                /* offset is not a number */
        "splice=0:sixteen:a.bin\n",           /* length is not a number */
        "splice=0x10:16:a.bin\n",             /* nor is hex */
        "splice=-1:16:a.bin\n",
        "splice=:16:a.bin\n",                 /* empty offset */
        "splice=0::a.bin\n",                  /* empty length */
        "splice=0:0:a.bin\n",                 /* nothing to serve */
        /* 2^63, one past what either number may hold. */
        "splice=9223372036854775808:16:a.bin\n",
        "splice=0:9223372036854775808:a.bin\n",
        "splice=99999999999999999999999999:16:a.bin\n",
        /* In range separately, past it added together: the end offset is
         * what a consumer computes, so the pair is bounded here. */
        "splice=9223372036854775807:1:a.bin\n",
        "splice=9223372036854775800:8:a.bin\n",
        /* A name the loader could not compare as bytes. */
        "splice=0:16:caf\xC3\xA9.bin\n",
    };
    char text[512];
    manifest_record r[CAP];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        (void)snprintf(text, sizeof(text),
                       "iso=a.iso\nkind=prepared\nsize=99\nsha256=" SHA_A
                       "\n%s", bad[i]);
        assert_int_equal(parse_ok(text, r, CAP), 0);
    }

    /* The boundary that is still legal: the end offset lands exactly on
     * MANIFEST_SIZE_MAX. */
    assert_int_equal(parse_ok("iso=a.iso\nkind=prepared\nsize=99\n"
                              "sha256=" SHA_A "\n"
                              "splice=9223372036854775799:8:a.bin\n",
                              r, CAP), 1);
    assert_true(r[0].splice[0].iso_off + r[0].splice[0].len
                == MANIFEST_SIZE_MAX);
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

/* The prepared kind through the writer and back. Two records of different
 * kinds in one file, because that is what a stick with both Linux and Windows
 * media holds and the kinds must not contaminate each other across the
 * render. */
static void test_manifest_prepared_round_trip(void **state)
{
    manifest_record in[3];
    manifest_record out[CAP];
    char text[4096];
    size_t used = 0;
    size_t n = 0;

    (void)state;
    fill(&in[0], "Linux/arch.iso", 1538670592u, "arch.uki", "arch", SHA_A);
    fill(&in[1], "Windows/Win11.iso", 7218589696u, "", "windows", SHA_B);
    in[1].kind = MANIFEST_KIND_PREPARED;
    in[1].splice_n = 2;
    in[1].splice[0].iso_off = 22095872u;
    in[1].splice[0].len = 593121462u;
    (void)snprintf(in[1].splice[0].name, sizeof(in[1].splice[0].name),
                   "%s", "boot.wim");
    in[1].splice[1].iso_off = 0;
    in[1].splice[1].len = 2048u;
    (void)snprintf(in[1].splice[1].name, sizeof(in[1].splice[1].name),
                   "%s", "catalog.bin");
    /* A prepared record with no byte map at all is degenerate but
     * representable, and the round trip must not invent one. */
    fill(&in[2], "Windows/Server.iso", 5u, "", "", SHA_A);
    in[2].kind = MANIFEST_KIND_PREPARED;

    assert_int_equal(manifest_render(in, 3, text, sizeof(text), &used),
                     MANIFEST_OK);
    assert_int_equal(manifest_parse(text, used, out, CAP, &n), MANIFEST_OK);
    assert_int_equal(n, 3);

    assert_int_equal(out[0].kind, MANIFEST_KIND_BLESSED);
    assert_string_equal(out[0].uki, "arch.uki");
    assert_int_equal(out[0].splice_n, 0);

    assert_int_equal(out[1].kind, MANIFEST_KIND_PREPARED);
    assert_string_equal(out[1].iso, "Windows/Win11.iso");
    assert_string_equal(out[1].uki, "");
    assert_string_equal(out[1].sha256, SHA_B);
    assert_true(out[1].size == 7218589696u);
    assert_int_equal(out[1].splice_n, 2);
    assert_true(out[1].splice[0].iso_off == 22095872u);
    assert_true(out[1].splice[0].len == 593121462u);
    assert_string_equal(out[1].splice[0].name, "boot.wim");
    assert_true(out[1].splice[1].iso_off == 0);
    assert_true(out[1].splice[1].len == 2048u);
    assert_string_equal(out[1].splice[1].name, "catalog.bin");

    assert_int_equal(out[2].kind, MANIFEST_KIND_PREPARED);
    assert_int_equal(out[2].splice_n, 0);
    assert_string_equal(out[2].family, "");

    /* A file with no prepared record in it must come out in the shape an
     * older reader already understands: no kind= key anywhere. */
    assert_int_equal(manifest_render(in, 1, text, sizeof(text), &used),
                     MANIFEST_OK);
    assert_true(used < sizeof(text));
    text[used] = '\0';
    assert_null(strstr(text, "kind="));
}

static void test_manifest_prepared_record_valid(void **state)
{
    manifest_record r;

    (void)state;
    fill(&r, "w.iso", 1, "", "windows", SHA_A);
    r.kind = MANIFEST_KIND_PREPARED;
    assert_true(manifest_record_valid(&r));

    /* A prepared record's hash names its folder, so it is required here in a
     * way it never is on a blessed one. */
    r.has_sha256 = 0;
    assert_false(manifest_record_valid(&r));

    /* A uki on a prepared record round-trips and is tolerated; a splice on a
     * blessed one has nothing to apply to and is not. */
    fill(&r, "w.iso", 1, "leftover.uki", "windows", SHA_A);
    r.kind = MANIFEST_KIND_PREPARED;
    assert_true(manifest_record_valid(&r));

    fill(&r, "a.iso", 1, "a.uki", "arch", SHA_A);
    r.splice_n = 1;
    r.splice[0].len = 4;
    (void)snprintf(r.splice[0].name, sizeof(r.splice[0].name), "%s", "x.bin");
    assert_false(manifest_record_valid(&r));

    /* A kind the format does not define cannot be written, the same way it
     * cannot be read. Nothing in the tool produces one; the writer is the
     * last place a stray cast could be caught. */
    fill(&r, "w.iso", 1, "", "windows", SHA_A);
    r.kind = (manifest_kind)99;
    assert_false(manifest_record_valid(&r));
}

static void test_manifest_splice_writer_rejections(void **state)
{
    manifest_record r;
    char text[4096];
    size_t used = 0;

    (void)state;
    /* Every shape the parser refuses, refused on the way out too, so a
     * record that cannot be read back never reaches the stick. */
    fill(&r, "w.iso", 1, "", "windows", SHA_A);
    r.kind = MANIFEST_KIND_PREPARED;
    r.splice_n = 1;
    r.splice[0].iso_off = 0;
    r.splice[0].len = 16;
    (void)snprintf(r.splice[0].name, sizeof(r.splice[0].name), "%s", "x.bin");
    assert_true(manifest_record_valid(&r));

    r.splice_n = MANIFEST_SPLICE_MAX + 1u;
    assert_false(manifest_record_valid(&r));
    r.splice_n = 1;

    r.splice[0].len = 0;
    assert_false(manifest_record_valid(&r));
    r.splice[0].len = 16;

    r.splice[0].iso_off = MANIFEST_SIZE_MAX - 15u;
    assert_false(manifest_record_valid(&r));
    r.splice[0].iso_off = 0;

    /* The two bytes the format itself cannot carry, in a splice name. */
    (void)snprintf(r.splice[0].name, sizeof(r.splice[0].name), "%s", "x#.bin");
    assert_false(manifest_record_valid(&r));
    (void)snprintf(r.splice[0].name, sizeof(r.splice[0].name), "%s", "x\n.bin");
    assert_false(manifest_record_valid(&r));
    (void)snprintf(r.splice[0].name, sizeof(r.splice[0].name), "%s", "");
    assert_false(manifest_record_valid(&r));
    memset(r.splice[0].name, 'n', sizeof(r.splice[0].name));
    assert_false(manifest_record_valid(&r));

    /* And render refuses the whole file for it, as it does for every other
     * unrepresentable record. */
    assert_int_equal(manifest_render(&r, 1, text, sizeof(text), &used),
                     MANIFEST_ERR_BAD_RECORD);
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
        cmocka_unit_test(test_manifest_prepared_record),
        cmocka_unit_test(test_manifest_absent_kind_is_blessed),
        cmocka_unit_test(test_manifest_kind_validity_is_per_kind),
        cmocka_unit_test(test_manifest_splices),
        cmocka_unit_test(test_manifest_bad_splices_skipped),
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
        cmocka_unit_test(test_manifest_prepared_round_trip),
        cmocka_unit_test(test_manifest_prepared_record_valid),
        cmocka_unit_test(test_manifest_splice_writer_rejections),
        cmocka_unit_test(test_manifest_upsert_replaces_in_place),
        cmocka_unit_test(test_manifest_upsert_bounds),
        cmocka_unit_test(test_manifest_rebless_cycle),
    };

    return cmocka_run_group_tests_name("blessed-artifact manifest", tests,
                                       NULL, NULL);
}
