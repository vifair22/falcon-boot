/* Tests for falcon-efi/src/mp/mpcore.c: which processors the UEFI lane
 * runner admits as argon2id workers, from the topology the firmware's
 * GetProcessorInfo reports. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <mp/mpcore.h>

static mp_cpu cpu(uint32_t core, uint32_t thread, int bsp)
{
    mp_cpu c;

    c.package = 0;
    c.core = core;
    c.thread = thread;
    c.bsp = (uint8_t)bsp;
    c.usable = 1;
    return c;
}

/* Siblings next to each other (c0t0 c0t1 c1t0 c1t1 ...), the BSP first:
 * one worker a core, none on the BSP's. */
static void test_smt_interleaved(void **state)
{
    mp_cpu c[8];
    uint8_t w[8];
    uint32_t i;

    (void)state;
    for (i = 0; i < 8; i++)
        c[i] = cpu(i / 2u, i % 2u, i == 0);
    assert_int_equal(mp_pick_workers(c, 8, w), 3);
    assert_int_equal(w[0], 0);
    assert_int_equal(w[1], 0);
    assert_int_equal(w[2], 1);
    assert_int_equal(w[3], 0);
    assert_int_equal(w[4], 1);
    assert_int_equal(w[6], 1);
}

/* Every core's first thread, then every second thread: the same answer. */
static void test_smt_grouped(void **state)
{
    mp_cpu c[8];
    uint8_t w[8];
    uint32_t i;

    (void)state;
    for (i = 0; i < 8; i++)
        c[i] = cpu(i % 4u, i / 4u, i == 0);
    assert_int_equal(mp_pick_workers(c, 8, w), 3);
    assert_int_equal(w[1], 1);
    assert_int_equal(w[2], 1);
    assert_int_equal(w[3], 1);
    for (i = 4; i < 8; i++)
        assert_int_equal(w[i], 0);
}

/* The BSP need not be first, nor on core 0. */
static void test_bsp_elsewhere(void **state)
{
    mp_cpu c[4];
    uint8_t w[4];

    (void)state;
    c[0] = cpu(0, 0, 0);
    c[1] = cpu(1, 0, 0);
    c[2] = cpu(2, 0, 1);
    c[3] = cpu(2, 1, 0);
    assert_int_equal(mp_pick_workers(c, 4, w), 2);
    assert_int_equal(w[0], 1);
    assert_int_equal(w[1], 1);
    assert_int_equal(w[2], 0);
    assert_int_equal(w[3], 0);
}

/* A firmware that reports no topology: every usable AP works. */
static void test_no_topology(void **state)
{
    mp_cpu c[4];
    uint8_t w[4];
    uint32_t i;

    (void)state;
    for (i = 0; i < 4; i++)
        c[i] = cpu(0, 0, i == 0);
    assert_int_equal(mp_pick_workers(c, 4, w), 3);
    assert_int_equal(w[0], 0);
}

/* A disabled or failed first thread leaves its core to the second. */
static void test_unusable(void **state)
{
    mp_cpu c[6];
    uint8_t w[6];
    uint32_t i;

    (void)state;
    for (i = 0; i < 6; i++)
        c[i] = cpu(i / 2u, i % 2u, i == 0);
    c[2].usable = 0;
    c[4].usable = 0;
    c[5].usable = 0;
    assert_int_equal(mp_pick_workers(c, 6, w), 1);
    assert_int_equal(w[2], 0);
    assert_int_equal(w[3], 1);
    assert_int_equal(w[4], 0);
    assert_int_equal(w[5], 0);
}

static void test_cap_and_alone(void **state)
{
    static mp_cpu c[MP_TABLE_MAX];
    static uint8_t w[MP_TABLE_MAX];
    uint32_t i;

    (void)state;
    for (i = 0; i < MP_TABLE_MAX; i++)
        c[i] = cpu(i, 0, i == 0);
    assert_int_equal(mp_pick_workers(c, MP_TABLE_MAX, w), MP_WORKERS_MAX);
    assert_int_equal(w[MP_WORKERS_MAX], 1);
    assert_int_equal(w[MP_WORKERS_MAX + 1u], 0);

    assert_int_equal(mp_pick_workers(c, 1, w), 0);
    assert_int_equal(mp_pick_workers(c, 0, w), 0);
}

int run_mpcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_smt_interleaved),
        cmocka_unit_test(test_smt_grouped),
        cmocka_unit_test(test_bsp_elsewhere),
        cmocka_unit_test(test_no_topology),
        cmocka_unit_test(test_unusable),
        cmocka_unit_test(test_cap_and_alone),
    };

    return cmocka_run_group_tests_name("mpcore", tests, NULL, NULL);
}
