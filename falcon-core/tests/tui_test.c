/* Tests for falcon-core/src/tui/tui.c.
 *
 * The numbers here are the screens both loaders draw. 80x25 is the mode
 * every console has and the only one falcon-bios uses, so its block is
 * pinned exactly, narrow margin and all: a change that moved it would move
 * both loaders' screens, and this is where that shows up before a rig run
 * does.
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
    assert_int_equal(f.width, 78);
    assert_int_equal(f.left, 1);
    assert_int_equal(f.text_col, 3);
    assert_int_equal(f.text_width, 74);
    assert_int_equal(f.rows, 25);
    assert_int_equal(tui_page_row(&f), 8);

    assert_true(tui_mode_at(&f, 3, &row, &col));
    assert_int_equal(row, 7);
    assert_int_equal(col, 74);
    assert_false(tui_mode_at(&f, 75, &row, &col));
}

/* The narrow margin stops at 80 columns: one more and the frame is the
 * wide one's again, six a side. */
static void test_frame_narrow_boundary(void **state)
{
    tui_frame f;

    (void)state;
    tui_frame_init(&f, 81, 25);
    assert_int_equal(f.width, 69);
    assert_int_equal(f.left, 6);
    tui_frame_init(&f, 100, 31);
    assert_int_equal(f.width, 88);
    assert_int_equal(f.left, 6);
    assert_int_equal(f.text_width, 84);
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
    assert_int_equal(f.left, 1);
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
    assert_int_equal(m.left, 1);
    assert_int_equal(m.width, 78);
    assert_int_equal(m.list_left, 3);
    assert_int_equal(m.list_width, 74);
    assert_int_equal(m.win, 3);
    assert_int_equal(m.logo_rows, TUI_LOGO_ROWS);
    assert_int_equal(m.logo_row, 6);
    assert_int_equal(m.banner_row, 12);
    assert_int_equal(m.list_top, 14);
    assert_int_equal(m.hint_row, 18);
}

static void test_menu_empty_list(void **state)
{
    tui_frame f;
    tui_menu m;

    (void)state;
    tui_frame_init(&f, 80, 25);
    tui_menu_layout(&f, 0, &m);
    assert_int_equal(m.win, 1);
    assert_int_equal(m.logo_rows, TUI_LOGO_ROWS);
    assert_int_equal(m.logo_row, 7);
    assert_int_equal(m.banner_row, 13);
    assert_int_equal(m.list_top, 15);
    assert_int_equal(m.hint_row, 17);
}

static void test_menu_long_list_fills(void **state)
{
    tui_frame f;
    tui_menu m;

    (void)state;
    tui_frame_init(&f, 80, 25);
    tui_menu_layout(&f, 50, &m);
    assert_int_equal(m.win, 19);
    assert_int_equal(m.logo_rows, 0);
    assert_int_equal(m.banner_row, 1);
    assert_int_equal(m.list_top, 3);
    assert_int_equal(m.hint_row, 23);
}

/* On 80x25 the logo, its blank and the chrome leave 13 rows: a list of 13
 * keeps it, a list of 14 would scroll with it and gives the rows back. */
static void test_menu_logo_edge(void **state)
{
    tui_frame f;
    tui_menu m;

    (void)state;
    tui_frame_init(&f, 80, 25);
    tui_menu_layout(&f, 13, &m);
    assert_int_equal(m.logo_rows, TUI_LOGO_ROWS);
    assert_int_equal(m.logo_row, 1);
    assert_int_equal(m.banner_row, 7);
    assert_int_equal(m.hint_row, 23);
    tui_menu_layout(&f, 14, &m);
    assert_int_equal(m.logo_rows, 0);
    assert_int_equal(m.win, 14);
}

/* Status lines sit under the banner and push the list down by as many;
 * none is the ordinary layout, and more than TUI_STATUS_ROWS is clipped. */
