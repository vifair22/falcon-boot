/* Tests for src/cli/sign/prepcore.c: the four judgements `falcon-boot-sign
 * prepare` makes, separated from the 600 MB of subprocess work around them
 * precisely so they can be tested without any of it.
 *
 * What is pinned, and why each matters:
 *
 *   the record   the exact manifest text a prepared ISO produces, compared
 *                byte for byte against the format DESIGN Section 8.1 spells
 *                out. The loader parses this; a stray key or a missing one is
 *                a record it drops without a word, which is the failure mode
 *                Section 8.1 exists to have fixed.
 *
 *   the guard    the refusal when the patched WIM no longer fits the extent
 *                it is served over. Section 6.3 requires it by name, and it
 *                is the one condition that makes the size-preserving shape
 *                impossible; silently producing an oversized sidecar would
 *                corrupt the image Windows reads.
 *
 *   idempotence  the four answers to "has this ISO been prepared already",
 *                which is what keeps a second run from redoing 600 MB of work
 *                and what keeps a changed ISO from keeping stale artifacts.
 *
 *   the config   the WinPE-side key=value file, whose keys are read by
 *                src/winjump/winjump.c and are therefore a wire format
 *                between two programs that never see each other build.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <cli/sign/sign.h>
#include <cli/sign/prepcore.h>
#include <manifest/manifest.h>

/* The real numbers from the Win11 25H2 image (DESIGN Section 6.3), so the
 * rendered record below is the one this tool actually emits for it rather
 * than a plausible-looking invention. */
#define ISO_REL   "Windows/Windows 11 25H2.iso"
#define ISO_SIZE  7736125440ull
#define WIM_OFF   22095872ull
#define WIM_LEN   593121462ull
#define SHA_HEX   "0123456789abcdef0123456789abcdef" \
                  "0123456789abcdef0123456789abcdef"

/* --------------------------------------------------------- the record */

static void test_prepcore_record_fields(void **state)
{
    manifest_record rec;

    (void)state;
    assert_int_equal(prep_build_record(ISO_REL, ISO_SIZE, SHA_HEX, WIM_OFF,
                                       WIM_LEN, PREP_BOOTWIM, &rec), SGN_OK);
    assert_string_equal(rec.iso, ISO_REL);
    assert_int_equal(rec.kind, MANIFEST_KIND_PREPARED);
    assert_int_equal(rec.size, ISO_SIZE);
    assert_string_equal(rec.sha256, SHA_HEX);
    assert_int_equal(rec.has_sha256, 1);
    assert_string_equal(rec.family, "windows");
    /* No UKI: a prepared ISO has no signed wrap to boot, which is the whole
     * reason kind= had to exist (Section 8.1). */
    assert_string_equal(rec.uki, "");
    assert_int_equal(rec.splice_n, 1);
    assert_int_equal(rec.splice[0].iso_off, WIM_OFF);
    assert_int_equal(rec.splice[0].len, WIM_LEN);
    assert_string_equal(rec.splice[0].name, PREP_BOOTWIM);
}

/* The text itself, because the loader reads text and not a struct. */
static void test_prepcore_record_renders_to_the_documented_shape(void **state)
{
    static const char expect[] =
        "\niso=" ISO_REL
        "\nkind=prepared"
        "\nsize=7736125440"
        "\nsha256=" SHA_HEX
        "\nsplice=22095872:593121462:" PREP_BOOTWIM
        "\nfamily=windows\n";
    manifest_record rec;
    char out[2048];
    size_t used = 0;
    const char *body;

    (void)state;
    assert_int_equal(prep_build_record(ISO_REL, ISO_SIZE, SHA_HEX, WIM_OFF,
                                       WIM_LEN, PREP_BOOTWIM, &rec), SGN_OK);
    assert_int_equal(manifest_render(&rec, 1, out, sizeof(out) - 1u, &used),
                     MANIFEST_OK);
    out[used] = '\0';

    /* Past the file header comment the renderer always emits. */
    body = strstr(out, "\niso=");
    assert_non_null(body);
    assert_string_equal(body, expect);
}

/* Round trip: what prepare writes is what the loader's parser reads back, and
 * it comes back as a prepared record with its splice intact. */
