/* Tests for falcon-bios/src/console/cdclock.c: the menu countdown's clock,
 * which must end a countdown when either the tick count or the RTC moves
 * (DESIGN Section 12.7). */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <console/cdclock.h>

/* Ticks moving, the RTC not answering: 18.2 ticks a second. */
static void test_ticks_only(void **state)
{
    cdclock c;

    (void)state;
    cdclock_start(&c, 1000u, CDCLOCK_NO_RTC);
    assert_int_equal(cdclock_tenths(&c, 1000u, CDCLOCK_NO_RTC), 0);
    assert_int_equal(cdclock_tenths(&c, 1018u, CDCLOCK_NO_RTC), 9);
    assert_int_equal(cdclock_tenths(&c, 1091u, CDCLOCK_NO_RTC), 50);
}

/* The ticks stopped (IRQ0 never delivered), the RTC moving: each change of
 * its seconds is a second. */
static void test_rtc_only(void **state)
{
    cdclock c;

    (void)state;
    cdclock_start(&c, 500u, 0x58);
    assert_int_equal(cdclock_tenths(&c, 500u, 0x58), 0);
    assert_int_equal(cdclock_tenths(&c, 500u, 0x59), 10);
    assert_int_equal(cdclock_tenths(&c, 500u, 0x59), 10);
    /* BCD 59 to 00: a change like any other. */
    assert_int_equal(cdclock_tenths(&c, 500u, 0x00), 20);
    assert_int_equal(cdclock_tenths(&c, 500u, 0x01), 30);
    /* A failed read in between counts nothing and loses nothing. */
    assert_int_equal(cdclock_tenths(&c, 500u, CDCLOCK_NO_RTC), 30);
    assert_int_equal(cdclock_tenths(&c, 500u, 0x02), 40);
}

/* Both moving: the one that says more. */
static void test_both(void **state)
{
    cdclock c;

    (void)state;
    cdclock_start(&c, 0u, 0x10);
    assert_int_equal(cdclock_tenths(&c, 9u, 0x11), 10);    /* RTC ahead */
    assert_int_equal(cdclock_tenths(&c, 40u, 0x11), 21);   /* ticks ahead */
}

/* An RTC that answered nothing at the start counts from its first reading. */
static void test_rtc_late(void **state)
{
    cdclock c;

    (void)state;
    cdclock_start(&c, 0u, CDCLOCK_NO_RTC);
    assert_int_equal(cdclock_tenths(&c, 0u, 0x30), 0);
    assert_int_equal(cdclock_tenths(&c, 0u, 0x31), 10);
}

/* Midnight: the count goes back to 0. */
static void test_midnight(void **state)
{
    cdclock c;

    (void)state;
    cdclock_start(&c, CDCLOCK_TICKS_PER_DAY - 9u, CDCLOCK_NO_RTC);
    assert_int_equal(cdclock_tenths(&c, 9u, CDCLOCK_NO_RTC), 9);
    /* A start the BIOS never wrote reads as nothing gone, not a day. */
    cdclock_start(&c, 0xFFFFFFF0u, CDCLOCK_NO_RTC);
    assert_int_equal(cdclock_tenths(&c, 5u, CDCLOCK_NO_RTC), 0);
}

int run_cdclock_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_ticks_only),
        cmocka_unit_test(test_rtc_only),
        cmocka_unit_test(test_both),
        cmocka_unit_test(test_rtc_late),
        cmocka_unit_test(test_midnight),
    };

    return cmocka_run_group_tests_name("cdclock", tests, NULL, NULL);
}
