/* Tests for falcon-cli/src/mkstick/updatecore.c: what `mkstick update`
 * decides a disk is, and so what it writes (docs/DESIGN.md Section 12.14).
 *
 * The refusals matter as much as the plans: an update that took a disk for
 * something it is not would write stages over somebody's boot code.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <mkstick/updatecore.h>

static upd_facts none(void)
{
    upd_facts f;

    memset(&f, 0, sizeof f);
    f.boot = -1;
    f.esp_boot = -1;
    f.library = -1;
    f.shelf = -1;
    return f;
}

static upd_facts stick(void)
{
    upd_facts f = none();

    f.boot = 0;
    f.library = 1;
    f.shelf = 2;
    f.stick_loader = 1;
    return f;
}

static void test_stick(void **state)
{
    upd_facts f = stick();
    upd_plan p;

    (void)state;
    upd_plan_make(&f, &p);
    assert_int_equal(p.kind, UPD_STICK);
    assert_true(p.uefi_stick);
    assert_false(p.bios);
    assert_false(p.uefi_station);

    /* The BIOS stages only where `mkstick bios` put them. */
    f.bios_installed = 1;
    upd_plan_make(&f, &p);
    assert_true(p.bios);
    assert_false(p.legacy);

    /* A BIOS-only stick is still a stick. */
    f.stick_loader = 0;
    upd_plan_make(&f, &p);
    assert_int_equal(p.kind, UPD_STICK);
    assert_true(p.bios);
    assert_false(p.uefi_stick);

    /* One with neither has nothing to update. */
    f.bios_installed = 0;
    upd_plan_make(&f, &p);
    assert_int_equal(p.kind, UPD_REFUSE);
    assert_non_null(p.why);
}

static void test_bios_station(void **state)
{
    upd_facts f = none();
    upd_plan p;

    (void)state;
    f.boot = 3;
    f.station_ini = 1;
    f.bios_installed = 1;
    upd_plan_make(&f, &p);
    assert_int_equal(p.kind, UPD_STATION);
    assert_true(p.bios);
    assert_false(p.legacy);
    assert_false(p.uefi_station);

    f.mods = 1;
    upd_plan_make(&f, &p);
    assert_true(p.legacy);

    /* STATION.INI decides a station over a library and shelf beside it: a
     * station's shelf carries its install ID too. */
    f.library = 1;
    f.shelf = 2;
    upd_plan_make(&f, &p);
    assert_int_equal(p.kind, UPD_STATION);
}

static void test_uefi_station(void **state)
{
    upd_facts f = none();
    upd_plan p;

    (void)state;
    f.esp_boot = 0;
    f.station_loader = 1;
    upd_plan_make(&f, &p);
    assert_int_equal(p.kind, UPD_STATION);
    assert_true(p.uefi_station);
    assert_false(p.totp);
    assert_false(p.bios);

    f.totp = 1;
    upd_plan_make(&f, &p);
    assert_true(p.totp);

    /* An ESP record with no loader beside it is nothing to re-sign. */
    f.station_loader = 0;
    upd_plan_make(&f, &p);
    assert_int_equal(p.kind, UPD_REFUSE);
    assert_non_null(p.why);
}

static void test_both_stations(void **state)
{
    upd_facts f = none();
    upd_plan p;

    (void)state;
    f.boot = 4;
    f.station_ini = 1;
    f.bios_installed = 1;
    f.esp_boot = 0;
    f.station_loader = 1;
    f.totp = 1;
    upd_plan_make(&f, &p);
    assert_int_equal(p.kind, UPD_STATION);
    assert_true(p.bios);
    assert_true(p.uefi_station);
    assert_true(p.totp);

    /* The BIOS half is skipped when LBA 0 holds somebody else's code. */
    f.bios_installed = 0;
    upd_plan_make(&f, &p);
    assert_false(p.bios);
    assert_true(p.uefi_station);
}

static void test_refusals(void **state)
{
    upd_facts f = none();
    upd_plan p;

    (void)state;
    upd_plan_make(&f, &p);
    assert_int_equal(p.kind, UPD_REFUSE);
    assert_non_null(p.why);

    /* A boot record alone: not a stick (no library, no shelf) and not a
     * station (no STATION.INI). */
    f.boot = 0;
    f.bios_installed = 1;
    upd_plan_make(&f, &p);
    assert_int_equal(p.kind, UPD_REFUSE);

    /* Half a stick is not one either. */
    f.library = 1;
    upd_plan_make(&f, &p);
    assert_int_equal(p.kind, UPD_REFUSE);

    /* A BIOS station whose LBA 0 is not falcon's, and no ESP half. */
    f = none();
    f.boot = 2;
    f.station_ini = 1;
    upd_plan_make(&f, &p);
    assert_int_equal(p.kind, UPD_REFUSE);
    assert_non_null(p.why);
}

int run_updatecore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_stick),
        cmocka_unit_test(test_bios_station),
        cmocka_unit_test(test_uefi_station),
        cmocka_unit_test(test_both_stations),
        cmocka_unit_test(test_refusals),
    };

    return cmocka_run_group_tests_name("mkstick update", tests, NULL, NULL);
}
