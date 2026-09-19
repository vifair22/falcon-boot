/* Tests for the next-boot marker half of falcon-efi/src/bless/blesscore.c
 * (docs/DESIGN.md Section 6.3.1), and for what a record of the retired
 * prepared kind means to the loader now that it patches Windows media itself
 * (Section 6.4): the manifest parser skips it, so its ISO boots plain.
 *
 * The marker is NVRAM, writable by anything with boot services on that
 * machine and by something that has never seen this stick. It is decoded,
 * never believed: the hostile shapes below all have to end at "no marker",
 * and the value is only ever compared against paths the scan found.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>

#include <bless/blesscore.h>

static const char g_vol[] = "p2";

/* A prepared record as an appliance from before Section 6.4 wrote it, and a
 * blessed record beside it. */
static const char g_manifest[] =
    "# falcon-boot manifest\n"
    "\n"
    "iso=Windows/Win11_25H2_x64.iso\n"
    "kind=prepared\n"
    "size=7218589696\n"
    "sha256=abcdef0123456789abcdef0123456789"
    "abcdef0123456789abcdef0123456789\n"
    "splice=22095872:593121462:boot.wim\n"
    "family=windows\n"
    "\n"
    "iso=Linux/arch.iso\n"
    "size=1538670592\n"
    "uki=arch.uki\n"
    "family=arch\n";

#define WIN11_SIZE 7218589696u
#define WIN11_PATH "Windows/Win11_25H2_x64.iso"

static bless_cache g_cache;

static int setup_cache(void **state)
{
    (void)state;
    bless_cache_init(&g_cache);
    (void)bless_cache_add(&g_cache, g_manifest, strlen(g_manifest), g_vol);
    return 0;
}