static void test_menu_status(void **state)
{
    tui_frame f;
    tui_menu m;
    tui_menu z;

    (void)state;
    tui_frame_init(&f, 80, 25);
    tui_menu_layout(&f, 3, &z);
    tui_menu_layout_status(&f, 3, 0, &m);
    assert_memory_equal(&m, &z, sizeof m);
    assert_int_equal(z.status_rows, 0);
    tui_menu_layout_status(&f, 3, 2, &m);
    assert_int_equal(m.status_rows, 2);
    assert_int_equal(m.status_row, m.banner_row + 1u);
    assert_int_equal(m.list_top, m.banner_row + 4u);
    assert_int_equal(m.hint_row, m.list_top + 3u + 1u);
    assert_int_equal(m.logo_rows, TUI_LOGO_ROWS);
    tui_menu_layout_status(&f, 3, 9, &m);
    assert_int_equal(m.status_rows, TUI_STATUS_ROWS);
    /* A full screen gives the status its rows out of the list. */
    tui_menu_layout_status(&f, 50, 2, &m);
    assert_int_equal(m.win, 17);
    assert_int_equal(m.banner_row, 1);
    assert_int_equal(m.list_top, 5);
    assert_int_equal(m.hint_row, 23);
}

/* Every line of the lettering is inside the width the layout checks. */
static void test_logo_width(void **state)
{
    size_t i;
    size_t n;

    (void)state;
    for (i = 0; i < TUI_LOGO_ROWS; i++) {
        for (n = 0; tui_logo[i][n] != 0; n++)
            assert_true(tui_logo[i][n] >= 0x20 && tui_logo[i][n] < 0x7F);
        assert_true(n <= TUI_LOGO_WIDTH);
    }
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
    assert_string_equal(narrow(tui_tag_pending), "...");
}

static void test_busy_text(void **state)
{
    uint16_t buf[64];
    uint16_t tiny[4];

    (void)state;
    tui_busy_text(buf, 64, tui_busy_disks);
    assert_string_equal(narrow(buf), "finding disks...");
    tui_busy_text(buf, 64, NULL);
    assert_string_equal(narrow(buf), "...");
    tui_busy_text(tiny, 4, tui_busy_tpm);
    assert_string_equal(narrow(tiny), "rea");
}

static void test_hint_checking(void **state)
{
    uint16_t buf[64];
    uint16_t tiny[6];

    (void)state;
    tui_hint_checking(buf, 64, 3, 12);
    assert_string_equal(narrow(buf), "checking 3 of 12 | esc back");
    tui_hint_checking(buf, 64, 0, 0);
    assert_string_equal(narrow(buf), "checking 0 of 0 | esc back");
    tui_hint_checking(buf, 64, 1024, 65535);
    assert_string_equal(narrow(buf), "checking 1024 of 65535 | esc back");
    /* Clipped and still terminated. */
    tui_hint_checking(tiny, 6, 3, 12);
    assert_string_equal(narrow(tiny), "check");
}

/* The wait after each wrong passphrase of a boot (DESIGN Section 5.2). */
static void test_unlock_delay(void **state)
{
    (void)state;
    assert_int_equal(tui_unlock_delay(0), 0);
    assert_int_equal(tui_unlock_delay(1), 0);
    assert_int_equal(tui_unlock_delay(2), 4);
    assert_int_equal(tui_unlock_delay(3), 8);
    assert_int_equal(tui_unlock_delay(4), 16);
    assert_int_equal(tui_unlock_delay(9), 16);
    assert_int_equal(TUI_UNLOCK_TRIES, 5);
}

int run_tui_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_frame_80x25),
        cmocka_unit_test(test_frame_narrow_boundary),
        cmocka_unit_test(test_frame_wide_is_capped),
        cmocka_unit_test(test_frame_nonsense_is_80x25),
        cmocka_unit_test(test_frame_past_row_max),
        cmocka_unit_test(test_menu_short_list),
        cmocka_unit_test(test_menu_empty_list),
        cmocka_unit_test(test_menu_long_list_fills),
        cmocka_unit_test(test_menu_logo_edge),
        cmocka_unit_test(test_logo_width),
        cmocka_unit_test(test_menu_scroll),
        cmocka_unit_test(test_row_text),
        cmocka_unit_test(test_row_entry_aligned),
        cmocka_unit_test(test_row_entry_own_tags),
        cmocka_unit_test(test_row_entry_name_wins),
        cmocka_unit_test(test_row_banner),
        cmocka_unit_test(test_attrs),
        cmocka_unit_test(test_menu_status),
        cmocka_unit_test(test_strings),
        cmocka_unit_test(test_hint_checking),
        cmocka_unit_test(test_busy_text),
        cmocka_unit_test(test_unlock_delay),
    };

    return cmocka_run_group_tests_name("tui", tests, NULL, NULL);
}
