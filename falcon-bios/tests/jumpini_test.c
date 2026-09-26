/* Tests for falcon-bios/src/jump/jumpini.c: JUMP.INI, falcon jump's default
 * and countdown (docs/DESIGN.md Section 14.7).
 *
 * What matters most is the failure side: a file jump cannot read exactly
 * must leave no default and no countdown, never a different one.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <jump/jumpini.h>

static int parse(const char *text, jump_cfg *c)
{
    return jump_ini_parse(text, strlen(text), c);
}

static void test_the_documented_file(void **state)
{
    jump_cfg c;

    (void)state;
    assert_int_equal(parse("default=hdd        ; hdd, memtest, cd or usb\n"
                           "timeout=5          ; seconds\n", &c), 0);
    assert_int_equal(c.def, JUMP_DEF_HDD);
    assert_int_equal(c.timeout, 5);
    assert_int_equal(c.err_line, 0);
}

static void test_empty_and_comments(void **state)
{
    jump_cfg c;

    (void)state;
    assert_int_equal(parse("", &c), 0);
    assert_int_equal(c.def, JUMP_DEF_NONE);
    assert_int_equal(c.timeout, 0);
    assert_int_equal(parse("\n\n; nothing\n# nor this\n   \t\n", &c), 0);
    assert_int_equal(c.def, JUMP_DEF_NONE);
    assert_int_equal(jump_ini_parse((const char *)0, 0, &c), 0);
    assert_int_equal(jump_ini_parse("x", 1, (jump_cfg *)0), -1);
}

/* Written on DOS: CRLF, upper case, spaces around the '='. */
static void test_dos_file(void **state)
{
    jump_cfg c;

    (void)state;
    assert_int_equal(parse("DEFAULT = MemTest\r\nTimeOut = 10\r\n", &c), 0);
    assert_int_equal(c.def, JUMP_DEF_MEMTEST);
    assert_int_equal(c.timeout, 10);
}

static void test_every_default(void **state)
{
    jump_cfg c;

    (void)state;
    assert_int_equal(parse("default=cd", &c), 0);
    assert_int_equal(c.def, JUMP_DEF_CD);
    assert_int_equal(parse("default=usb", &c), 0);
    assert_int_equal(c.def, JUMP_DEF_USB);
    assert_int_equal(parse("default=CF", &c), 0);
    assert_int_equal(c.def, JUMP_DEF_CF);
    /* A default with no timeout: preselected, and the menu waits. */
    assert_int_equal(c.timeout, 0);
    assert_string_equal(jump_default_name(JUMP_DEF_MEMTEST), "memtest");
    assert_string_equal(jump_default_name((jump_default)99), "");
}

static void test_refusals_name_the_line(void **state)
{
    jump_cfg c;

    (void)state;
    assert_int_equal(parse("timeout=5\ndefualt=hdd\n", &c), -1);
    assert_int_equal(c.err_line, 2);
    assert_string_equal(c.why, "unknown key 'defualt'");
    /* The failure undoes what the good line set. */
    assert_int_equal(c.def, JUMP_DEF_NONE);
    assert_int_equal(c.timeout, 0);

    assert_int_equal(parse("default=floppy\n", &c), -1);
    assert_string_equal(c.why,
                        "default 'floppy' is not hdd, memtest, cd, usb or cf");
    assert_int_equal(parse("default=hdd\ntimeout=five\n", &c), -1);
    assert_int_equal(c.err_line, 2);
    assert_string_equal(c.why,
                        "timeout 'five' is not a number of seconds up to 3600");
    assert_int_equal(parse("default=hdd\ntimeout=3601\n", &c), -1);
    assert_int_equal(parse("default=hdd\ntimeout=\n", &c), -1);
    assert_int_equal(parse("default=hdd\ntimeout=-1\n", &c), -1);
    assert_int_equal(parse("just words\n", &c), -1);
    assert_string_equal(c.why, "a line that is not key=value 'just words'");
    assert_int_equal(parse("default=hdd\ndefault=cd\n", &c), -1);
    assert_string_equal(c.why, "default is given twice");
    assert_int_equal(parse("default=hdd\ntimeout=1\ntimeout=2\n", &c), -1);
    assert_string_equal(c.why, "timeout is given twice");
}

static void test_timeout_needs_default(void **state)
{
    jump_cfg c;

    (void)state;
    assert_int_equal(parse("\ntimeout=5\n\n", &c), -1);
    assert_int_equal(c.err_line, 2);
    assert_string_equal(c.why, "timeout is set but default is not");
    /* Zero is no countdown, which needs no default. */
    assert_int_equal(parse("timeout=0\n", &c), 0);
    assert_int_equal(parse("default=hdd\ntimeout=3600\n", &c), 0);
    assert_int_equal(c.timeout, 3600);
}

/* A value long enough to fill the sentence is cut, not overrun. */
static void test_long_value_is_cut(void **state)
{
    jump_cfg c;

    (void)state;
    assert_int_equal(parse("default=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n",
                           &c), -1);
    assert_true(strlen(c.why) < sizeof c.why);
    assert_non_null(strstr(c.why, "'aaaaaaaaaaaaaaaaaaaaaaaa'"));
}

int run_jumpini_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_the_documented_file),
        cmocka_unit_test(test_empty_and_comments),
        cmocka_unit_test(test_dos_file),
        cmocka_unit_test(test_every_default),
        cmocka_unit_test(test_refusals_name_the_line),
        cmocka_unit_test(test_timeout_needs_default),
        cmocka_unit_test(test_long_value_is_cut),
    };

    return cmocka_run_group_tests_name("jumpini", tests, NULL, NULL);
}
