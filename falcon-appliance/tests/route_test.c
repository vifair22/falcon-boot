/* Tests for falcon-appliance/src/route.c: what the on-stick appliance owes
 * each image it finds on p2 (docs/DESIGN.md Sections 7.2 and 7.5).
 *
 * Why this is worth a group of its own. The routing decides whether an image
 * reaches the path that needs the MOK private key, and whether one the user
 * expects to be offered is offered at all: an image wrongly called "already
 * done" is one the user is told to expect and does not get.
 *
 * What is pinned:
 *
 *   freshness   the loader's own rule, path plus size and never the hash
 *               (Section 8.1). A stale record means sign it again.
 *
 *   Windows     owed nothing, whatever the manifest says: the loader patches
 *               it as it boots it (Section 6.4). Every other family is
 *               blessed.
 *
 *   payloads    what makes a bare .efi or .img "already done" is our
 *               certificate inside it rather than a record beside it (DESIGN
 *               Section 7.2). Wrong, that is silent: a payload wrongly called
 *               done is one the user never gets to sign.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <route.h>
#include <manifest/manifest.h>

/* Every family the registry knows but Windows, so a new row added to
 * family.c without a thought for the appliance shows up here as an unrouted
 * id rather than as a surprise queue at boot. BSD is here: what it is signed
 * for is its own loader rather than a wrap, but the appliance's answer to
 * "is this owed a signature" is the same yes. */
static const family_id g_linux_families[] = {
    FAMILY_UNKNOWN,
    FAMILY_DEBIAN_CASPER,
    FAMILY_DEBIAN_LIVE,
    FAMILY_FEDORA_DRACUT,
    FAMILY_ARCH,
    FAMILY_SYSTEMRESCUE,
    FAMILY_MEMTEST,
    FAMILY_GENTOO,
    FAMILY_BSD,
};

#define LINUX_N (sizeof(g_linux_families) / sizeof(g_linux_families[0]))

/* ------------------------------------------------------------ Windows */

static void test_route_windows_boots_as_is(void **state)
{
    static const manifest_bless all[] = { MANIFEST_BLESS_NONE,
                                          MANIFEST_BLESS_STALE,
                                          MANIFEST_BLESS_FRESH };
    size_t i;

    (void)state;
    /* A record naming Windows media, current or not, describes nothing the
     * appliance could have produced; none of the three states is a reason to
     * queue it or to count it as blessed. */
    for (i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        assert_int_equal(appl_route(all[i], FAMILY_WINDOWS),
                         APPL_JOB_BOOTS_AS_IS);
}

static void test_route_only_windows_boots_as_is(void **state)
{
    size_t i;
    size_t n = 0;
    const family_desc *tab = family_table(&n);

    (void)state;
    for (i = 0; i < LINUX_N; i++)
        assert_int_not_equal(appl_route(MANIFEST_BLESS_NONE,
                                        g_linux_families[i]),
                             APPL_JOB_BOOTS_AS_IS);

    /* The list above must stay the registry's, minus Windows: a family added
     * to family.c and not to it would go untested here and unnoticed. */
    assert_int_equal(n, LINUX_N + 1u);
    for (i = 0; i < n; i++) {
        if (tab[i].id == FAMILY_WINDOWS)
            continue;
        assert_int_not_equal(appl_route(MANIFEST_BLESS_NONE, tab[i].id),
                             APPL_JOB_BOOTS_AS_IS);
    }
}

/* --------------------------------------------------------- freshness */

static void test_route_unrecorded_is_signed(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < LINUX_N; i++)
        assert_int_equal(appl_route(MANIFEST_BLESS_NONE, g_linux_families[i]),
                         APPL_JOB_SIGN);
}

static void test_route_stale_is_signed_again(void **state)
{
    size_t i;

    (void)state;
    /* The file changed under a record that describes it, so the wrap that
     * record names was built from an ISO that no longer exists. */
    for (i = 0; i < LINUX_N; i++)
        assert_int_equal(appl_route(MANIFEST_BLESS_STALE,
                                    g_linux_families[i]), APPL_JOB_SIGN);
}

static void test_route_fresh_is_done(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < LINUX_N; i++)
        assert_int_equal(appl_route(MANIFEST_BLESS_FRESH,
                                    g_linux_families[i]),
                         APPL_JOB_DONE_BLESSED);
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

/* ------------------------------------------------------------------ run */

int run_appliance_route_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_route_windows_boots_as_is),
        cmocka_unit_test(test_route_only_windows_boots_as_is),
        cmocka_unit_test(test_route_unrecorded_is_signed),
        cmocka_unit_test(test_route_stale_is_signed_again),
        cmocka_unit_test(test_route_fresh_is_done),
        cmocka_unit_test(test_route_payload_shapes_are_the_loader_set),
        cmocka_unit_test(test_route_payload_done_is_our_certificate),
    };

    return cmocka_run_group_tests_name("appliance routing", tests, NULL, NULL);
}
