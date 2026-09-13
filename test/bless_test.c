/* Tests for src/bless/blesscore.c, the EFI-free half of the blessed-boot path
 * (docs/DESIGN.md Section 8.1).
 *
 * What this group is for. The module turns a file on writable, unverified
 * exFAT into the answer "this menu entry boots a signed UKI instead of the
 * ISO's own loader", and it does that while the menu is being drawn, where
 * there is no way to report a problem and no user to ask. So the properties
 * worth pinning are not "does it accept a good manifest" but the ones that
 * decide what happens when it is handed something else:
 *
 *   totality      no input produces an error, an out-of-bounds read or a
 *                 refusal: a corrupt manifest, an absent one, one padded past
 *                 the cap and one that parses to nothing all have to arrive at
 *                 "nothing is blessed" or "fewer things are blessed".
 *
 *   the rule      Section 8.1 is path AND size. A size mismatch is stale,
 *                 which the menu labels differently and the boot dispatch
 *                 treats as unblessed. Getting the path half wrong in either
 *                 direction is the expensive bug: too strict and a blessed
 *                 ISO silently loses its blessing, too loose and one ISO's
 *                 entry launches the signed cmdline built for another.
 *
 *   the tag       records carry which volume they came from. Two sticks in
 *                 one machine that have both blessed "Linux/arch.iso" must
 *                 not answer for each other, because the UKI that would be
 *                 launched lives on the other stick.
 *
 * What is deliberately NOT here: the hash. Section 8.1 has the loader check
 * size only, so a record whose sha256 no longer matches the file is still
 * fresh as far as this code is concerned, and a test asserting otherwise
 * would be asserting a design this document rejected.
 *
 * The firmware half (reading the manifest off the volume, publishing the
 * image, the launch) is proven in the QEMU rig by tools/mk-test-blessed-stick.sh
 * and not here; it is firmware all the way down and there is nothing about it
 * a host process could stand in for.
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

#include <bless/blesscore.h>

/* Volume tags. Their values never matter, only their identity: the module
 * compares them as pointers and never dereferences one. */
static const char g_vol_a[] = "p2 on the first stick";
static const char g_vol_b[] = "p2 on the second stick";
static const char g_vol_c[] = "a volume with no manifest";

/* Two blessed ISOs in the exact shape DESIGN Section 8.1 documents. */
static const char g_manifest[] =
    "# falcon-boot manifest\n"
    "\n"
    "iso=Linux/archlinux-2026.01.01-x86_64.iso\n"
    "size=1538670592\n"
    "sha256=1111111111111111111111111111111111111111111111111111111111111111\n"
    "uki=archlinux-2026.01.01-x86_64.uki\n"
    "family=arch\n"
    "\n"
    "iso=kubuntu-26.04-desktop-amd64.iso\n"
    "size=5068847104\n"
    "uki=kubuntu-26.04.uki\n"
    "family=casper\n";

/* The scan hands the lookup a UCS-2 path, so every case has to build one.
 * Explicit widening rather than a u"" literal, matching the rest of the
 * suite, and because the interesting cases are the ones no literal can
 * express anyway (see the non-ASCII test). */
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

/* One lookup by ASCII path, which is how every case below reads. */
static bless_state look(const bless_cache *c, const void *src,
                        const char *path, uint64_t size, bless_hit *out)
{
    uint16_t wide[512];

    widen(wide, sizeof(wide) / sizeof(wide[0]), path);
    return bless_cache_lookup(c, src, wide, size, out);
}

/* The cache is ~16 KB, past what a test frame should carry. */
static bless_cache g_cache;

static int setup_cache(void **state)
{
    (void)state;
    bless_cache_init(&g_cache);
    return 0;
}

/* ------------------------------------------------------- the happy path */

static void test_bless_add_two(void **state)
{
    (void)state;
    assert_int_equal(bless_cache_add(&g_cache, g_manifest,
                                     strlen(g_manifest), g_vol_a), 2);
    assert_int_equal(g_cache.n, 2);
    assert_int_equal(g_cache.dropped, 0);
}

static void test_bless_fresh(void **state)
{
    bless_hit hit;

    (void)state;
    (void)bless_cache_add(&g_cache, g_manifest, strlen(g_manifest), g_vol_a);

    assert_int_equal(look(&g_cache, g_vol_a,
                          "Linux/archlinux-2026.01.01-x86_64.iso",
                          1538670592u, &hit), BLESS_FRESH);
    assert_non_null(hit.rec);
    assert_string_equal(hit.rec->uki, "archlinux-2026.01.01-x86_64.uki");
    assert_string_equal(hit.rec->family, "arch");
    assert_ptr_equal(hit.src, g_vol_a);
    assert_int_equal(hit.state, BLESS_FRESH);
}

