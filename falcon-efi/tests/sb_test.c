/* Tests for falcon-efi/src/secboot/sb.c, the Secure Boot state reader
 * (docs/DESIGN.md Sections 7.3, 7.4).
 *
 * The module has no I/O of its own: everything it knows arrives through one
 * runtime-services function pointer, so a fake GetVariable driven by a small
 * table is enough to reach every state it can report. That is the whole
 * surface, which is why the suite is a truth table rather than a scenario
 * walk.
 *
 * Two of the assertions here are not about the mapping but about the module's
 * reason for existing, and they exist so the next change to it trips over
 * them:
 *
 *   - The state never answers YES on a variable it could not read cleanly.
 *     "Unknown" folded into "yes" would put a permissive-mode warning on a
 *     machine that is enforcing normally; folded into "no" it would hide one
 *     on a machine that is not. Both directions are tested.
 *   - sb_state_read asks for MokSBStateRT and never for MokSBState. On shim
 *     16.1, MokSBState is the
 *     user's request and shim deletes it outright when the running OS could
 *     have written it, while MokSBStateRT is the mirror shim creates only
 *     after it has decided to stop validating. Reading the request would tell
 *     the user validation was off in exactly the case where shim had just
 *     refused to turn it off. The fake fails the test if the wrong name or
 *     the wrong vendor GUID is ever asked for.
 *
 * Nothing here tests an enforcement decision; the module makes none (DESIGN
 * Section 7.3).
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <secboot/sb.h>

/* 8be4df61-93ca-11d2-aa0d-00e098032b8c, and 605dab50-e046-4300-abb6-3dd810dd8b23.
 * Transcribed independently of sb.c's copies on purpose: a GUID that agrees
 * only with itself would let a typo in the module pass unnoticed. */
static const EFI_GUID guid_global = {
    0x8be4df61, 0x93ca, 0x11d2,
    { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c }
};
static const EFI_GUID guid_shim = {
    0x605dab50, 0xe046, 0x4300,
    { 0xab, 0xb6, 0x3d, 0xd8, 0x10, 0xdd, 0x8b, 0x23 }
};

/* ---- the fake platform ---- */

/* One row per variable the platform is pretending to hold. A row is matched
 * by name only; the GUID it carries is what the fake asserts the caller asked
 * with, so a lookup under the wrong namespace is a failure rather than a
 * miss that quietly reads as "absent". */
typedef struct {
    const uint16_t *name;
    const EFI_GUID *guid;
    EFI_STATUS      status;  /* what GetVariable returns for this row */
    UINTN           size;    /* bytes the row reports back */
    UINT8           data;
} fake_var;

#define FAKE_MAX 4

static fake_var fake_vars[FAKE_MAX];
static UINTN    fake_count;
/* Every name the module asked for, so a test can assert on what was NOT
 * asked as well as on what came back. */
static const uint16_t *fake_asked[FAKE_MAX + 4];
static UINTN           fake_asked_count;

static int ucs2_same(const uint16_t *a, const uint16_t *b)
{
    UINTN i = 0;

    while (a[i] != 0 && a[i] == b[i])
        i++;
    return a[i] == b[i];
}

