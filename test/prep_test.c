/* Tests for the prepared-media half of src/bless/blesscore.c: the Windows
 * auto-prep decisions of docs/DESIGN.md Sections 6.3 and 6.3.1.
 *
 * What this group is for. Everything the loader decides about prepared media
 * is decided from two untrusted inputs - a manifest on writable exFAT and an
 * EFI variable anything on the machine can write - and acted on before a menu
 * exists to report a problem on. The firmware half (opening the sidecars,
 * publishing the overlaid medium, setting and deleting the variable) cannot be
 * stood in for by a host process, so it is not here. The decisions can be, and
 * these are the ones worth pinning:
 *
 *   freshness     the same rule blessed records use, which DESIGN 8.1 makes
 *                 explicit for the loader: path AND size, never the hash. A
 *                 prepared record whose ISO changed is stale, labelled as
 *                 such, and boots as if it were not prepared - which for
 *                 Windows media means the offer to prepare it again.
 *
 *   the label     the five words the menu can append are the contract with
 *                 the user, so the exact text is asserted.
 *
 *   the byte map  a record's splice list becomes an override set or it becomes
 *                 a refusal. A record asking for more ranges than a medium may
 *                 carry, and one asking for none at all, are both refusals:
 *                 serving an ISO unaltered under a label promising otherwise
 *                 is the one failure the user cannot diagnose.
 *
 *   the marker    the next-boot value is NVRAM, writable by anything with boot
 *                 services on that machine and by something that has never
 *                 seen this stick. It is decoded, never believed: the hostile
 *                 shapes below all have to end at "no marker", and the value
 *                 is only ever compared against paths the scan found.
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

/* One prepared record in the exact shape DESIGN Section 8.1 documents, one
 * blessed record beside it (the two kinds must not answer for each other),
 * and a second prepared one with two splices. */
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
    "family=arch\n"
    "\n"
    "iso=Windows/Win10.iso\n"
    "kind=prepared\n"
    "size=4700000000\n"
    "sha256=0000000000000000000000000000000000000000"
    "000000000000000000000000\n"
    "splice=1024:2048:boot.wim\n"
    "splice=8192:512:catalog.bin\n"
    "family=windows\n";

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

static bless_state look(const char *path, uint64_t size, bless_hit *out)
{
    uint16_t wide[512];

    widen(wide, sizeof(wide) / sizeof(wide[0]), path);
    return bless_cache_lookup(&g_cache, g_vol, wide, size, out);
}

/* The label, as the menu would compose it. */
static const char *suffix_of(const bless_hit *h, family_id fam)
{
    static char buf[32];
    const uint16_t *s = bless_suffix(h, fam);

    if (s == NULL)
        return NULL;
    ucs2_to_ascii(s, buf, sizeof(buf));
    return buf;
}

/* ------------------------------------------------ detection and labelling */

/* A prepared record whose ISO is still the size it was prepared from. Note
 * what is NOT consulted: the sha256, which names the sidecar folder and is
 * never recomputed over a multi-gigabyte ISO at menu time. */
static void test_prep_fresh(void **state)
{
    bless_hit hit;

    (void)state;
    assert_int_equal(look(WIN11_PATH, WIN11_SIZE, &hit), BLESS_FRESH);
    assert_int_equal(hit.rec->kind, MANIFEST_KIND_PREPARED);
    assert_int_equal(bless_prep_state(&hit, FAMILY_WINDOWS), BLESS_PREP_READY);
    assert_int_equal(bless_hit_action(&hit), BLESS_ACT_OVERLAY);
    assert_string_equal(suffix_of(&hit, FAMILY_WINDOWS), "  [prepared]");
}

/* One byte of difference is a different ISO as far as Section 8.1 is
 * concerned, and the artifacts on the stick were computed from the other
 * bytes. Stale is a label and a re-offer, never a boot with the old map. */
static void test_prep_stale(void **state)
{
    bless_hit hit;

    (void)state;
    assert_int_equal(look(WIN11_PATH, WIN11_SIZE - 1u, &hit), BLESS_STALE);
    assert_int_equal(bless_prep_state(&hit, FAMILY_WINDOWS), BLESS_PREP_STALE);
    assert_int_equal(bless_hit_action(&hit), BLESS_ACT_PLAIN);
    assert_string_equal(suffix_of(&hit, FAMILY_WINDOWS),
                        "  [prepared, stale]");
}

