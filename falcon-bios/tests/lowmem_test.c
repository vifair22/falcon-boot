/* Tests for falcon-bios/src/mem/lowmem.c: where a resident's block goes at
 * the top of conventional memory, which three residents now share. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <mem/lowmem.h>

static void test_segment(void **state)
{
    (void)state;
    /* 639 KB, the common count (an EBDA takes the last one): the virtual
     * CD's 16 KB starts at 623 KB. */
    assert_int_equal(lowmem_segment_for(639u, 16u * 1024u), 623u * 64u);
    /* The reverse thunk's 1 KB, then the CD's 16 KB below it. */
    assert_int_equal(lowmem_segment_for(639u, 1024u), 638u * 64u);
    assert_int_equal(lowmem_segment_for(638u, 16u * 1024u), 622u * 64u);
}

static void test_refusals(void **state)
{
    (void)state;
    /* Past the floor: 0x98000 is 608 KB. */
    assert_int_equal(lowmem_segment_for(624u, 16u * 1024u), 608u * 64u);
    assert_int_equal(lowmem_segment_for(623u, 16u * 1024u), 0);
    assert_int_equal(lowmem_segment_for(200u, 1024u), 0);
    assert_int_equal(lowmem_segment_for(639u, 0), 0);
    assert_int_equal(lowmem_segment_for(639u, 1000u), 0);
    assert_int_equal(lowmem_segment_for(639u, 639u * 1024u), 0);
}

int run_lowmem_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_segment),
        cmocka_unit_test(test_refusals),
    };

    return cmocka_run_group_tests_name("lowmem", tests, NULL, NULL);
}
