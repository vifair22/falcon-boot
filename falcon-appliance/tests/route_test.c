/* Tests for falcon-appliance/src/route.c: how the on-stick appliance splits the
 * ISO library on p2 into the two things it can do with an image
 * (docs/DESIGN.md Sections 6.3.1 and 7.5).
 *
 * Why this is worth a group of its own. The split decides whether an image
 * reaches a path that needs the MOK private key, and the appliance's whole
 * value is that the key spends as little time decrypted as possible: an ISO
 * misrouted into the bless queue is a passphrase prompt and an unlocked p3
 * that a prepare-only boot should never have had. It also decides whether the
 * batching of Section 6.3.1 works at all, since an image wrongly called
 * "already done" is one the user is told to expect and does not get.
 *
 * What is pinned:
 *
 *   freshness   the loader's own rule, path plus size and never the hash
 *               (Section 8.1). A stale record means do the work again.
 *
 *   the family  Windows is prepared, everything else is blessed, and that is
 *               the only thing the family decides here.
 *
 *   payloads    a bare .efi or .img is blessed and never prepared, and what
 *               makes one "already done" is our certificate inside it rather
 *               than a record beside it (DESIGN Section 7.2). Both halves of
 *               that are silent when wrong: a payload routed to the prepare
 *               queue would be handed to a tool that refuses it, and one
 *               wrongly called done is one the user never gets to sign.
 *
 *   the kinds   a current record counts only when its kind is the kind this
 *               medium's own path produces. The crossed cases (a prepared
 *               record on Linux media, a blessed one on Windows media) name
 *               an artifact that cannot boot what it claims to, so they are
 *               work rather than done. This is the appliance-side twin of
 *               prep_lookup's PREP_STATE_OTHER_KIND, and the two are checked
 *               against each other below rather than merely resembling each
 *               other in prose.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include <route.h>
#include <sign/prepcore.h>
#include <manifest/manifest.h>

/* Every family the registry knows, so a new row added to family.c without a
 * thought for the appliance shows up here as an unrouted id rather than as a
 * surprise queue at boot. */
static const family_id g_linux_families[] = {
    FAMILY_UNKNOWN,
    FAMILY_DEBIAN_CASPER,
    FAMILY_DEBIAN_LIVE,
    FAMILY_FEDORA_DRACUT,
    FAMILY_ARCH,
    FAMILY_SYSTEMRESCUE,
    FAMILY_MEMTEST,
    FAMILY_GENTOO,
};

#define LINUX_N (sizeof(g_linux_families) / sizeof(g_linux_families[0]))

/* ------------------------------------------------------------ the family */

static void test_route_only_windows_is_prepared(void **state)
{
    size_t i;
    size_t n = 0;
    const family_desc *tab = family_table(&n);

    (void)state;
    assert_true(appl_family_is_prepared(FAMILY_WINDOWS));
    for (i = 0; i < LINUX_N; i++)
        assert_false(appl_family_is_prepared(g_linux_families[i]));

    /* The list above must stay the registry's, minus Windows: a family added
     * to family.c and not to it would go untested here and unnoticed. */
    assert_int_equal(n, LINUX_N + 1u);
    for (i = 0; i < n; i++) {
        if (tab[i].id == FAMILY_WINDOWS)
            continue;
        assert_false(appl_family_is_prepared(tab[i].id));
    }
}

/* ----------------------------------------------------------- no record */

static void test_route_unrecorded_goes_by_family(void **state)
{
    size_t i;

    (void)state;
    /* kind is meaningless with no record, and must not change the answer:
     * a zeroed manifest_record reads as BLESSED, so a caller that forgot to
     * check state would otherwise get a different result from one that did. */
    assert_int_equal(appl_route(MANIFEST_BLESS_NONE, MANIFEST_KIND_BLESSED,
                                FAMILY_WINDOWS), APPL_JOB_PREPARE);
    assert_int_equal(appl_route(MANIFEST_BLESS_NONE, MANIFEST_KIND_PREPARED,
                                FAMILY_WINDOWS), APPL_JOB_PREPARE);
    for (i = 0; i < LINUX_N; i++) {
        assert_int_equal(appl_route(MANIFEST_BLESS_NONE,
                                    MANIFEST_KIND_BLESSED,
                                    g_linux_families[i]), APPL_JOB_SIGN);
        assert_int_equal(appl_route(MANIFEST_BLESS_NONE,
                                    MANIFEST_KIND_PREPARED,
                                    g_linux_families[i]), APPL_JOB_SIGN);
    }
}