/* The second record has no sha256=, which the loader never reads. It must
 * still be a usable blessing, because the bless tool is allowed to omit a
 * field only the tool itself consumes. */
static void test_bless_fresh_without_sha(void **state)
{
    bless_hit hit;

    (void)state;
    (void)bless_cache_add(&g_cache, g_manifest, strlen(g_manifest), g_vol_a);

    assert_int_equal(look(&g_cache, g_vol_a, "kubuntu-26.04-desktop-amd64.iso",
                          5068847104u, &hit), BLESS_FRESH);
    assert_string_equal(hit.rec->uki, "kubuntu-26.04.uki");
    assert_int_equal(hit.rec->has_sha256, 0);
}

/* ------------------------------------------------------ the size half */

/* The ISO was replaced after it was blessed. Section 8.1: labelled stale,
 * booted as unblessed, and the record still comes back so the caller can say
 * which blessing went stale. */
static void test_bless_stale(void **state)
{
    bless_hit hit;

    (void)state;
    (void)bless_cache_add(&g_cache, g_manifest, strlen(g_manifest), g_vol_a);

    assert_int_equal(look(&g_cache, g_vol_a,
                          "Linux/archlinux-2026.01.01-x86_64.iso",
                          1538670593u, &hit), BLESS_STALE);
    assert_non_null(hit.rec);
    assert_string_equal(hit.rec->uki, "archlinux-2026.01.01-x86_64.uki");
    assert_int_equal(hit.state, BLESS_STALE);

    /* One byte short counts too: the check is equality, not a range. */
    assert_int_equal(look(&g_cache, g_vol_a,
                          "Linux/archlinux-2026.01.01-x86_64.iso",
                          1538670591u, &hit), BLESS_STALE);
    /* And a zero-length file, which is what a half-finished copy looks like
     * to the directory listing. */
    assert_int_equal(look(&g_cache, g_vol_a,
                          "Linux/archlinux-2026.01.01-x86_64.iso", 0, &hit),
                     BLESS_STALE);
}

/* ------------------------------------------------------ the path half */

static void test_bless_path_unknown(void **state)
{
    bless_hit hit;

    (void)state;
    (void)bless_cache_add(&g_cache, g_manifest, strlen(g_manifest), g_vol_a);

    /* An ISO nobody blessed. */
    assert_int_equal(look(&g_cache, g_vol_a, "systemrescue-12.00.iso",
                          1538670592u, &hit), BLESS_NONE);
    /* The right name in the wrong directory is a different file. */
    assert_int_equal(look(&g_cache, g_vol_a,
                          "Other/archlinux-2026.01.01-x86_64.iso",
                          1538670592u, &hit), BLESS_NONE);
    /* A prefix of a blessed path is not that path. */
    assert_int_equal(look(&g_cache, g_vol_a, "Linux/archlinux-2026.01.01.iso",
                          1538670592u, &hit), BLESS_NONE);
}

/* The scan builds its paths with '/', the manifest may be hand-edited with
 * either, and exFAT itself is case-insensitive. All three have to agree or a
 * blessed ISO loses its label for a reason no user could guess. */
static void test_bless_path_equivalences(void **state)
{
    bless_hit hit;

    (void)state;
    (void)bless_cache_add(&g_cache, g_manifest, strlen(g_manifest), g_vol_a);

    assert_int_equal(look(&g_cache, g_vol_a,
                          "Linux\\archlinux-2026.01.01-x86_64.iso",
                          1538670592u, &hit), BLESS_FRESH);
    assert_int_equal(look(&g_cache, g_vol_a,
                          "LINUX/ARCHLINUX-2026.01.01-X86_64.ISO",
                          1538670592u, &hit), BLESS_FRESH);
    assert_int_equal(look(&g_cache, g_vol_a,
                          "/Linux//archlinux-2026.01.01-x86_64.iso",
                          1538670592u, &hit), BLESS_FRESH);
}

/* A path that is not printable ASCII cannot match any record, because
 * manifest_parse drops records containing such a byte. The answer is NONE,
 * not an error: the entry is shown and boots the ordinary way. */