static void test_prepcore_record_round_trips(void **state)
{
    manifest_record rec;
    manifest_record back[4];
    char out[2048];
    size_t used = 0;
    size_t n = 0;

    (void)state;
    assert_int_equal(prep_build_record(ISO_REL, ISO_SIZE, SHA_HEX, WIM_OFF,
                                       WIM_LEN, PREP_BOOTWIM, &rec), SGN_OK);
    assert_int_equal(manifest_render(&rec, 1, out, sizeof(out), &used),
                     MANIFEST_OK);
    assert_int_equal(manifest_parse(out, used, back, 4, &n), MANIFEST_OK);
    assert_int_equal(n, 1);
    assert_int_equal(back[0].kind, MANIFEST_KIND_PREPARED);
    assert_string_equal(back[0].iso, ISO_REL);
    assert_int_equal(back[0].splice_n, 1);
    assert_int_equal(back[0].splice[0].iso_off, WIM_OFF);
    assert_int_equal(back[0].splice[0].len, WIM_LEN);
    assert_string_equal(back[0].splice[0].name, PREP_BOOTWIM);
    assert_string_equal(back[0].sha256, SHA_HEX);
    assert_string_equal(back[0].family, "windows");
}

static void test_prepcore_record_rejects_unrepresentable(void **state)
{
    char longpath[MANIFEST_PATH_MAX + 8];
    manifest_record rec;

    (void)state;
    memset(longpath, 'a', sizeof(longpath) - 1u);
    longpath[sizeof(longpath) - 1u] = '\0';

    assert_int_equal(prep_build_record(longpath, ISO_SIZE, SHA_HEX, WIM_OFF,
                                       WIM_LEN, PREP_BOOTWIM, &rec),
                     SGN_ERR_NOSPACE);
    assert_int_equal(prep_build_record("", ISO_SIZE, SHA_HEX, WIM_OFF,
                                       WIM_LEN, PREP_BOOTWIM, &rec),
                     SGN_ERR_NOSPACE);
    /* A '#' would turn the rest of the line into a comment, so a manifest
     * cannot name that file at all; the tool says so rather than writing a
     * record that reads back as half a path. */
    assert_int_equal(prep_build_record("Windows/a#b.iso", ISO_SIZE, SHA_HEX,
                                       WIM_OFF, WIM_LEN, PREP_BOOTWIM, &rec),
                     SGN_ERR_FORMAT);
    /* A splice of zero length is not a byte range. */
    assert_int_equal(prep_build_record(ISO_REL, ISO_SIZE, SHA_HEX, WIM_OFF,
                                       0u, PREP_BOOTWIM, &rec),
                     SGN_ERR_FORMAT);
    /* A digest that is not 64 hex characters cannot name a sidecar folder. */
    assert_int_equal(prep_build_record(ISO_REL, ISO_SIZE, "deadbeef", WIM_OFF,
                                       WIM_LEN, PREP_BOOTWIM, &rec),
                     SGN_ERR_INTERNAL);
    assert_int_equal(prep_build_record(ISO_REL, ISO_SIZE, SHA_HEX, WIM_OFF,
                                       WIM_LEN, "", &rec),
                     SGN_ERR_NOSPACE);
    assert_int_equal(prep_build_record(NULL, ISO_SIZE, SHA_HEX, WIM_OFF,
                                       WIM_LEN, PREP_BOOTWIM, NULL),
                     SGN_ERR_INTERNAL);
}

/* ---------------------------------------------------------- the guard */

static void test_prepcore_fits(void **state)
{
    (void)state;

    /* Smaller is the normal case: wimlib repacks Microsoft's own boot.wim
     * below its shipped size, which is what the whole shape rests on. */
    assert_int_equal(prep_fits(WIM_LEN - (1u << 20), WIM_LEN), SGN_OK);
    /* Exactly the same length needs no padding and is still fine. */
    assert_int_equal(prep_fits(WIM_LEN, WIM_LEN), SGN_OK);
    assert_int_equal(prep_fits(WIM_LEN + 1u, WIM_LEN), SGN_ERR_NOSPACE);

    /* The message has to be actionable: both sizes and the way out. */
    assert_non_null(strstr(sgn_error(), "593121463"));
    assert_non_null(strstr(sgn_error(), "593121462"));
    assert_non_null(strstr(sgn_error(), "--recompress"));
}

/* ----------------------------------------------------- the idempotence */