/* -------------------------------------------------------- stale record */

static void test_route_stale_is_work_again(void **state)
{
    size_t i;

    (void)state;
    /* The file changed under a record that describes it, so whatever that
     * record points at describes an ISO that no longer exists. Both kinds,
     * because a stale prepared record is exactly the "replaced the ISO with
     * a different build" case Section 6.3.1 wants re-run automatically. */
    assert_int_equal(appl_route(MANIFEST_BLESS_STALE, MANIFEST_KIND_PREPARED,
                                FAMILY_WINDOWS), APPL_JOB_PREPARE);
    assert_int_equal(appl_route(MANIFEST_BLESS_STALE, MANIFEST_KIND_BLESSED,
                                FAMILY_WINDOWS), APPL_JOB_PREPARE);
    for (i = 0; i < LINUX_N; i++) {
        assert_int_equal(appl_route(MANIFEST_BLESS_STALE,
                                    MANIFEST_KIND_BLESSED,
                                    g_linux_families[i]), APPL_JOB_SIGN);
        assert_int_equal(appl_route(MANIFEST_BLESS_STALE,
                                    MANIFEST_KIND_PREPARED,
                                    g_linux_families[i]), APPL_JOB_SIGN);
    }
}

/* ------------------------------------------------------- current record */

static void test_route_fresh_matching_kind_is_done(void **state)
{
    size_t i;

    (void)state;
    assert_int_equal(appl_route(MANIFEST_BLESS_FRESH, MANIFEST_KIND_PREPARED,
                                FAMILY_WINDOWS), APPL_JOB_DONE_PREPARED);
    for (i = 0; i < LINUX_N; i++)
        assert_int_equal(appl_route(MANIFEST_BLESS_FRESH,
                                    MANIFEST_KIND_BLESSED,
                                    g_linux_families[i]),
                         APPL_JOB_DONE_BLESSED);
}

static void test_route_fresh_wrong_kind_is_work(void **state)
{
    size_t i;

    (void)state;
    /* A blessed record naming a UKI wrapped out of Windows media: there is no
     * Linux kernel in there to have wrapped, so the record cannot be honoured
     * and preparing replaces it. */
    assert_int_equal(appl_route(MANIFEST_BLESS_FRESH, MANIFEST_KIND_BLESSED,
                                FAMILY_WINDOWS), APPL_JOB_PREPARE);
    /* And the mirror: splices over a Linux ISO serve bytes nothing asked for. */
    for (i = 0; i < LINUX_N; i++)
        assert_int_equal(appl_route(MANIFEST_BLESS_FRESH,
                                    MANIFEST_KIND_PREPARED,
                                    g_linux_families[i]), APPL_JOB_SIGN);
}

/* --------------------------------------------------- bare payloads */

/* The set of names the appliance surveys has to be the set the loader lists
 * (falcon-efi/src/efi/main.c: *.iso, *.img, *.efi), and the split between the
 * wrap path and the payload path inside that set is this one function. An
 * appliance that disagreed with the menu here would leave a user looking at an
 * entry it cannot offer to bless. */
static void test_route_payload_shapes_are_the_loader_set(void **state)
{
    (void)state;
    assert_int_equal(sgn_payload_shape_of("Other/Shell.efi"), SGN_PAYLOAD_EFI);
    assert_int_equal(sgn_payload_shape_of("Other/memtest86-usb.img"),
                     SGN_PAYLOAD_IMG);
    assert_int_equal(sgn_payload_shape_of("Linux/arch.iso"), SGN_PAYLOAD_NONE);

    /* The extension, not the name: an ISO whose name happens to contain the
     * other extensions is still an ISO, and the case is the user's. */
    assert_int_equal(sgn_payload_shape_of("efi-img.iso"), SGN_PAYLOAD_NONE);
    assert_int_equal(sgn_payload_shape_of("BOOTX64.EFI"), SGN_PAYLOAD_EFI);
}