static void test_bless_path_non_ascii(void **state)
{
    /* "caf<e-acute>.iso", as the exFAT driver would hand it over. */
    static const uint16_t accented[] = { 'c', 'a', 'f', 0x00E9, '.', 'i',
                                         's', 'o', 0 };
    bless_hit hit;

    (void)state;
    (void)bless_cache_add(&g_cache, g_manifest, strlen(g_manifest), g_vol_a);

    assert_int_equal(bless_cache_lookup(&g_cache, g_vol_a, accented, 1u, &hit),
                     BLESS_NONE);
    assert_null(hit.rec);
}

/* ------------------------------------------------------------ the tag */

/* Two sticks, both blessing a path with the same spelling. Each answers for
 * its own volume and neither answers for the other: the UKI a hit names lives
 * on the volume the record came from, so a crossed tag would launch an image
 * off the wrong stick, or off no stick at all. */
static void test_bless_volume_tag(void **state)
{
    static const char other[] =
        "iso=Linux/archlinux-2026.01.01-x86_64.iso\n"
        "size=999\n"
        "uki=second-stick.uki\n";
    bless_hit hit;

    (void)state;
    (void)bless_cache_add(&g_cache, g_manifest, strlen(g_manifest), g_vol_a);
    assert_int_equal(bless_cache_add(&g_cache, other, strlen(other), g_vol_b),
                     1);
    assert_int_equal(g_cache.n, 3);

    assert_int_equal(look(&g_cache, g_vol_a,
                          "Linux/archlinux-2026.01.01-x86_64.iso",
                          1538670592u, &hit), BLESS_FRESH);
    assert_string_equal(hit.rec->uki, "archlinux-2026.01.01-x86_64.uki");

    assert_int_equal(look(&g_cache, g_vol_b,
                          "Linux/archlinux-2026.01.01-x86_64.iso", 999u,
                          &hit), BLESS_FRESH);
    assert_string_equal(hit.rec->uki, "second-stick.uki");
    assert_ptr_equal(hit.src, g_vol_b);

    /* A volume that contributed no records answers for nothing. */
    assert_int_equal(look(&g_cache, g_vol_c,
                          "Linux/archlinux-2026.01.01-x86_64.iso",
                          1538670592u, &hit), BLESS_NONE);
}

/* ------------------------------------------------- absent and corrupt */

/* "There was no manifest" is the call the loader makes for every volume that
 * is not one of our sticks, and it must be indistinguishable from an empty
 * one (DESIGN Section 8.1: degrade to "nothing is blessed", never to an
 * error). */
static void test_bless_absent(void **state)
{
    bless_hit hit;

    (void)state;
    assert_int_equal(bless_cache_add(&g_cache, NULL, 0, g_vol_a), 0);
    assert_int_equal(bless_cache_add(&g_cache, "", 0, g_vol_a), 0);
    assert_int_equal(g_cache.n, 0);
    assert_int_equal(g_cache.dropped, 0);
    assert_int_equal(look(&g_cache, g_vol_a, "anything.iso", 1u, &hit),
                     BLESS_NONE);
    assert_null(hit.rec);
    assert_null(hit.src);
    assert_int_equal(hit.state, BLESS_NONE);
}

/* Anything that is not a manifest: a renamed binary, a half-written file, a
 * block of comments, and records missing the fields that make them usable.
 * Every one of them is zero records and no complaint. */
static void test_bless_corrupt(void **state)
{
    static const char binary[] = "\x7f" "ELF\x02\x01\x01\x00 not text at all";
    static const char comments[] = "# blessed\n# iso=a.iso\n# size=1\n";
    static const char no_uki[] = "iso=a.iso\nsize=10\nfamily=arch\n";
    static const char no_size[] = "iso=a.iso\nuki=a.uki\n";
    static const char bad_size[] = "iso=a.iso\nsize=twelve\nuki=a.uki\n";
    static const char truncated[] = "iso=a.iso\nsize=10\nuki";
    bless_hit hit;

    (void)state;
    assert_int_equal(bless_cache_add(&g_cache, binary, sizeof(binary) - 1u,
                                     g_vol_a), 0);
    assert_int_equal(bless_cache_add(&g_cache, comments, strlen(comments),
                                     g_vol_a), 0);
    assert_int_equal(bless_cache_add(&g_cache, no_uki, strlen(no_uki),
                                     g_vol_a), 0);
    assert_int_equal(bless_cache_add(&g_cache, no_size, strlen(no_size),
                                     g_vol_a), 0);
    assert_int_equal(bless_cache_add(&g_cache, bad_size, strlen(bad_size),
                                     g_vol_a), 0);
    assert_int_equal(bless_cache_add(&g_cache, truncated, strlen(truncated),
                                     g_vol_a), 0);

    assert_int_equal(g_cache.n, 0);
    assert_int_equal(g_cache.dropped, 0);
    assert_int_equal(look(&g_cache, g_vol_a, "a.iso", 10u, &hit), BLESS_NONE);
}

