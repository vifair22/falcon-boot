/* Tests for falcon-core/src/tui/tui.c.
 *
 * The numbers here are the screens both loaders draw. 80x25 is the mode
 * every console has and the only one falcon-bios uses, so its block is
 * pinned exactly: a change that moved it would move both loaders' screens,
 * and this is where that shows up before a rig run does.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <tui/tui.h>

static uint16_t g_row[TUI_ROW_MAX];

static void wide(uint16_t *dst, const char *s)
{
    size_t i;

    for (i = 0; s[i] != '\0'; i++)
        dst[i] = (uint16_t)(unsigned char)s[i];
    dst[i] = 0;
}

/* The row as ASCII, for a readable assert_string_equal. */
static const char *narrow(const uint16_t *s)
{
    static char buf[TUI_ROW_MAX + 1u];
    size_t i;

    for (i = 0; s[i] != 0 && i < TUI_ROW_MAX; i++)
        buf[i] = (s[i] < 128u) ? (char)s[i] : '?';
    buf[i] = '\0';
    return buf;
}

/* ---- the frame ---- */

static void test_frame_80x25(void **state)
{
    tui_frame f;
    size_t row = 0;
    size_t col = 0;

    (void)state;
    tui_frame_init(&f, 80, 25);
    assert_int_equal(f.width, 68);
    assert_int_equal(f.left, 6);
    assert_int_equal(f.text_col, 8);
    assert_int_equal(f.text_width, 64);
    assert_int_equal(f.rows, 25);
    assert_int_equal(tui_page_row(&f), 8);

    assert_true(tui_mode_at(&f, 3, &row, &col));
    assert_int_equal(row, 7);
    assert_int_equal(col, 69);
    assert_false(tui_mode_at(&f, 65, &row, &col));
}

static void test_frame_wide_is_capped(void **state)
{
    tui_frame f;

    (void)state;
    tui_frame_init(&f, 160, 42);
    assert_int_equal(f.width, TUI_WIDTH_MAX);
    assert_int_equal(f.left, 30);
    assert_int_equal(f.text_col, 32);
    assert_int_equal(f.text_width, 96);
    assert_int_equal(tui_page_row(&f), 14);
}

static void test_frame_nonsense_is_80x25(void **state)
{
    tui_frame f;

    (void)state;
    tui_frame_init(&f, 0, 0);
    assert_int_equal(f.left, 6);
    assert_int_equal(f.rows, 25);
    tui_frame_init(&f, 39, 50);
    assert_int_equal(f.rows, 25);
    tui_frame_init(&f, 80, 11);
    assert_int_equal(f.rows, 25);
}

static void test_frame_past_row_max(void **state)
{
    tui_frame f;

    (void)state;
    tui_frame_init(&f, 4000, 60);
    assert_int_equal(f.width, TUI_WIDTH_MAX);
    assert_int_equal(f.left, (TUI_ROW_MAX - TUI_WIDTH_MAX) / 2u);
}

/* ---- the menu block ---- */

static void test_menu_short_list(void **state)
{
    tui_frame f;
    tui_menu m;

    (void)state;
    tui_frame_init(&f, 80, 25);
    tui_menu_layout(&f, 3, &m);
    assert_int_equal(m.left, 6);
    assert_int_equal(m.width, 68);
    assert_int_equal(m.list_left, 8);
    assert_int_equal(m.list_width, 64);
    assert_int_equal(m.win, 3);
    assert_int_equal(m.banner_row, 9);
    assert_int_equal(m.list_top, 11);
    assert_int_equal(m.hint_row, 15);
}

static void test_menu_empty_list(void **state)
{
    tui_frame f;
    tui_menu m;

    (void)state;
    tui_frame_init(&f, 80, 25);
    tui_menu_layout(&f, 0, &m);
    assert_int_equal(m.win, 1);
    assert_int_equal(m.banner_row, 10);
    assert_int_equal(m.list_top, 12);
    assert_int_equal(m.hint_row, 14);
}

static void test_menu_long_list_fills(void **state)
{
    tui_frame f;
    tui_menu m;

    (void)state;
    tui_frame_init(&f, 80, 25);
    tui_menu_layout(&f, 50, &m);
    assert_int_equal(m.win, 19);
    assert_int_equal(m.banner_row, 1);
    assert_int_equal(m.list_top, 3);
    assert_int_equal(m.hint_row, 23);
}

static void test_menu_scroll(void **state)
{
    size_t top = 0;

    (void)state;
    assert_false(tui_menu_scroll(&top, 0, 5));
    assert_false(tui_menu_scroll(&top, 4, 5));
    assert_true(tui_menu_scroll(&top, 5, 5));
    assert_int_equal(top, 1);
    assert_true(tui_menu_scroll(&top, 9, 5));
    assert_int_equal(top, 5);
    assert_true(tui_menu_scroll(&top, 2, 5));
    assert_int_equal(top, 2);
    assert_false(tui_menu_scroll(&top, 3, 0));
}

/* ---- rows ---- */