/* Windows media with no record at all: the one answer the manifest cannot
 * give, which is why the survey's family is an input here. */
static void test_prep_absent(void **state)
{
    bless_hit hit;

    (void)state;
    assert_int_equal(look("Windows/Win11_other.iso", WIN11_SIZE, &hit),
                     BLESS_NONE);
    assert_int_equal(bless_prep_state(&hit, FAMILY_WINDOWS),
                     BLESS_PREP_ABSENT);
    assert_int_equal(bless_hit_action(&hit), BLESS_ACT_PLAIN);
    assert_string_equal(suffix_of(&hit, FAMILY_WINDOWS), "  [not prepared]");
}

/* An unprobed entry has no family, so nothing may be said about whether it
 * wants preparing. "?" is the label it already carries and this adds nothing
 * to it: claiming "not prepared" about media nobody looked inside would be a
 * verdict, which is exactly what these labels never are. */
static void test_prep_unprobed_without_record(void **state)
{
    bless_hit hit;

    (void)state;
    (void)look("Windows/Win11_other.iso", WIN11_SIZE, &hit);
    assert_int_equal(bless_prep_state(&hit, FAMILY_UNKNOWN), BLESS_PREP_NA);
    assert_null(suffix_of(&hit, FAMILY_UNKNOWN));
}

/* The record says what it is, so an entry past the probe limit still labels
 * and boots as prepared: the survey is needed only for media with no record. */
static void test_prep_unprobed_with_record(void **state)
{
    bless_hit hit;

    (void)state;
    (void)look(WIN11_PATH, WIN11_SIZE, &hit);
    assert_int_equal(bless_prep_state(&hit, FAMILY_UNKNOWN), BLESS_PREP_READY);
    assert_int_equal(bless_hit_action(&hit), BLESS_ACT_OVERLAY);
    assert_string_equal(suffix_of(&hit, FAMILY_UNKNOWN), "  [prepared]");
}

/* The two kinds must not answer for each other. A blessed record launches a
 * signed wrap and has no byte map; it is not a preparation and must never be
 * reported as a missing one, even on Windows media. */
static void test_prep_blessed_is_not_prepared(void **state)
{
    bless_hit hit;

    (void)state;
    assert_int_equal(look("Linux/arch.iso", 1538670592u, &hit), BLESS_FRESH);
    assert_int_equal(bless_hit_action(&hit), BLESS_ACT_UKI);
    assert_int_equal(bless_prep_state(&hit, FAMILY_ARCH), BLESS_PREP_NA);
    assert_int_equal(bless_prep_state(&hit, FAMILY_WINDOWS), BLESS_PREP_NA);
    assert_string_equal(suffix_of(&hit, FAMILY_WINDOWS), "  [blessed]");
}

/* Nothing on the stick, nothing inside the medium: the case every non-Windows
 * ISO on every stick takes, which must stay exactly as it was. */
static void test_prep_ordinary_entry(void **state)
{
    bless_hit hit;

    (void)state;
    assert_int_equal(look("Linux/fedora.iso", 100u, &hit), BLESS_NONE);
    assert_int_equal(bless_prep_state(&hit, FAMILY_ARCH), BLESS_PREP_NA);
    assert_int_equal(bless_hit_action(&hit), BLESS_ACT_PLAIN);
    assert_null(suffix_of(&hit, FAMILY_ARCH));
}

static void test_prep_null_hit(void **state)
{
    (void)state;
    assert_int_equal(bless_hit_action(NULL), BLESS_ACT_PLAIN);
    assert_int_equal(bless_prep_state(NULL, FAMILY_WINDOWS), BLESS_PREP_NA);
    assert_null(bless_suffix(NULL, FAMILY_WINDOWS));
}

/* ------------------------------------------------------------ the byte map */