/* A manifest that is mostly rubbish still blesses what it got right. The
 * alternative (one bad record disables the stick) is the failure mode a
 * hand-edited file makes most likely. */
static void test_bless_partial(void **state)
{
    static const char mixed[] =
        "iso=broken.iso\n"           /* no uki=, no size= */
        "iso=good.iso\nsize=42\nuki=good.uki\n"
        "nonsense line with no equals sign\n"
        "iso=also-broken.iso\nsize=notanumber\nuki=x.uki\n";
    bless_hit hit;

    (void)state;
    assert_int_equal(bless_cache_add(&g_cache, mixed, strlen(mixed),
                                     g_vol_a), 1);
    assert_int_equal(look(&g_cache, g_vol_a, "good.iso", 42u, &hit),
                     BLESS_FRESH);
    assert_int_equal(look(&g_cache, g_vol_a, "broken.iso", 1u, &hit),
                     BLESS_NONE);
}

/* ---------------------------------------------------------- the cap */

/* Builds a manifest of n records named iso-<i>.iso with size <i>. */
static char *many(size_t n)
{
    size_t cap = n * 96u + 64u;
    char *buf = malloc(cap);
    size_t used = 0;
    size_t i;

    assert_non_null(buf);
    for (i = 0; i < n; i++) {
        int k = snprintf(buf + used, cap - used,
                         "iso=iso-%zu.iso\nsize=%zu\nuki=uki-%zu.uki\n",
                         i, i, i);
        assert_true(k > 0 && (size_t)k < cap - used);
        used += (size_t)k;
    }
    return buf;
}

/* One record past the cap. The first BLESS_MAX_RECORDS are kept and usable,
 * the rest are counted rather than silently forgotten, and nothing is written
 * past the end of the array (which is what the guard rail matters for: the
 * manifest's length is an attacker's choice). */
static void test_bless_cap_overflow(void **state)
{
    char *text = many(BLESS_MAX_RECORDS + 1u);
    char name[32];
    bless_hit hit;

    (void)state;
    assert_int_equal(bless_cache_add(&g_cache, text, strlen(text), g_vol_a),
                     BLESS_MAX_RECORDS);
    assert_int_equal(g_cache.n, BLESS_MAX_RECORDS);
    assert_int_equal(g_cache.dropped, 1);

    /* The last one that fitted is a working blessing. */
    (void)snprintf(name, sizeof(name), "iso-%u.iso", BLESS_MAX_RECORDS - 1u);
    assert_int_equal(look(&g_cache, g_vol_a, name,
                          (uint64_t)BLESS_MAX_RECORDS - 1u, &hit),
                     BLESS_FRESH);

    /* The one past the cap is simply not blessed, which is a label a user
     * loses rather than a boot that breaks. */
    (void)snprintf(name, sizeof(name), "iso-%u.iso", BLESS_MAX_RECORDS);
    assert_int_equal(look(&g_cache, g_vol_a, name,
                          (uint64_t)BLESS_MAX_RECORDS, &hit), BLESS_NONE);
    free(text);
}

/* A full cache and a second volume: nothing is added, the overflow is
 * counted, and the records already there are untouched. */
static void test_bless_cap_second_volume(void **state)
{
    char *text = many(BLESS_MAX_RECORDS);
    static const char more[] = "iso=late.iso\nsize=1\nuki=late.uki\n";
    bless_hit hit;

    (void)state;
    assert_int_equal(bless_cache_add(&g_cache, text, strlen(text), g_vol_a),
                     BLESS_MAX_RECORDS);
    assert_int_equal(g_cache.dropped, 0);

    assert_int_equal(bless_cache_add(&g_cache, more, strlen(more), g_vol_b),
                     0);
    assert_int_equal(g_cache.n, BLESS_MAX_RECORDS);
    assert_int_equal(g_cache.dropped, 1);

    assert_int_equal(look(&g_cache, g_vol_b, "late.iso", 1u, &hit),
                     BLESS_NONE);
    assert_int_equal(look(&g_cache, g_vol_a, "iso-0.iso", 0, &hit),
                     BLESS_FRESH);
    free(text);
}