static void test_route_payload_is_never_prepared(void **state)
{
    static const appl_ours all[] = { APPL_OURS_UNKNOWN, APPL_OURS_NO,
                                     APPL_OURS_YES };
    size_t i;

    (void)state;
    /* Preparation rewrites boot.wim inside Windows install media and there is
     * no such thing inside a PE. Pinned over every answer to the certificate
     * question, because none of them is a reason to prepare anything. */
    for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        appl_job job = appl_route_payload(all[i]);

        assert_int_not_equal(job, APPL_JOB_PREPARE);
        assert_int_not_equal(job, APPL_JOB_DONE_PREPARED);
    }
}

static void test_route_payload_done_is_our_certificate(void **state)
{
    (void)state;
    /* The whole of "already done" for a payload: our exact certificate is in
     * it. This is sgn_payload_signed_with's question and not a second one, so
     * the queue and the tool cannot disagree about a file. */
    assert_int_equal(appl_route_payload(APPL_OURS_YES), APPL_JOB_DONE_BLESSED);
    assert_int_equal(appl_route_payload(APPL_OURS_NO), APPL_JOB_SIGN);

    /* Unknown is the survey's state, while p3 still holds the certificate to
     * ask with, and it must answer SIGN: offering a payload that turns out to
     * carry our certificate costs a listing line and a tool run that signs
     * nothing, and hiding one that does not costs a boot. */
    assert_int_equal(appl_route_payload(APPL_OURS_UNKNOWN), APPL_JOB_SIGN);
}

/* ------------------------------------------- agreement with the tool */

/* The appliance routes an image and `falcon-boot-sign prepare` then decides
 * for itself whether there is anything to do. If those two disagreed, a queued
 * image would be reported as prepared having done nothing, or a skipped one
 * would never be offered. Checked here over the same three inputs rather than
 * asserted in a comment. */
static void test_route_agrees_with_prep_lookup(void **state)
{
    manifest_record rec;
    prep_state ps;

    (void)state;
    memset(&rec, 0, sizeof(rec));
    (void)snprintf(rec.iso, sizeof(rec.iso), "%s", "Windows/win11.iso");
    rec.size = 7736125440ull;
    rec.kind = MANIFEST_KIND_PREPARED;
    rec.has_sha256 = 1;

    ps = prep_lookup(&rec, 1u, rec.iso, rec.size, NULL);
    assert_int_equal(ps, PREP_STATE_READY);
    assert_int_equal(appl_route(MANIFEST_BLESS_FRESH, rec.kind,
                                FAMILY_WINDOWS), APPL_JOB_DONE_PREPARED);

    /* Same path, a different length: both halves call it work. */
    ps = prep_lookup(&rec, 1u, rec.iso, rec.size - 1u, NULL);
    assert_int_equal(ps, PREP_STATE_STALE);
    assert_int_equal(appl_route(MANIFEST_BLESS_STALE, rec.kind,
                                FAMILY_WINDOWS), APPL_JOB_PREPARE);

    /* A blessed record on Windows media: OTHER_KIND there, PREPARE here. */
    rec.kind = MANIFEST_KIND_BLESSED;
    ps = prep_lookup(&rec, 1u, rec.iso, rec.size, NULL);
    assert_int_equal(ps, PREP_STATE_OTHER_KIND);
    assert_int_equal(appl_route(MANIFEST_BLESS_FRESH, rec.kind,
                                FAMILY_WINDOWS), APPL_JOB_PREPARE);

    /* An ISO nothing has a record for. */
    ps = prep_lookup(&rec, 1u, "Windows/other.iso", rec.size, NULL);
    assert_int_equal(ps, PREP_STATE_ABSENT);
    assert_int_equal(appl_route(MANIFEST_BLESS_NONE, MANIFEST_KIND_BLESSED,
                                FAMILY_WINDOWS), APPL_JOB_PREPARE);
}

/* ------------------------------------------------------------------ run */

int run_appliance_route_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_route_only_windows_is_prepared),
        cmocka_unit_test(test_route_unrecorded_goes_by_family),
        cmocka_unit_test(test_route_stale_is_work_again),
        cmocka_unit_test(test_route_fresh_matching_kind_is_done),
        cmocka_unit_test(test_route_fresh_wrong_kind_is_work),
        cmocka_unit_test(test_route_payload_shapes_are_the_loader_set),
        cmocka_unit_test(test_route_payload_is_never_prepared),
        cmocka_unit_test(test_route_payload_done_is_our_certificate),
        cmocka_unit_test(test_route_agrees_with_prep_lookup),
    };

    return cmocka_run_group_tests_name("appliance routing", tests, NULL, NULL);
}