static int guid_same(const EFI_GUID *a, const EFI_GUID *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static EFI_STATUS EFIAPI fake_get_variable(CHAR16 *name, EFI_GUID *guid,
                                           UINT32 *attributes, UINTN *size,
                                           void *data)
{
    UINTN i;

    /* sb.c passes no attribute pointer, and should not start: the attributes
     * of a variable it only reports are none of its business. */
    assert_null(attributes);
    assert_non_null(name);
    assert_non_null(guid);
    assert_non_null(size);
    assert_non_null(data);

    if (fake_asked_count < sizeof(fake_asked) / sizeof(fake_asked[0]))
        fake_asked[fake_asked_count++] = (const uint16_t *)name;

    for (i = 0; i < fake_count; i++) {
        if (!ucs2_same(fake_vars[i].name, (const uint16_t *)name))
            continue;
        /* Right name, wrong namespace: fail loudly instead of returning
         * NOT_FOUND, which would look like a normal absent variable. */
        assert_true(guid_same(fake_vars[i].guid, guid));
        if (EFI_ERROR(fake_vars[i].status)) {
            *size = fake_vars[i].size;
            return fake_vars[i].status;
        }
        *size = fake_vars[i].size;
        if (fake_vars[i].size >= 1)
            *(UINT8 *)data = fake_vars[i].data;
        return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
}

static EFI_RUNTIME_SERVICES fake_rt;

static void fake_reset(void)
{
    memset(fake_vars, 0, sizeof(fake_vars));
    memset(fake_asked, 0, sizeof(fake_asked));
    memset(&fake_rt, 0, sizeof(fake_rt));
    fake_count = 0;
    fake_asked_count = 0;
    fake_rt.GetVariable = fake_get_variable;
}

static void fake_add(const uint16_t *name, const EFI_GUID *guid,
                     EFI_STATUS status, UINTN size, UINT8 data)
{
    assert_true(fake_count < FAKE_MAX);
    fake_vars[fake_count].name = name;
    fake_vars[fake_count].guid = guid;
    fake_vars[fake_count].status = status;
    fake_vars[fake_count].size = size;
    fake_vars[fake_count].data = data;
    fake_count++;
}

/* The three names the module may ask for, plus the one it must not. */
static const uint16_t name_secure_boot[] = u"SecureBoot";
static const uint16_t name_setup_mode[]  = u"SetupMode";
static const uint16_t name_mok_rt[]      = u"MokSBStateRT";
static const uint16_t name_mok_req[]     = u"MokSBState";

static int asked_for(const uint16_t *name)
{
    UINTN i;

    for (i = 0; i < fake_asked_count; i++) {
        if (ucs2_same(fake_asked[i], name))
            return 1;
    }
    return 0;
}

/* Reads the state through the fake and reduces it to a mode in one step, so
 * each truth-table case below stays one line of setup and one assertion. */
static sb_mode mode_of(void)
{
    sb_state state;

    assert_int_equal(sb_state_read(&fake_rt, &state), SB_OK);
    return sb_state_mode(&state);
}

static void assert_label(sb_mode mode, const char *expect)
{
    char got[64];

    ucs2_to_ascii((const uint16_t *)sb_mode_label(mode), got, sizeof(got));
    assert_string_equal(got, expect);
}

/* ---- reading the variables ---- */

static void test_no_runtime_services_is_unknown(void **state)
{
    sb_state s;

    (void)state;
    fake_reset();
    assert_int_equal(sb_state_read((EFI_RUNTIME_SERVICES *)NULL, &s), SB_OK);
    assert_int_equal(s.secure_boot, SB_TRI_UNKNOWN);
    assert_int_equal(s.setup_mode, SB_TRI_UNKNOWN);
    assert_int_equal(s.mok_sb_validation_disabled, SB_TRI_UNKNOWN);
    assert_int_equal(sb_state_mode(&s), SB_MODE_UNKNOWN);
}

static void test_no_get_variable_is_unknown(void **state)
{
    sb_state s;

    (void)state;
    fake_reset();
    fake_rt.GetVariable = NULL;
    assert_int_equal(sb_state_read(&fake_rt, &s), SB_OK);
    assert_int_equal(s.secure_boot, SB_TRI_UNKNOWN);
    assert_int_equal(sb_state_mode(&s), SB_MODE_UNKNOWN);
}

static void test_null_out_is_rejected(void **state)
{
    (void)state;
    fake_reset();
    assert_int_equal(sb_state_read(&fake_rt, (sb_state *)NULL),
                     SB_ERR_BAD_ARG);
    assert_int_equal(fake_asked_count, 0);
}

/* The measured contract: the mirror shim publishes, never the request the
 * user filed. See the header comment. */
static void test_reads_the_mirror_not_the_request(void **state)
{
    sb_state s;

    (void)state;
    fake_reset();
    fake_add(name_mok_req, &guid_shim, EFI_SUCCESS, 1, 1);
    assert_int_equal(sb_state_read(&fake_rt, &s), SB_OK);

    assert_true(asked_for(name_secure_boot));
    assert_true(asked_for(name_setup_mode));
    assert_true(asked_for(name_mok_rt));
    assert_false(asked_for(name_mok_req));
    /* A MokSBState that shim did not mirror says nothing about enforcement. */
    assert_int_equal(s.mok_sb_validation_disabled, SB_TRI_NO);
}

/* ---- the truth table ---- */

static void test_on_and_validating(void **state)
{
    (void)state;
    fake_reset();
    fake_add(name_secure_boot, &guid_global, EFI_SUCCESS, 1, 1);
    assert_int_equal(mode_of(), SB_MODE_ON);
}

static void test_on_and_validation_disabled(void **state)
{
    (void)state;
    fake_reset();
    fake_add(name_secure_boot, &guid_global, EFI_SUCCESS, 1, 1);
    fake_add(name_mok_rt, &guid_shim, EFI_SUCCESS, 1, 1);
    assert_int_equal(mode_of(), SB_MODE_ON_VALIDATION_DISABLED);
}

/* shim writes 1; anything nonzero means the same thing, and a reader that
 * only accepted 1 would silently fall back to "enforcing". */
static void test_mirror_nonzero_counts_as_disabled(void **state)
{
    (void)state;
    fake_reset();
    fake_add(name_secure_boot, &guid_global, EFI_SUCCESS, 1, 1);
    fake_add(name_mok_rt, &guid_shim, EFI_SUCCESS, 1, 0xff);
    assert_int_equal(mode_of(), SB_MODE_ON_VALIDATION_DISABLED);
}

static void test_mirror_present_but_zero_is_validating(void **state)
{
    (void)state;
    fake_reset();
    fake_add(name_secure_boot, &guid_global, EFI_SUCCESS, 1, 1);
    fake_add(name_mok_rt, &guid_shim, EFI_SUCCESS, 1, 0);
    assert_int_equal(mode_of(), SB_MODE_ON);
}

/* A variable of the wrong size is a variable we did not understand. It must
 * not become a warning (that would cry wolf on an enforcing machine) and it
 * must not become silence either, which is why the state keeps UNKNOWN even
 * though the mode it produces is the same as the enforcing one. */
static void test_mirror_wrong_size_is_not_a_warning(void **state)
{
    sb_state s;

    (void)state;
    fake_reset();
    fake_add(name_secure_boot, &guid_global, EFI_SUCCESS, 1, 1);
    fake_add(name_mok_rt, &guid_shim, EFI_SUCCESS, 4, 1);
    assert_int_equal(sb_state_read(&fake_rt, &s), SB_OK);
    assert_int_equal(s.mok_sb_validation_disabled, SB_TRI_UNKNOWN);
    assert_int_equal(sb_state_mode(&s), SB_MODE_ON);
    assert_false(sb_mode_needs_warning(sb_state_mode(&s)));
}

static void test_mirror_read_error_is_not_a_warning(void **state)
{
    sb_state s;

    (void)state;
    fake_reset();
    fake_add(name_secure_boot, &guid_global, EFI_SUCCESS, 1, 1);
    fake_add(name_mok_rt, &guid_shim, EFI_DEVICE_ERROR, 1, 1);
    assert_int_equal(sb_state_read(&fake_rt, &s), SB_OK);
    assert_int_equal(s.mok_sb_validation_disabled, SB_TRI_UNKNOWN);
    assert_int_equal(sb_state_mode(&s), SB_MODE_ON);
}

/* The whole point of the mode being derived rather than reported raw: with
 * Secure Boot off there is nothing for shim to relax, so the machine is not
 * in permissive mode, it is simply off, and the banner must say so. */
static void test_mirror_without_secure_boot_is_plain_off(void **state)
{
    (void)state;
    fake_reset();
    fake_add(name_secure_boot, &guid_global, EFI_SUCCESS, 1, 0);
    fake_add(name_mok_rt, &guid_shim, EFI_SUCCESS, 1, 1);
    assert_int_equal(mode_of(), SB_MODE_OFF);
    assert_false(sb_mode_needs_warning(SB_MODE_OFF));
}

static void test_off(void **state)
{
    (void)state;
    fake_reset();
    fake_add(name_secure_boot, &guid_global, EFI_SUCCESS, 1, 0);
    assert_int_equal(mode_of(), SB_MODE_OFF);
}

/* Absence is an answer: a platform with no SecureBoot variable at all is not
 * enforcing, and that is a defined "off" rather than an unknown. */
static void test_absent_variables_are_off(void **state)
{
    (void)state;
    fake_reset();
    assert_int_equal(mode_of(), SB_MODE_OFF);
}

static void test_setup_mode(void **state)
{
    (void)state;
    fake_reset();
    fake_add(name_secure_boot, &guid_global, EFI_SUCCESS, 1, 0);
    fake_add(name_setup_mode, &guid_global, EFI_SUCCESS, 1, 1);
    assert_int_equal(mode_of(), SB_MODE_SETUP);
}

/* Setup mode with SecureBoot asserted cannot happen on a sane platform. If it
 * does, what is being enforced is the useful half, so ON wins. */
static void test_secure_boot_outranks_setup_mode(void **state)
{
    (void)state;
    fake_reset();
    fake_add(name_secure_boot, &guid_global, EFI_SUCCESS, 1, 1);
    fake_add(name_setup_mode, &guid_global, EFI_SUCCESS, 1, 1);
    assert_int_equal(mode_of(), SB_MODE_ON);
}

static void test_unreadable_secure_boot_is_unknown(void **state)
{
    (void)state;
    fake_reset();
    fake_add(name_secure_boot, &guid_global, EFI_DEVICE_ERROR, 1, 1);
    assert_int_equal(mode_of(), SB_MODE_UNKNOWN);
}

static void test_null_state_is_unknown(void **state)
{
    (void)state;
    assert_int_equal(sb_state_mode((const sb_state *)NULL), SB_MODE_UNKNOWN);
}

/* ---- what the user is shown ---- */

static void test_labels(void **state)
{
    (void)state;
    assert_label(SB_MODE_OFF, "SB: off");
    assert_label(SB_MODE_ON, "SB: on");
    assert_label(SB_MODE_ON_VALIDATION_DISABLED,
                 "SB: on (shim validation DISABLED)");
    assert_label(SB_MODE_SETUP, "SB: setup mode");
    assert_label(SB_MODE_UNKNOWN, "SB: unknown");
    /* Never NULL, whatever it is handed: the banner prints this without a
     * guard, and a mode added later must degrade to a label, not a fault. */
    assert_label((sb_mode)99, "SB: unknown");
}

/* The banner has the title beside it in the spec's guaranteed 80x25 mode, so
 * a label that grew past this would push the two apart or be clipped. */
static void test_labels_fit_the_banner(void **state)
{
    const sb_mode all[] = { SB_MODE_UNKNOWN, SB_MODE_OFF, SB_MODE_ON,
                            SB_MODE_ON_VALIDATION_DISABLED, SB_MODE_SETUP };
    size_t i;

    (void)state;
    for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        char got[64];

        ucs2_to_ascii((const uint16_t *)sb_mode_label(all[i]), got,
                      sizeof(got));
        assert_true(strlen(got) <= 33);
    }
}

/* Exactly one state is worth shouting about. Widening this would dilute it;
 * narrowing it would hide the one state DESIGN Section 7.4 calls out. */
static void test_only_validation_disabled_warns(void **state)
{
    (void)state;
    assert_true(sb_mode_needs_warning(SB_MODE_ON_VALIDATION_DISABLED));
    assert_false(sb_mode_needs_warning(SB_MODE_ON));
    assert_false(sb_mode_needs_warning(SB_MODE_OFF));
    assert_false(sb_mode_needs_warning(SB_MODE_SETUP));
    assert_false(sb_mode_needs_warning(SB_MODE_UNKNOWN));
}

int run_sb_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_no_runtime_services_is_unknown),
        cmocka_unit_test(test_no_get_variable_is_unknown),
        cmocka_unit_test(test_null_out_is_rejected),
        cmocka_unit_test(test_reads_the_mirror_not_the_request),
        cmocka_unit_test(test_on_and_validating),
        cmocka_unit_test(test_on_and_validation_disabled),
        cmocka_unit_test(test_mirror_nonzero_counts_as_disabled),
        cmocka_unit_test(test_mirror_present_but_zero_is_validating),
        cmocka_unit_test(test_mirror_wrong_size_is_not_a_warning),
        cmocka_unit_test(test_mirror_read_error_is_not_a_warning),
        cmocka_unit_test(test_mirror_without_secure_boot_is_plain_off),
        cmocka_unit_test(test_off),
        cmocka_unit_test(test_absent_variables_are_off),
        cmocka_unit_test(test_setup_mode),
        cmocka_unit_test(test_secure_boot_outranks_setup_mode),
        cmocka_unit_test(test_unreadable_secure_boot_is_unknown),
        cmocka_unit_test(test_null_state_is_unknown),
        cmocka_unit_test(test_labels),
        cmocka_unit_test(test_labels_fit_the_banner),
        cmocka_unit_test(test_only_validation_disabled_warns),
    };

    return cmocka_run_group_tests_name("sb", tests, NULL, NULL);
}