/* ------------------------------------------------------- housekeeping */

static void test_bless_reset(void **state)
{
    bless_hit hit;

    (void)state;
    (void)bless_cache_add(&g_cache, g_manifest, strlen(g_manifest), g_vol_a);
    bless_cache_init(&g_cache);
    assert_int_equal(g_cache.n, 0);
    assert_int_equal(g_cache.dropped, 0);
    assert_int_equal(look(&g_cache, g_vol_a,
                          "Linux/archlinux-2026.01.01-x86_64.iso",
                          1538670592u, &hit), BLESS_NONE);
}

/* Nothing here may dereference a null. The loader calls all of it while
 * drawing a menu, where a fault is a brick rather than a stack trace. */
static void test_bless_null_args(void **state)
{
    uint16_t wide[8];
    bless_hit hit;

    (void)state;
    assert_int_equal(bless_cache_add(NULL, g_manifest, strlen(g_manifest),
                                     g_vol_a), 0);
    bless_cache_init(NULL);

    widen(wide, sizeof(wide) / sizeof(wide[0]), "a.iso");
    assert_int_equal(bless_cache_lookup(NULL, g_vol_a, wide, 1u, &hit),
                     BLESS_NONE);
    assert_null(hit.rec);
    assert_int_equal(bless_cache_lookup(&g_cache, g_vol_a, NULL, 1u, &hit),
                     BLESS_NONE);
    /* A caller that does not want the record passes no buffer. */
    assert_int_equal(bless_cache_lookup(&g_cache, g_vol_a, wide, 1u, NULL),
                     BLESS_NONE);
}

/* The menu appends these verbatim, so the exact text is the contract. The
 * prepared vocabulary is exercised in prep_test.c, beside the rest of the
 * Section 6.3.1 logic it belongs to. */
static void test_bless_suffix(void **state)
{
    bless_hit hit;
    char buf[32];

    (void)state;
    (void)bless_cache_add(&g_cache, g_manifest, strlen(g_manifest), g_vol_a);

    (void)look(&g_cache, g_vol_a, "Linux/archlinux-2026.01.01-x86_64.iso",
               1538670592u, &hit);
    ucs2_to_ascii(bless_suffix(&hit, FAMILY_ARCH), buf, sizeof(buf));
    assert_string_equal(buf, "  [blessed]");

    (void)look(&g_cache, g_vol_a, "Linux/archlinux-2026.01.01-x86_64.iso",
               1u, &hit);
    ucs2_to_ascii(bless_suffix(&hit, FAMILY_ARCH), buf, sizeof(buf));
    assert_string_equal(buf, "  [blessed, stale]");

    /* Nothing to append for an ordinary entry, so the caller appends
     * nothing rather than an empty pair of brackets. */
    (void)look(&g_cache, g_vol_a, "nothing-blesses-this.iso", 1u, &hit);
    assert_null(bless_suffix(&hit, FAMILY_ARCH));
}

int run_bless_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_bless_add_two, setup_cache),
        cmocka_unit_test_setup(test_bless_fresh, setup_cache),
        cmocka_unit_test_setup(test_bless_fresh_without_sha, setup_cache),
        cmocka_unit_test_setup(test_bless_stale, setup_cache),
        cmocka_unit_test_setup(test_bless_path_unknown, setup_cache),
        cmocka_unit_test_setup(test_bless_path_equivalences, setup_cache),
        cmocka_unit_test_setup(test_bless_path_non_ascii, setup_cache),
        cmocka_unit_test_setup(test_bless_volume_tag, setup_cache),
        cmocka_unit_test_setup(test_bless_absent, setup_cache),
        cmocka_unit_test_setup(test_bless_corrupt, setup_cache),
        cmocka_unit_test_setup(test_bless_partial, setup_cache),
        cmocka_unit_test_setup(test_bless_cap_overflow, setup_cache),
        cmocka_unit_test_setup(test_bless_cap_second_volume, setup_cache),
        cmocka_unit_test_setup(test_bless_reset, setup_cache),
        cmocka_unit_test_setup(test_bless_null_args, setup_cache),
        cmocka_unit_test_setup(test_bless_suffix, setup_cache),
    };

    return cmocka_run_group_tests_name("bless", tests, NULL, NULL);
}