static void widen(uint16_t *dst, size_t cap, const char *src)
{
    size_t i = 0;

    assert_true(cap > 0);
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = (uint16_t)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

/* ------------------------------------------------- a retired prepared record */

/* It boots as if it were not there, with no label: the loader patches the
 * ISO itself as it serves it. The blessed record beside it is unaffected. */
static void test_prepared_record_is_plain(void **state)
{
    uint16_t wide[512];
    bless_hit hit;

    (void)state;
    widen(wide, sizeof(wide) / sizeof(wide[0]), WIN11_PATH);
    assert_int_equal(bless_cache_lookup(&g_cache, g_vol, wide, WIN11_SIZE,
                                        &hit), BLESS_NONE);
    assert_int_equal(bless_hit_action(&hit), BLESS_ACT_PLAIN);
    assert_null(bless_suffix(&hit));
    assert_null(bless_suffix(NULL));
    assert_int_equal(bless_hit_action(NULL), BLESS_ACT_PLAIN);

    widen(wide, sizeof(wide) / sizeof(wide[0]), "Linux/arch.iso");
    assert_int_equal(bless_cache_lookup(&g_cache, g_vol, wide, 1538670592u,
                                        &hit), BLESS_FRESH);
    assert_int_equal(bless_hit_action(&hit), BLESS_ACT_UKI);
    assert_non_null(bless_suffix(&hit));
}

/* ------------------------------------------------------- the next-boot value */

static void test_next_decode_plain(void **state)
{
    char out[BLESS_NEXT_MAX];

    (void)state;
    /* With the terminator, which is what this loader writes. */
    assert_int_equal(bless_next_decode(WIN11_PATH, sizeof WIN11_PATH, out,
                                       sizeof out), BLESS_OK);
    assert_string_equal(out, WIN11_PATH);

    /* And without it, which is what a variable editor is likely to write. */
    assert_int_equal(bless_next_decode(WIN11_PATH, strlen(WIN11_PATH), out,
                                       sizeof out), BLESS_OK);
    assert_string_equal(out, WIN11_PATH);
}

static void test_next_decode_empty(void **state)
{
    char out[BLESS_NEXT_MAX];

    (void)state;
    assert_int_equal(bless_next_decode("", 0, out, sizeof out),
                     BLESS_ERR_VALUE);
    assert_string_equal(out, "");
    /* A lone terminator is an empty value, not a path to anything. */
    assert_int_equal(bless_next_decode("", 1, out, sizeof out),
                     BLESS_ERR_VALUE);
    assert_string_equal(out, "");
}

static void test_next_decode_oversized(void **state)
{
    char big[BLESS_NEXT_MAX + 64];
    char out[BLESS_NEXT_MAX];

    (void)state;
    memset(big, 'a', sizeof big);
    assert_int_equal(bless_next_decode(big, sizeof big, out, sizeof out),
                     BLESS_ERR_VALUE);
    assert_string_equal(out, "");
    /* Exactly one character past what fits, terminator included. */
    assert_int_equal(bless_next_decode(big, BLESS_NEXT_MAX, out, sizeof out),
                     BLESS_ERR_VALUE);
    /* And the longest value that does fit. */
    assert_int_equal(bless_next_decode(big, BLESS_NEXT_MAX - 1u, out,
                                       sizeof out), BLESS_OK);
    assert_int_equal(strlen(out), BLESS_NEXT_MAX - 1u);
}

/* Bytes outside printable ASCII cannot name a record: manifest_parse drops
 * any record carrying one, so a value with one has nothing to match. */
static void test_next_decode_non_ascii(void **state)
{
    static const unsigned char utf8[] = { 'W', 'i', 'n', 0xC3, 0xA9, '.',
                                          'i', 's', 'o', 0 };
    static const unsigned char ctrl[] = { 'a', '\n', 'b', 0 };
    static const unsigned char tab[] = { 'a', '\t', 'b', 0 };
    char out[BLESS_NEXT_MAX];

    (void)state;
    assert_int_equal(bless_next_decode(utf8, sizeof utf8, out, sizeof out),
                     BLESS_ERR_VALUE);
    assert_int_equal(bless_next_decode(ctrl, sizeof ctrl, out, sizeof out),
                     BLESS_ERR_VALUE);
    assert_int_equal(bless_next_decode(tab, sizeof tab, out, sizeof out),
                     BLESS_ERR_VALUE);
    assert_string_equal(out, "");
}

/* An embedded NUL is refused rather than taken as a terminator, so no two
 * readers of the same value can disagree about where it ends. */
static void test_next_decode_embedded_nul(void **state)
{
    static const char split[] = "a.iso\0b.iso";
    char out[BLESS_NEXT_MAX];

    (void)state;
    assert_int_equal(bless_next_decode(split, sizeof split - 1u, out,
                                       sizeof out), BLESS_ERR_VALUE);
    assert_string_equal(out, "");
}

static void test_next_decode_null_args(void **state)
{
    char out[BLESS_NEXT_MAX];

    (void)state;
    assert_int_equal(bless_next_decode(NULL, 4, out, sizeof out),
                     BLESS_ERR_VALUE);
    assert_int_equal(bless_next_decode("x", 1, NULL, sizeof out),
                     BLESS_ERR_BAD_ARG);
    assert_int_equal(bless_next_decode("x", 1, out, 0), BLESS_ERR_BAD_ARG);
}

/* The value is only ever compared against a path the scan found, and the
 * comparison is the manifest's own: case-insensitive, either separator. */
static void test_next_names(void **state)
{
    uint16_t wide[512];

    (void)state;
    widen(wide, sizeof(wide) / sizeof(wide[0]), WIN11_PATH);
    assert_true(bless_next_names(wide, WIN11_PATH));
    assert_true(bless_next_names(wide, "windows/win11_25h2_x64.iso"));
    assert_true(bless_next_names(wide, "/Windows/Win11_25H2_x64.iso"));
    assert_false(bless_next_names(wide, "Windows/Win10.iso"));
    assert_false(bless_next_names(wide, ""));
    assert_false(bless_next_names(wide, NULL));
    assert_false(bless_next_names(NULL, WIN11_PATH));

    /* An exFAT path arrives backslash-separated on a stick written by
     * Windows; it names the same file. */
    widen(wide, sizeof(wide) / sizeof(wide[0]),
          "Windows\\Win11_25H2_x64.iso");
    assert_true(bless_next_names(wide, WIN11_PATH));
}

/* A path no record could carry cannot be named by a marker either, and
 * answering that without searching is what keeps the comparison total. */
static void test_next_names_non_ascii_path(void **state)
{
    uint16_t wide[8];

    (void)state;
    wide[0] = (uint16_t)'a';
    wide[1] = 0x00E9u;   /* e-acute: no manifest record may carry it */
    wide[2] = 0;
    assert_false(bless_next_names(wide, "a?"));
}

/* The marker names an entry; booting it is then the menu's own decision, so
 * the rule here is only whether the decoded value names the entry in hand.
 * Nothing indexes anything with the value, so every hostile shape can only
 * fail to match. */
static int names_entry(const char *marker, const char *path)
{
    char decoded[BLESS_NEXT_MAX];
    uint16_t wide[512];

    if (bless_next_decode(marker, strlen(marker) + 1u, decoded,
                          sizeof decoded) != BLESS_OK)
        return 0;
    widen(wide, sizeof(wide) / sizeof(wide[0]), path);
    return bless_next_names(wide, decoded);
}

static void test_next_rule(void **state)
{
    (void)state;
    assert_true(names_entry(WIN11_PATH, WIN11_PATH));
    assert_true(names_entry("Linux/arch.iso", "Linux/arch.iso"));
    /* The marker names something that is not the entry in hand. */
    assert_false(names_entry("Windows/Win10.iso", WIN11_PATH));
    /* Hostile shapes, none of which reach a lookup at all. */
    assert_false(names_entry("", WIN11_PATH));
    assert_false(names_entry("../../etc/shadow", WIN11_PATH));
}

/* The reserved namespace. ':' is the load-bearing character: exFAT forbids it
 * in a filename, so no path the scan builds can ever land in here. */
static void test_next_reserved_personal(void **state)
{
    (void)state;
    assert_true(bless_next_is_reserved(BLESS_NEXT_PERSONAL));
    assert_true(bless_next_is_personal(BLESS_NEXT_PERSONAL));
}

/* A reserved value this build does not know is reserved but not personal, so
 * the loader refuses it rather than looking it up as a path. */
static void test_next_reserved_unknown(void **state)
{
    (void)state;
    assert_true(bless_next_is_reserved(":whatever"));
    assert_false(bless_next_is_personal(":whatever"));
}

/* Ordinary paths are not reserved, and neither is the '@personal' spelling
 * that was considered and rejected: a file may legally be called that. Only a
 * LEADING colon reserves, which keeps the rule checkable from one byte. */
static void test_next_reserved_rejects_paths(void **state)
{
    (void)state;
    assert_false(bless_next_is_reserved(WIN11_PATH));
    assert_false(bless_next_is_reserved("Linux/arch.iso"));
    assert_false(bless_next_is_reserved("@personal"));
    assert_false(bless_next_is_personal("@personal"));
    assert_false(bless_next_is_reserved(""));
}

/* Exact match, not a prefix. A prefix rule would make the namespace impossible
 * to extend without silently rebinding older markers. */
static void test_next_personal_is_exact(void **state)
{
    (void)state;
    assert_false(bless_next_is_personal(":personalish"));
    assert_false(bless_next_is_personal(":persona"));
    assert_false(bless_next_is_personal(":"));
}

static void test_next_reserved_null_args(void **state)
{
    (void)state;
    assert_false(bless_next_is_reserved(NULL));
    assert_false(bless_next_is_personal(NULL));
}

int run_next_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_prepared_record_is_plain, setup_cache),
        cmocka_unit_test_setup(test_next_decode_plain, setup_cache),
        cmocka_unit_test_setup(test_next_decode_empty, setup_cache),
        cmocka_unit_test_setup(test_next_decode_oversized, setup_cache),
        cmocka_unit_test_setup(test_next_decode_non_ascii, setup_cache),
        cmocka_unit_test_setup(test_next_decode_embedded_nul, setup_cache),
        cmocka_unit_test_setup(test_next_decode_null_args, setup_cache),
        cmocka_unit_test_setup(test_next_names, setup_cache),
        cmocka_unit_test_setup(test_next_names_non_ascii_path, setup_cache),
        cmocka_unit_test_setup(test_next_rule, setup_cache),
        cmocka_unit_test_setup(test_next_reserved_personal, setup_cache),
        cmocka_unit_test_setup(test_next_reserved_unknown, setup_cache),
        cmocka_unit_test_setup(test_next_reserved_rejects_paths, setup_cache),
        cmocka_unit_test_setup(test_next_personal_is_exact, setup_cache),
        cmocka_unit_test_setup(test_next_reserved_null_args, setup_cache),
    };

    return cmocka_run_group_tests_name("next", tests, NULL, NULL);
}