static void test_row_text(void **state)
{
    uint16_t s[64];

    (void)state;
    wide(s, "hello");
    tui_row_text(g_row, 12, s);
    assert_string_equal(narrow(g_row), "  hello     ");

    wide(s, "a string far longer than the row");
    tui_row_text(g_row, 12, s);
    assert_string_equal(narrow(g_row), "  a string  ");

    tui_row_text(g_row, 6, (const uint16_t *)0);
    assert_string_equal(narrow(g_row), "      ");

    /* Too narrow to inset: the text takes the whole row. */
    wide(s, "abcdef");
    tui_row_text(g_row, 4, s);
    assert_string_equal(narrow(g_row), "abcd");
}

static void test_row_entry_aligned(void **state)
{
    uint16_t label[32];
    uint16_t sig[16];
    uint16_t why[16];

    (void)state;
    wide(label, "archlinux.iso");
    wide(sig, "[x]");
    wide(why, "{y}");
    tui_row_entry(g_row, 40, label, sig, (const uint16_t *)0, 3, 0);
    assert_string_equal(narrow(g_row),
                        "  archlinux.iso                    [x]  ");

    tui_row_entry(g_row, 40, label, sig, why, 3, 3);
    assert_string_equal(narrow(g_row),
                        "  archlinux.iso               [x]  {y}  ");

    /* A row with no tag in a list whose widest has one keeps the name. */
    tui_row_entry(g_row, 40, label, (const uint16_t *)0, (const uint16_t *)0,
                  3, 3);
    assert_string_equal(narrow(g_row),
                        "  archlinux.iso                         ");
}

static void test_row_entry_own_tags(void **state)
{
    uint16_t label[32];
    uint16_t sig[16];
    uint16_t why[16];

    (void)state;
    wide(label, "abc");
    wide(sig, "[x]");
    wide(why, "{y}");
    /* 16 cells between the pads, 17 needed to align: each row places its
     * own tags at the right edge instead. */
    tui_row_entry(g_row, 20, label, sig, (const uint16_t *)0, 3, 0);
    assert_string_equal(narrow(g_row), "  abc          [x]  ");

    tui_row_entry(g_row, 20, label, sig, why, 3, 3);
    assert_string_equal(narrow(g_row), "  abc     [x]  {y}  ");
}

static void test_row_entry_name_wins(void **state)
{
    uint16_t label[32];
    uint16_t sig[16];

    (void)state;
    wide(label, "abcdefgh");
    wide(sig, "[x]");
    tui_row_entry(g_row, 8, label, sig, (const uint16_t *)0, 3, 0);
    assert_string_equal(narrow(g_row), "  abcd  ");

    tui_row_entry(g_row, 8, label, (const uint16_t *)0, (const uint16_t *)0,
                  0, 0);
    assert_string_equal(narrow(g_row), "  abcd  ");
}

static void test_row_banner(void **state)
{
    uint16_t title[64];
    uint16_t sb[16];
    size_t at = 0;
    size_t end = 0;

    (void)state;
    wide(title, "falcon-boot 1.0");
    wide(sb, "SB: off");
    tui_row_banner(g_row, 30, title, sb, &at, &end);
    assert_string_equal(narrow(g_row), "  falcon-boot 1.0  SB: off    ");
    assert_int_equal(at, 17);
    assert_int_equal(end, 26);

    tui_row_banner(g_row, 30, title, (const uint16_t *)0, &at, &end);
    assert_string_equal(narrow(g_row), "  falcon-boot 1.0             ");
    assert_int_equal(at, 17);
    assert_int_equal(end, 17);

    /* The title gives way to the state. */
    wide(title, "abcdefghijklmnopqrstuvwxyz");
    tui_row_banner(g_row, 30, title, sb, &at, &end);
    assert_string_equal(narrow(g_row), "  abcdefghijklmnopq  SB: off  ");
    assert_int_equal(at, 19);
    assert_int_equal(end, 28);
}

static void test_attrs(void **state)
{
    (void)state;
    assert_int_equal(tui_entry_attr(0, 0), 0x07);
    assert_int_equal(tui_entry_attr(0, 1), 0x70);
    assert_int_equal(tui_entry_attr(1, 0), 0x0E);
    assert_int_equal(tui_entry_attr(1, 1), 0x60);
    assert_int_equal(TUI_ATTR_TITLE, 0x0B);
    assert_int_equal(TUI_ATTR_MODE, 0x0F);
}

static void test_strings(void **state)
{
    (void)state;
    assert_string_equal(narrow(tui_hint_empty), "no entries | esc back");
    assert_string_equal(narrow(tui_list_empty),
                        "(no ISOs found on the exFAT partition)");
    assert_true(strstr(narrow(tui_hint_default), "esc back") != NULL);
}

int run_tui_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_frame_80x25),
        cmocka_unit_test(test_frame_wide_is_capped),
        cmocka_unit_test(test_frame_nonsense_is_80x25),
        cmocka_unit_test(test_frame_past_row_max),
        cmocka_unit_test(test_menu_short_list),
        cmocka_unit_test(test_menu_empty_list),
        cmocka_unit_test(test_menu_long_list_fills),
        cmocka_unit_test(test_menu_scroll),
        cmocka_unit_test(test_row_text),
        cmocka_unit_test(test_row_entry_aligned),
        cmocka_unit_test(test_row_entry_own_tags),
        cmocka_unit_test(test_row_entry_name_wins),
        cmocka_unit_test(test_row_banner),
        cmocka_unit_test(test_attrs),
        cmocka_unit_test(test_strings),
    };

    return cmocka_run_group_tests_name("tui", tests, NULL, NULL);
}