static void test_overlay_one_splice(void **state)
{
    bless_hit hit;
    iso_overlay ovl;

    (void)state;
    (void)look(WIN11_PATH, WIN11_SIZE, &hit);
    assert_int_equal(bless_prepared_overlay(hit.rec, &ovl), BLESS_OK);
    assert_int_equal(ovl.n, 1);
    assert_int_equal(ovl.extra, 0);
    assert_int_equal(ovl.ovl[0].iso_off, 22095872u);
    assert_int_equal(ovl.ovl[0].len, 593121462u);
    /* The sidecar IS the replacement, whole: a splice names a file, not a
     * range inside one. */
    assert_int_equal(ovl.ovl[0].src_off, 0);
    /* Opening files is the caller's half of the line. */
    assert_null(ovl.ovl[0].src);
}

static void test_overlay_two_splices(void **state)
{
    bless_hit hit;
    iso_overlay ovl;

    (void)state;
    (void)look("Windows/Win10.iso", 4700000000u, &hit);
    assert_int_equal(bless_prepared_overlay(hit.rec, &ovl), BLESS_OK);
    assert_int_equal(ovl.n, 2);
    assert_int_equal(ovl.ovl[0].iso_off, 1024u);
    assert_int_equal(ovl.ovl[0].len, 2048u);
    assert_int_equal(ovl.ovl[1].iso_off, 8192u);
    assert_int_equal(ovl.ovl[1].len, 512u);
}

/* A set built from a record has to pass the gate the boot path runs it
 * through, or the two halves disagree about what a valid map is. */
static void test_overlay_passes_the_gate(void **state)
{
    bless_hit hit;
    iso_overlay ovl;
    uint64_t sizes[ISO_MAX_OVERRIDES];
    fs_file dummy[ISO_MAX_OVERRIDES];
    size_t i;

    (void)state;
    (void)look("Windows/Win10.iso", 4700000000u, &hit);
    assert_int_equal(bless_prepared_overlay(hit.rec, &ovl), BLESS_OK);
    for (i = 0; i < ovl.n; i++) {
        ovl.ovl[i].src = &dummy[i];
        sizes[i] = ovl.ovl[i].len;
    }
    assert_int_equal(iso_overlay_check(&ovl, 4700000000u, sizes),
                     ISO_OVL_OK);
}

static void test_overlay_wrong_kind(void **state)
{
    bless_hit hit;
    iso_overlay ovl;

    (void)state;
    (void)look("Linux/arch.iso", 1538670592u, &hit);
    assert_int_equal(bless_prepared_overlay(hit.rec, &ovl), BLESS_ERR_KIND);
    assert_int_equal(ovl.n, 0);
}

/* A prepared record with no splice= parses (Section 8.1 requires only iso=,
 * size= and sha256=), and it describes a preparation that did nothing.
 * Serving the ISO unaltered under a "[prepared]" label is the one outcome
 * with no symptom a user could act on, so it is a refusal. */
static void test_overlay_no_splices(void **state)
{
    static const char text[] =
        "iso=Windows/Empty.iso\n"
        "kind=prepared\n"
        "size=100\n"
        "sha256=1111111111111111111111111111111111111111"
        "111111111111111111111111\n";
    bless_hit hit;
    iso_overlay ovl;

    (void)state;
    bless_cache_init(&g_cache);
    assert_int_equal(bless_cache_add(&g_cache, text, strlen(text), g_vol), 1);
    assert_int_equal(look("Windows/Empty.iso", 100u, &hit), BLESS_FRESH);
    assert_int_equal(bless_prepared_overlay(hit.rec, &ovl),
                     BLESS_ERR_NOSPLICE);
    assert_int_equal(ovl.n, 0);
}

/* The format allows eight splices per record and a medium carries four, so
 * the gap is reachable from a file a user can write. Refused whole rather
 * than truncated to the first four: half a byte map is worse than none. */
static void test_overlay_too_many_splices(void **state)
{
    static const char text[] =
        "iso=Windows/Many.iso\n"
        "kind=prepared\n"
        "size=1000000\n"
        "sha256=2222222222222222222222222222222222222222"
        "222222222222222222222222\n"
        "splice=0:16:a\n"
        "splice=16:16:b\n"
        "splice=32:16:c\n"
        "splice=48:16:d\n"
        "splice=64:16:e\n";
    bless_hit hit;
    iso_overlay ovl;

    (void)state;
    bless_cache_init(&g_cache);
    assert_int_equal(bless_cache_add(&g_cache, text, strlen(text), g_vol), 1);
    assert_int_equal(look("Windows/Many.iso", 1000000u, &hit), BLESS_FRESH);
    assert_int_equal(hit.rec->splice_n, 5);
    assert_int_equal(bless_prepared_overlay(hit.rec, &ovl),
                     BLESS_ERR_SPLICES);
    assert_int_equal(ovl.n, 0);
}