static manifest_record make_prepared(const char *iso, uint64_t size)
{
    manifest_record rec;

    assert_int_equal(prep_build_record(iso, size, SHA_HEX, WIM_OFF, WIM_LEN,
                                       PREP_BOOTWIM, &rec), SGN_OK);
    return rec;
}

static void test_prepcore_lookup(void **state)
{
    manifest_record recs[3];
    const manifest_record *hit = NULL;

    (void)state;
    memset(recs, 0, sizeof(recs));
    recs[0] = make_prepared(ISO_REL, ISO_SIZE);

    /* Prepared, and the file is still exactly what the record describes. */
    assert_int_equal(prep_lookup(recs, 1, ISO_REL, ISO_SIZE, &hit),
                     PREP_STATE_READY);
    assert_ptr_equal(hit, &recs[0]);

    /* A different build of the same name: the size moved, so the artifacts
     * belong to a file that is no longer there and the work is redone. */
    assert_int_equal(prep_lookup(recs, 1, ISO_REL, ISO_SIZE + 2048u, &hit),
                     PREP_STATE_STALE);
    assert_ptr_equal(hit, &recs[0]);

    /* Nothing about this path at all. */
    assert_int_equal(prep_lookup(recs, 1, "Linux/arch.iso", 1u, &hit),
                     PREP_STATE_ABSENT);
    assert_null(hit);

    /* A blessed record for the same path is not this operation's artifact,
     * and the caller warns about the UKI it leaves behind. */
    memset(&recs[1], 0, sizeof(recs[1]));
    memcpy(recs[1].iso, "Linux/arch.iso", sizeof("Linux/arch.iso"));
    memcpy(recs[1].uki, "arch.uki", sizeof("arch.uki"));
    recs[1].size = 1u;
    recs[1].kind = MANIFEST_KIND_BLESSED;
    assert_int_equal(prep_lookup(recs, 2, "Linux/arch.iso", 1u, &hit),
                     PREP_STATE_OTHER_KIND);
    assert_ptr_equal(hit, &recs[1]);

    /* Path matching is the manifest's own: case-insensitive, separators
     * interchangeable, so an ISO reached through a differently-spelled path
     * is still the one that was prepared. */
    assert_int_equal(prep_lookup(recs, 2, "windows\\WINDOWS 11 25H2.ISO",
                                 ISO_SIZE, &hit), PREP_STATE_READY);

    assert_int_equal(prep_lookup(NULL, 0, ISO_REL, ISO_SIZE, &hit),
                     PREP_STATE_ABSENT);
    assert_int_equal(prep_lookup(recs, 1, NULL, ISO_SIZE, NULL),
                     PREP_STATE_ABSENT);
}

/* --------------------------------------------------------- the config */

static void test_prepcore_cfg(void **state)
{
    char buf[512];
    size_t used = 0;

    (void)state;
    assert_int_equal(prep_cfg_render(ISO_REL, ISO_SIZE, buf, sizeof(buf),
                                     &used), SGN_OK);
    assert_int_equal(strlen(buf), used);

    /* The two keys src/winjump/winjump.c reads, spelled the way it spells
     * them, with the path relative to the p2 root and no leading separator. */
    assert_non_null(strstr(buf, "\niso=" ISO_REL "\n"));
    assert_non_null(strstr(buf, "\nsize=7736125440\n"));
    /* Comments are '#' to end of line in that parser too. */
    assert_int_equal(buf[0], '#');

    assert_int_equal(prep_cfg_render(ISO_REL, ISO_SIZE, buf, 16u, &used),
                     SGN_ERR_NOSPACE);
    assert_int_equal(used, 0);
    assert_int_equal(prep_cfg_render(NULL, ISO_SIZE, buf, sizeof(buf), &used),
                     SGN_ERR_INTERNAL);
}

/* ------------------------------------------------------------------ run */

int run_prepcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_prepcore_record_fields),
        cmocka_unit_test(test_prepcore_record_renders_to_the_documented_shape),
        cmocka_unit_test(test_prepcore_record_round_trips),
        cmocka_unit_test(test_prepcore_record_rejects_unrepresentable),
        cmocka_unit_test(test_prepcore_fits),
        cmocka_unit_test(test_prepcore_lookup),
        cmocka_unit_test(test_prepcore_cfg),
    };

    return cmocka_run_group_tests_name("Windows preparation (bless tool)",
                                       tests, NULL, NULL);
}