static void test_overlay_null_args(void **state)
{
    bless_hit hit;
    iso_overlay ovl;

    (void)state;
    (void)look(WIN11_PATH, WIN11_SIZE, &hit);
    assert_int_equal(bless_prepared_overlay(NULL, &ovl), BLESS_ERR_BAD_ARG);
    assert_int_equal(bless_prepared_overlay(hit.rec, NULL),
                     BLESS_ERR_BAD_ARG);
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

/* ------------------------------------------------- the whole Section 6.3.1 rule
 *
 * Exactly as src/efi/main.c composes it: find the scanned entry the marker
 * names, then ask what booting it means. Nothing indexes anything with the
 * value, so every hostile shape can only fail to match.
 */
static int may_boot_menuless(const char *marker, const char *path,
                             uint64_t cur_size)
{
    char decoded[BLESS_NEXT_MAX];
    uint16_t wide[512];
    bless_hit hit;

    if (bless_next_decode(marker, strlen(marker) + 1u, decoded,
                          sizeof decoded) != BLESS_OK)
        return 0;
    widen(wide, sizeof(wide) / sizeof(wide[0]), path);
    if (!bless_next_names(wide, decoded))
        return 0;
    (void)bless_cache_lookup(&g_cache, g_vol, wide, cur_size, &hit);
    return bless_hit_action(&hit) == BLESS_ACT_OVERLAY;
}

static void test_next_rule(void **state)
{
    (void)state;
    /* The one case that boots without a menu. */
    assert_true(may_boot_menuless(WIN11_PATH, WIN11_PATH, WIN11_SIZE));

    /* Preparation did not happen, or happened to something else. */
    assert_false(may_boot_menuless("Windows/Nothing.iso",
                                   "Windows/Nothing.iso", WIN11_SIZE));
    /* The ISO changed between the marker being set and this boot. */
    assert_false(may_boot_menuless(WIN11_PATH, WIN11_PATH, WIN11_SIZE + 1u));
    /* The marker names a blessed entry: a real record, the wrong kind, and
     * one whose launch path this marker has no business triggering. */
    assert_false(may_boot_menuless("Linux/arch.iso", "Linux/arch.iso",
                                   1538670592u));
    /* The marker names something that is not the entry in hand. */
    assert_false(may_boot_menuless("Windows/Win10.iso", WIN11_PATH,
                                   WIN11_SIZE));
    /* Hostile shapes, none of which reach a lookup at all. */
    assert_false(may_boot_menuless("", WIN11_PATH, WIN11_SIZE));
    assert_false(may_boot_menuless("../../etc/shadow", WIN11_PATH,
                                   WIN11_SIZE));
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

int run_prep_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_prep_fresh, setup_cache),
        cmocka_unit_test_setup(test_prep_stale, setup_cache),
        cmocka_unit_test_setup(test_prep_absent, setup_cache),
        cmocka_unit_test_setup(test_prep_unprobed_without_record, setup_cache),
        cmocka_unit_test_setup(test_prep_unprobed_with_record, setup_cache),
        cmocka_unit_test_setup(test_prep_blessed_is_not_prepared, setup_cache),
        cmocka_unit_test_setup(test_prep_ordinary_entry, setup_cache),
        cmocka_unit_test_setup(test_prep_null_hit, setup_cache),
        cmocka_unit_test_setup(test_overlay_one_splice, setup_cache),
        cmocka_unit_test_setup(test_overlay_two_splices, setup_cache),
        cmocka_unit_test_setup(test_overlay_passes_the_gate, setup_cache),
        cmocka_unit_test_setup(test_overlay_wrong_kind, setup_cache),
        cmocka_unit_test_setup(test_overlay_no_splices, setup_cache),
        cmocka_unit_test_setup(test_overlay_too_many_splices, setup_cache),
        cmocka_unit_test_setup(test_overlay_null_args, setup_cache),
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

    return cmocka_run_group_tests_name("prep", tests, NULL, NULL);
}
