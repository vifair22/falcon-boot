/* Tests for falcon-core/src/tui: the screens, over a fake display.
 *
 * The fake records every cell it is sent, plays keys from a script, and
 * keeps a clock that moves only when a key wait times out, so a countdown
 * or a status refresh is a few script entries. Whole screens are compared
 * against snapshots in falcon-core/tests/tui/: the glyphs as their ASCII
 * stand-ins, then the attribute of every cell, foreground and background
 * as hex digits. A snapshot that differs is a screen that changed on every
 * loader at once; FALCON_TUI_SNAP=write rewrites them after a change that
 * meant to.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tui/tui.h>

/* ---- the fake display ---- */

#define FCOLS 80u
#define FROWS 25u

typedef struct {
    uint16_t ch[FROWS][FCOLS];
    uint8_t  attr[FROWS][FCOLS];
    int      cursor_on;
    size_t   cursor_row;
    size_t   cursor_col;
    const int *keys;
    size_t   nkeys;
    size_t   at;
    uint32_t clock;
    unsigned touches;
    unsigned idles;
    unsigned reclaims;
    unsigned waits;
    int      power;
    char     log[8192];
    size_t   loglen;
    size_t   puts;
} fake;

static fake g_fake;

/* A screen to snapshot when the script reaches entry g_snap_at, for a
 * screen that is only up while a call waits for its keys. */
static size_t g_snap_at = (size_t)-1;
static const char *g_snap_name;
static void snapshot(const char *name);

static void f_put(void *ctx, size_t row, size_t col, const uint16_t *s,
                  size_t n, uint8_t attr)
{
    fake *f = ctx;
    size_t i;

    assert_true(row < FROWS);
    /* The bottom right cell is never asked for. */
    assert_false(row == FROWS - 1u && col + n >= FCOLS);
    for (i = 0; i < n; i++) {
        assert_true(col + i < FCOLS);
        f->ch[row][col + i] = s[i];
        f->attr[row][col + i] = attr;
    }
    f->puts++;
}

static void f_cursor(void *ctx, int on, size_t row, size_t col)
{
    fake *f = ctx;

    f->cursor_on = on;
    f->cursor_row = row;
    f->cursor_col = col;
}

/* The script: a key, or TUI_KEY_NONE for "the wait ran out", which moves
 * the clock by the wait (a second for TUI_FOREVER). Past its end there is
 * no keyboard. */
static int f_key(void *ctx, uint32_t tenths)
{
    fake *f = ctx;
    int k;

    if (f->at == g_snap_at) {
        g_snap_at = (size_t)-1;
        snapshot(g_snap_name);
    }
    if (f->at >= f->nkeys)
        return TUI_KEY_GONE;
    k = f->keys[f->at++];
    if (k == TUI_KEY_NONE)
        f->clock += tenths == TUI_FOREVER ? 10u : (tenths == 0 ? 1u : tenths);
    return k;
}

static uint32_t f_clock(void *ctx)
{
    return ((fake *)ctx)->clock;
}

static void f_touch(void *ctx)
{
    ((fake *)ctx)->touches++;
}

static void f_idle(void *ctx)
{
    ((fake *)ctx)->idles++;
}

static jmp_buf g_off;

static void f_power(void *ctx)
{
    ((fake *)ctx)->power = 1;
    longjmp(g_off, 1);
}

static void f_reclaim(void *ctx)
{
    ((fake *)ctx)->reclaims++;
}

static void f_log(void *ctx, const uint16_t *line)
{
    fake *f = ctx;

    while (*line != 0 && f->loglen + 2u < sizeof f->log)
        f->log[f->loglen++] = (char)tui_ascii(*line++, '?');
    f->log[f->loglen++] = '\n';
    f->log[f->loglen] = '\0';
}

static void f_wait(void *ctx, int begin)
{
    (void)begin;
    ((fake *)ctx)->waits++;
}

static const tui_display g_disp = {
    &g_fake, f_put, f_cursor, f_key, f_clock, f_touch, f_idle, f_power,
    f_reclaim, f_log, f_wait,
};

static tui_cell g_screen[FCOLS * FROWS];
static tui_cell g_shown[FCOLS * FROWS];
static tui_cell g_keep[FCOLS * FROWS];

static void start(const int *keys, size_t nkeys)
{
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.keys = keys;
    g_fake.nkeys = nkeys;
    tui_init(&g_disp, FCOLS, FROWS, g_screen, g_shown, g_keep,
             sizeof g_screen / sizeof g_screen[0]);
    tui_title(u"falcon-boot 0.1.2", u"Secure Boot on, MS CA",
              TUI_ATTR_STATE);
}

/* ---- reading the screen back ---- */

static const char *row_text(size_t row)
{
    static char buf[FCOLS + 1u];
    size_t c;

    for (c = 0; c < FCOLS; c++)
        buf[c] = tui_ascii(g_fake.ch[row][c], '?');
    buf[FCOLS] = '\0';
    return buf;
}

static int screen_has(const char *s)
{
    size_t r;

    for (r = 0; r < FROWS; r++) {
        if (strstr(row_text(r), s) != NULL)
            return 1;
    }
    return 0;
}

/* The row a string is on, or -1. */
static int row_of(const char *s)
{
    size_t r;

    for (r = 0; r < FROWS; r++) {
        if (strstr(row_text(r), s) != NULL)
            return (int)r;
    }
    return -1;
}

static void snapshot(const char *name)
{
    static char now[3u * FROWS * (FCOLS + 1u) + 8u];
    static char was[sizeof now];
    char path[256];
    const char *mode = getenv("FALCON_TUI_SNAP");
    size_t n = 0;
    size_t r;
    size_t c;
    FILE *f;

    for (r = 0; r < FROWS; r++) {
        for (c = 0; c < FCOLS; c++)
            now[n++] = tui_ascii(g_fake.ch[r][c], '?');
        now[n++] = '\n';
    }
    now[n++] = '\n';
    for (r = 0; r < FROWS; r++) {
        for (c = 0; c < FCOLS; c++)
            now[n++] = "0123456789abcdef"[g_fake.attr[r][c] & 0xFu];
        now[n++] = '\n';
    }
    now[n++] = '\n';
    for (r = 0; r < FROWS; r++) {
        for (c = 0; c < FCOLS; c++)
            now[n++] = "0123456789abcdef"[g_fake.attr[r][c] >> 4];
        now[n++] = '\n';
    }
    now[n] = '\0';

    snprintf(path, sizeof path, "falcon-core/tests/tui/%s.txt", name);
    if (mode != NULL && strcmp(mode, "write") == 0) {
        f = fopen(path, "w");
        assert_non_null(f);
        fputs(now, f);
        fclose(f);
        return;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "snapshot %s missing; FALCON_TUI_SNAP=write makes it\n",
                path);
        fail();
    }
    n = fread(was, 1, sizeof was - 1u, f);
    was[n] = '\0';
    fclose(f);
    if (strcmp(now, was) != 0) {
        fprintf(stderr, "snapshot %s differs; the screen now:\n%s", path, now);
        fail();
    }
}

/* ---- the list ---- */

static tui_row g_rows[64];

static const tui_hint k_hints[] = {
    { u"enter", u"boot" }, { u"c", u"chain" }, { u"d", u"direct" },
    { u"b", u"bless" }, { u"a", u"bless new" }, { u"esc", u"back" },
};

static void library(void)
{
    static const struct { const char *label; const char *sig;
                          const char *why; int caution; } lib[] = {
        { "Personal OS", "[MOK]", NULL, 0 },
        { "archlinux-2026.09.01-x86_64.iso", "[blessed]", NULL, 0 },
        { "ubuntu-26.04.1-desktop-amd64.iso", "[MS UEFI CA]", NULL, 0 },
        { "Win11_25H2_English_x64.iso", "[MS Windows]", NULL, 0 },
        { "debian-live-13.7-amd64-kde.iso", "[unsigned]", "{NOT BLESSED}", 1 },
        { "freedos-1.4-live.iso", "[no efi]", "{BIOS ONLY}", 1 },
        { "memtest86plus-8.10.iso", "[MS UEFI CA]", NULL, 0 },
        { "Reboot to firmware setup", NULL, NULL, 0 },
    };
    static uint16_t text[8][3][48];
    size_t i;
    size_t k;

    memset(g_rows, 0, sizeof g_rows);
    for (i = 0; i < 8u; i++) {
        const char *src[3] = { lib[i].label, lib[i].sig, lib[i].why };

        for (k = 0; k < 3u; k++) {
            size_t j;

            if (src[k] == NULL)
                continue;
            for (j = 0; src[k][j] != '\0'; j++)
                text[i][k][j] = (uint16_t)src[k][j];
            text[i][k][j] = 0;
        }
        g_rows[i].label = text[i][0];
        g_rows[i].sig = lib[i].sig != NULL ? text[i][1] : NULL;
        g_rows[i].why = lib[i].why != NULL ? text[i][2] : NULL;
        g_rows[i].caution = (uint8_t)lib[i].caution;
        g_rows[i].probed = 1;
    }
}

static void detail(void *ctx, size_t idx, uint16_t *buf, size_t cap)
{
    static const uint16_t s[] = u"3.9 GB   p2 exFAT   Linux";

    (void)ctx;
    (void)idx;
    assert_true(cap > 32u);
    memcpy(buf, s, sizeof s);
}

static void note(void *ctx, uint16_t *buf, size_t cap)
{
    static const uint16_t s[] = u"8 images on p2";

    (void)ctx;
    assert_true(cap > 32u);
    memcpy(buf, s, sizeof s);
}

static void test_list_menu(void **state)
{
    /* The pause lets the move be painted: keys that arrive together are
     * taken together, and only then is the screen drawn. */
    static const int keys[] = { TUI_KEY_DOWN, TUI_KEY_NONE, TUI_KEY_ENTER };
    tui_list l;
    int which = 0;

    (void)state;
    start(keys, 3);
    library();
    memset(&l, 0, sizeof l);
    l.title = u"ISO library";
    l.rows = g_rows;
    l.count = 8;
    l.hints = k_hints;
    l.nhints = sizeof k_hints / sizeof k_hints[0];
    l.keys = "cdba";
    l.detail = detail;
    l.note = note;
    assert_int_equal(tui_list_run(&l, &which), 1);
    assert_int_equal(which, TUI_KEY_ENTER);
    snapshot("menu");
    /* The harness reads the selected row off its colours. */
    assert_int_equal(g_fake.attr[4][3], TUI_ATTR(TUI_BLACK, TUI_CYAN));
    assert_int_equal(g_fake.ch[4][3], '>');
    assert_true(screen_has("esc back"));
    assert_false(g_fake.cursor_on);
}

static void test_list_commit_key_and_escape(void **state)
{
    static const int keys1[] = { 'B' };
    static const int keys2[] = { TUI_KEY_ESC };
    tui_list l;
    int which = 0;

    (void)state;
    start(keys1, 1);
    library();
    memset(&l, 0, sizeof l);
    l.rows = g_rows;
    l.count = 8;
    l.keys = "cdba";
    assert_int_equal(tui_list_run(&l, &which), 0);
    assert_int_equal(which, 'b');
    start(keys2, 1);
    assert_int_equal(tui_list_run(&l, &which), -1);
    start(keys2, 0);
    assert_int_equal(tui_list_run(&l, &which), -1);
}

static void long_list(size_t n)
{
    static uint16_t text[64][40];
    size_t i;

    memset(g_rows, 0, sizeof g_rows);
    for (i = 0; i < n; i++) {
        {
            char a[40];
            size_t j;

            snprintf(a, sizeof a, "image-%02zu.iso", i + 1u);
            for (j = 0; a[j] != '\0'; j++)
                text[i][j] = (uint16_t)a[j];
            text[i][j] = 0;
        }
        g_rows[i].label = text[i];
        g_rows[i].probed = 1;
    }
}

static void test_list_scrolls_with_bar(void **state)
{
    int keys[24];
    tui_list l;
    size_t i;

    (void)state;
    for (i = 0; i < 19u; i++)
        keys[i] = TUI_KEY_DOWN;
    keys[19] = TUI_KEY_NONE;
    keys[20] = TUI_KEY_ENTER;
    start(keys, 21);
    long_list(41);
    memset(&l, 0, sizeof l);
    l.title = u"ISO library";
    l.rows = g_rows;
    l.count = 41;
    assert_int_equal(tui_list_run(&l, (int *)0), 19);
    snapshot("longlist");
    assert_true(screen_has("20/41"));
    /* More above and below: both arrows on the border. */
    assert_int_equal(g_fake.ch[3][77], TUI_UP);
    assert_int_equal(g_fake.ch[19][77], TUI_DOWN);
}

static void test_list_pages(void **state)
{
    static const int keys[] = { TUI_KEY_PGDN, TUI_KEY_PGDN, TUI_KEY_PGDN,
                                TUI_KEY_PGUP, TUI_KEY_END, TUI_KEY_HOME,
                                TUI_KEY_PGDN, TUI_KEY_ENTER };
    tui_list l;

    (void)state;
    start(keys, sizeof keys / sizeof keys[0]);
    long_list(41);
    memset(&l, 0, sizeof l);
    l.rows = g_rows;
    l.count = 41;
    assert_int_equal(tui_list_run(&l, (int *)0), 17);
}

static void test_list_empty(void **state)
{
    static const int keys[] = { TUI_KEY_ENTER, TUI_KEY_ESC };
    tui_list l;

    (void)state;
    start(keys, 2);
    memset(&l, 0, sizeof l);
    l.title = u"ISO library";
    l.rows = g_rows;
    l.count = 0;
    assert_int_equal(tui_list_run(&l, (int *)0), -1);
    assert_true(screen_has("(no ISOs found on the exFAT partition)"));
}

/* ---- the probe ---- */

static int g_steps;

static int probe_step(void *ctx, size_t sel)
{
    (void)ctx;
    (void)sel;
    return ++g_steps < 3;
}

static int probe_fill(void *ctx, size_t idx)
{
    (void)ctx;
    /* Row 2 is final from the second step on; the rest stay pending until
     * the probe ends. */
    if (idx == 2 && g_steps >= 2) {
        g_rows[idx].sig = u"[a much longer signer]";
        return 1;
    }
    return g_steps >= 3;
}

static void test_list_probe(void **state)
{
    static const int keys[] = { TUI_KEY_NONE, TUI_KEY_NONE, TUI_KEY_NONE,
                                TUI_KEY_NONE, TUI_KEY_ESC };
    tui_list l;
    size_t i;

    (void)state;
    start(keys, sizeof keys / sizeof keys[0]);
    library();
    for (i = 0; i < 8u; i++)
        g_rows[i].probed = 0;
    g_steps = 0;
    memset(&l, 0, sizeof l);
    l.title = u"ISO library";
    l.rows = g_rows;
    l.count = 8;
    l.step = probe_step;
    l.fill = probe_fill;
    l.note = note;
    assert_int_equal(tui_list_run(&l, (int *)0), -1);
    for (i = 0; i < 8u; i++)
        assert_int_equal(g_rows[i].probed, 1);
    assert_true(screen_has("[a much longer signer]"));
    assert_false(screen_has("..."));
}

static void test_list_pending_shows_placeholder(void **state)
{
    static const int keys[] = { TUI_KEY_ESC };
    tui_list l;

    (void)state;
    start(keys, 1);
    library();
    g_rows[3].probed = 0;
    g_steps = 0;
    memset(&l, 0, sizeof l);
    l.rows = g_rows;
    l.count = 8;
    l.fill = probe_fill;
    assert_int_equal(tui_list_run(&l, (int *)0), -1);
    assert_true(screen_has("..."));
}

/* ---- countdown and status ---- */

static unsigned g_status_calls;

static void status(void *ctx, tui_status *out)
{
    static const uint16_t code[2][8] = { u"482 917", u"105 338" };
    (void)ctx;
    memcpy(out->line[0], code[g_status_calls++ >= 2 ? 1 : 0],
           sizeof code[0]);
    out->attr[0] = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    memcpy(out->line[1], u"PCRs 2 4 7", sizeof u"PCRs 2 4 7");
    out->attr[1] = TUI_ATTR(TUI_DARKGRAY, TUI_BLACK);
    out->n = 2;
}

static void station_rows(void)
{
    static const uint16_t *labels[5] = {
        u"Gentoo (Latest)", u"Gentoo (fallback)", u"Windows Boot Manager",
        u"Memory test", u"Reboot to firmware setup",
    };
    size_t i;

    memset(g_rows, 0, sizeof g_rows);
    for (i = 0; i < 5u; i++) {
        g_rows[i].label = labels[i];
        g_rows[i].probed = 1;
    }
    g_rows[0].sig = u"default";
}

static void test_list_countdown_runs_out(void **state)
{
    static const int keys[] = { TUI_KEY_NONE, TUI_KEY_NONE, TUI_KEY_NONE,
                                TUI_KEY_NONE };
    static const tui_hint hints[] = {
        { u"enter", u"boot" }, { u"up/down", u"choose" },
        { u"any key", u"stops the count" },
    };
    tui_list l;
    int which = 0;

    (void)state;
    start(keys, sizeof keys / sizeof keys[0]);
    station_rows();
    g_status_calls = 0;
    memset(&l, 0, sizeof l);
    l.title = u"vfairfield-dt";
    l.rows = g_rows;
    l.count = 5;
    l.seconds = 3;
    l.hints = hints;
    l.nhints = 3;
    l.status = status;
    l.status_title = u"TOTP";
    assert_int_equal(tui_list_run(&l, &which), 0);
    assert_int_equal(which, TUI_KEY_ENTER);
    assert_true(g_fake.touches >= 3u);
    assert_true(g_status_calls >= 3u);
    assert_true(screen_has("105 338"));
}

static void test_list_countdown_stopped_by_key(void **state)
{
    static const int keys[] = { TUI_KEY_NONE, TUI_KEY_DOWN, TUI_KEY_NONE,
                                TUI_KEY_NONE, TUI_KEY_NONE, TUI_KEY_NONE,
                                TUI_KEY_ENTER };
    tui_list l;

    (void)state;
    start(keys, sizeof keys / sizeof keys[0]);
    station_rows();
    g_status_calls = 0;
    memset(&l, 0, sizeof l);
    l.title = u"vfairfield-dt";
    l.rows = g_rows;
    l.count = 5;
    l.seconds = 2;
    l.status = status;
    l.status_title = u"TOTP";
    assert_int_equal(tui_list_run(&l, (int *)0), 1);
    assert_false(screen_has("Booting in"));
}

static void test_list_station_snapshot(void **state)
{
    static const int keys[] = { TUI_KEY_ESC };
    tui_list l;

    (void)state;
    start(keys, 1);
    station_rows();
    g_status_calls = 0;
    memset(&l, 0, sizeof l);
    l.title = u"vfairfield-dt";
    l.rows = g_rows;
    l.count = 5;
    l.seconds = 5;
    l.status = status;
    l.status_title = u"TOTP";
    (void)tui_list_run(&l, (int *)0);
    /* Escape is a key, so the count is gone; draw it with one running. */
    start(keys, 0);
    (void)tui_list_run(&l, (int *)0);
    snapshot("station");
    assert_true(screen_has("Booting in 5 s"));
}

static void test_list_idle(void **state)
{
    static const int keys[] = { TUI_KEY_IDLE, TUI_KEY_ESC };
    tui_list l;

    (void)state;
    start(keys, 2);
    library();
    memset(&l, 0, sizeof l);
    l.rows = g_rows;
    l.count = 8;
    assert_int_equal(tui_list_run(&l, (int *)0), -1);
    assert_int_equal(g_fake.idles, 1);
    assert_int_equal(g_fake.reclaims, 1);
}

/* ---- pages ---- */

static void test_busy(void **state)
{
    (void)state;
    start(NULL, 0);
    tui_busy(tui_busy_disks);
    tui_busy(tui_busy_shelf);
    tui_busy(tui_busy_library);
    snapshot("busy");
    assert_true(strstr(g_fake.log, "finding disks...\n") != NULL);
}

static void boot_page(void)
{
    tui_boot_page(u"archlinux-2026.09.01-x86_64.iso", u"DIR");
    tui_step(u"opening the ISO");
    tui_step(u"reading the ISO's filesystem");
    tui_step(u"reading the kernel");
    tui_step_done(u"14.2 MB");
    tui_step(u"reading the initrd");
    tui_step_done(u"211 MB");
    tui_step(u"starting the kernel");
}

static void test_boot_page(void **state)
{
    (void)state;
    start(NULL, 0);
    boot_page();
    assert_memory_equal(tui_stage(), u"starting the kernel",
                        sizeof u"starting the kernel");
    tui_flush();
    snapshot("booting");
    assert_true(screen_has("DIR"));
    assert_true(strstr(g_fake.log, "booting: archlinux-2026.09.01-x86_64.iso\n"
                                   "opening the ISO...\n") != NULL);
}

static void test_handoff(void **state)
{
    (void)state;
    start(NULL, 0);
    boot_page();
    tui_handoff();
    snapshot("handoff");
    assert_true(g_fake.cursor_on);
    assert_int_equal(g_fake.cursor_row, 21);
    assert_int_equal(g_fake.cursor_col, 0);
    assert_null(tui_stage());
    /* Text after the hand-off reaches the transcript and not the screen. */
    tui_out(u"starting it\n");
    assert_true(strstr(g_fake.log, "starting it\n") != NULL);
    assert_false(screen_has("starting it"));
}

static void test_page_text_wraps_and_scrolls(void **state)
{
    size_t i;

    (void)state;
    start(NULL, 0);
    tui_page(u"blessing", u"APP");
    tui_step(u"a step that scrolls away");
    for (i = 0; i < 30u; i++) {
        tui_out_dec(i);
        tui_out(u" a line of text that is long enough to wrap across the "
                u"body of the page\r\n");
    }
    tui_note_ascii("a note");
    tui_step_ascii("a step at the bottom");
    tui_step_done((const uint16_t *)0);
    tui_flush();
    assert_true(screen_has("a step at the bottom"));
    assert_false(screen_has("a step that scrolls away"));
    assert_true(screen_has("a note"));
}

static void test_tick_turns_spinner(void **state)
{
    static const int keys[] = { TUI_KEY_NONE };
    uint16_t before;
    int r;

    (void)state;
    start(keys, 1);
    boot_page();
    r = row_of("starting the kernel");
    assert_true(r > 0);
    before = g_fake.ch[r][72];
    tui_tick();                         /* no time has passed */
    assert_int_equal(g_fake.ch[r][72], before);
    g_fake.clock++;
    tui_tick();
    assert_int_not_equal(g_fake.ch[r][72], before);
    tui_busy(u"one");
    g_fake.clock++;
    tui_tick();
}

/* ---- dialogs ---- */

static void test_failure_over_menu(void **state)
{
    static const int keys[] = { TUI_KEY_DOWN, TUI_KEY_ENTER, 'x' };
    tui_list l;

    (void)state;
    start(keys, sizeof keys / sizeof keys[0]);
    library();
    memset(&l, 0, sizeof l);
    l.title = u"ISO library";
    l.rows = g_rows;
    l.count = 8;
    l.hints = k_hints;
    l.nhints = sizeof k_hints / sizeof k_hints[0];
    assert_int_equal(tui_list_run(&l, (int *)0), 1);
    boot_page();
    tui_report(TUI_FAIL, u"blessed boot failed",
               u"archlinux-2026.09.01-x86_64.iso");
    tui_report_failed(u"blessed boot", u"LoadImage of the image (shim)",
                      0x800000000000001aull, 1);
    tui_report_advice(u"Bless it again with falcon-boot-sign, or turn Secure "
                      u"Boot off in firmware setup.");
    tui_report_end(NULL);
    snapshot("failure");
    assert_int_equal(tui_wait(), 'x');
    assert_true(strstr(g_fake.log, "blessed boot failed while LoadImage of "
                                   "the image (shim): 0x800000000000001a\n")
                != NULL);
    assert_true(strstr(g_fake.log, "press any key to return to the menu.\n")
                != NULL);
}

static void test_report_after_handoff_repaints(void **state)
{
    (void)state;
    start(NULL, 0);
    boot_page();
    tui_handoff();
    tui_report(TUI_INFO, u"Personal OS", u"Gentoo");
    tui_report_exited(u"the image", 0);
    tui_report_failed(u"BIOS boot", NULL, 0, 0);
    tui_report_end(NULL);
    assert_int_equal(g_fake.reclaims, 1);
    assert_true(screen_has("the image exited, status 0x0"));
    assert_true(screen_has("BIOS boot failed while starting."));
}

static void test_ask(void **state)
{
    static const int keys[] = { 'Y' };

    (void)state;
    start(keys, 1);
    assert_int_equal(tui_ask(u"Firmware setup",
                             u"Reboot into this machine's firmware setup?",
                             u"y to reboot, any other key to go back."),
                     'y');
    snapshot("confirm");
    start(keys, 0);
    assert_int_equal(tui_ask(u"t", u"q", u"k"), TUI_KEY_GONE);
}

static void test_dialog_long_text(void **state)
{
    size_t i;

    (void)state;
    start(NULL, 0);
    tui_report(TUI_FAIL, u"a very long report", NULL);
    for (i = 0; i < 40u; i++)
        tui_out(u"signature: someone <- something (db)\n");
    tui_out(u"averyveryveryveryveryveryveryveryveryveryveryveryveryverylongword"
            u"thatcannotbreak and then more\n");
    tui_report_end(NULL);
    assert_true(screen_has("press any key"));
}

static void test_wait_idle_and_pause(void **state)
{
    static const int keys[] = { TUI_KEY_IDLE, TUI_KEY_NONE, 'q',
                                TUI_KEY_NONE, 'z', 'z' };

    (void)state;
    start(keys, sizeof keys / sizeof keys[0]);
    assert_int_equal(tui_wait(), 'q');
    assert_int_equal(g_fake.idles, 1);
    tui_pause(1);
    assert_int_equal(g_fake.at, g_fake.nkeys);
}

static void test_power_off(void **state)
{
    (void)state;
    start(NULL, 0);
    if (setjmp(g_off) == 0)
        tui_power_off();
    assert_int_equal(g_fake.power, 1);
}

/* ---- the passphrase ---- */

static void test_secret_typed(void **state)
{
    static const int keys[] = { 'h', 'u', 'n', TUI_KEY_BACKSPACE, 't',
                                TUI_KEY_UP, 0x07, TUI_KEY_ENTER };
    tui_secret_spec s;
    char buf[16];

    (void)state;
    start(keys, sizeof keys / sizeof keys[0]);
    boot_page();
    memset(&s, 0, sizeof s);
    s.title = u"Unlock the shelf";
    s.prompt = u"Passphrase for the encrypted partition:";
    assert_int_equal(tui_secret(&s, buf, sizeof buf), 3);
    assert_string_equal(buf, "hut");
    /* The page comes back from under it. */
    assert_true(screen_has("starting the kernel"));
    assert_false(screen_has("Unlock the shelf"));
}

static void test_secret_snapshot(void **state)
{
    static const int keys[] = { 'a', 'b', 'c' };
    tui_secret_spec s;
    char buf[16];

    (void)state;
    start(keys, sizeof keys / sizeof keys[0]);
    tui_page(u"Personal OS", u"POS");
    memset(&s, 0, sizeof s);
    s.title = u"Unlock the shelf";
    s.prompt = u"Passphrase for the encrypted partition:";
    s.status = u"wrong passphrase.";
    /* The keys run out with the dialog up: no keyboard is a cancel. */
    g_snap_at = 3;
    g_snap_name = "secret";
    assert_int_equal(tui_secret(&s, buf, sizeof buf), -1);
    assert_true(g_snap_at == (size_t)-1);
    assert_string_equal(buf, "");
}

static void test_secret_silent_full_and_escape(void **state)
{
    static const int keys[] = { 'a', TUI_KEY_TAB, 'b', 'c', 'd', 'e',
                                TUI_KEY_BACKSPACE, TUI_KEY_ESC };
    tui_secret_spec s;
    char buf[4];

    (void)state;
    start(keys, sizeof keys / sizeof keys[0]);
    memset(&s, 0, sizeof s);
    s.title = u"Unlock the shelf";
    assert_int_equal(tui_secret(&s, buf, sizeof buf), -1);
    assert_memory_equal(buf, "\0\0\0\0", 4);
}

static void test_secret_idle_wipes(void **state)
{
    static const int keys[] = { 'a', 'b', TUI_KEY_IDLE, 'c', TUI_KEY_ENTER };
    tui_secret_spec s;
    char buf[8];

    (void)state;
    start(keys, sizeof keys / sizeof keys[0]);
    memset(&s, 0, sizeof s);
    s.title = u"Unlock the shelf";
    assert_int_equal(tui_secret(&s, buf, sizeof buf), 1);
    assert_string_equal(buf, "c");
    assert_int_equal(g_fake.idles, 1);
}

static void test_secret_lines_refresh(void **state)
{
    static const int keys[] = { TUI_KEY_NONE, TUI_KEY_NONE, TUI_KEY_NONE,
                                'x', TUI_KEY_ENTER };
    tui_secret_spec s;
    char buf[8];

    (void)state;
    start(keys, sizeof keys / sizeof keys[0]);
    g_status_calls = 0;
    memset(&s, 0, sizeof s);
    s.title = u"Unlock the shelf";
    s.prompt = u"Passphrase:";
    s.lines = status;
    assert_int_equal(tui_secret(&s, buf, sizeof buf), 1);
    assert_true(g_status_calls >= 3u);
}

/* ---- glyphs and the rest ---- */

static void test_glyphs(void **state)
{
    (void)state;
    assert_int_equal(tui_cp437('A', '?'), 'A');
    assert_int_equal(tui_cp437(TUI_HLINE, '?'), 0xC4);
    assert_int_equal(tui_cp437(0x2554u, '?'), 0xC9);
    assert_int_equal(tui_cp437(TUI_SHADE, '?'), 0xB0);
    assert_int_equal(tui_cp437(TUI_BLOCK, '?'), 0xDB);
    assert_int_equal(tui_cp437(TUI_UP, '?'), 0x1E);
    assert_int_equal(tui_cp437(TUI_DOWN, '?'), 0x1F);
    assert_int_equal(tui_cp437(0x00E9u, '?'), '?');
    assert_int_equal(tui_ascii(0x250Cu, '?'), '+');
    assert_int_equal(tui_ascii(0x255Du, '?'), '+');
    assert_int_equal(tui_ascii(TUI_HLINE, '?'), '-');
    assert_int_equal(tui_ascii(0x2550u, '?'), '=');
    assert_int_equal(tui_ascii(0x2551u, '?'), '|');
    assert_int_equal(tui_ascii(TUI_SHADE, '?'), ' ');
    assert_int_equal(tui_ascii(TUI_BLOCK, '?'), '#');
    assert_int_equal(tui_ascii(TUI_UP, '?'), '^');
    assert_int_equal(tui_ascii(TUI_DOWN, '?'), 'v');
    assert_int_equal(tui_ascii(0x00E9u, '?'), '?');
    assert_int_equal(tui_spin(0), '/');
    assert_int_equal(tui_spin(1), TUI_HLINE);
    assert_int_equal(tui_spin(6), '\\');
    assert_int_equal(tui_unlock_delay(0), 0);
    assert_int_equal(tui_unlock_delay(1), 0);
    assert_int_equal(tui_unlock_delay(2), 4);
    assert_int_equal(tui_unlock_delay(4), 16);
    assert_int_equal(tui_unlock_delay(9), 16);
}

static void test_title_gives_way(void **state)
{
    (void)state;
    start(NULL, 0);
    tui_title(u"falcon-boot 0.1.2_20261014.2350.release with a long tail "
              u"that cannot fit",
              u"SB: on  trusts: MOK, MS UEFI CA, +1", TUI_ATTR_ALERT);
    tui_code(u"DIR");
    tui_flush();
    assert_true(screen_has("SB: on  trusts: MOK, MS UEFI CA, +1"));
    assert_int_equal(g_fake.attr[0][1], TUI_ATTR(TUI_LIGHTCYAN, TUI_BLACK));
    tui_code(NULL);
}

static size_t g_big_puts;

static void big_put(void *ctx, size_t row, size_t col, const uint16_t *s,
                    size_t n, uint8_t attr)
{
    (void)ctx;
    (void)s;
    (void)attr;
    assert_true(row < 37u);
    assert_true(col + n <= TUI_COLS_MAX);
    g_big_puts++;
}

static void test_init_small_and_large(void **state)
{
    static tui_cell big[TUI_COLS_MAX * TUI_ROWS_MAX];
    static tui_cell big2[TUI_COLS_MAX * TUI_ROWS_MAX];
    static tui_cell big3[TUI_COLS_MAX * TUI_ROWS_MAX];
    tui_display d = g_disp;

    (void)state;
    memset(&g_fake, 0, sizeof g_fake);
    /* Nonsense is 80x25; a display larger than the buffers is cut down,
     * columns to TUI_COLS_MAX and rows to what the buffer holds. */
    tui_init(&g_disp, 10, 5, g_screen, g_shown, g_keep, FCOLS * FROWS);
    tui_flush();
    assert_true(g_fake.puts > 0u);
    d.put = big_put;
    d.cursor = NULL;
    tui_init(&d, 1000, 1000, big, big3, big2, 240u * 37u);
    tui_busy(u"x");
    assert_true(g_big_puts > 0u);
    start(NULL, 0);
}

int run_tui_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_list_menu),
        cmocka_unit_test(test_list_commit_key_and_escape),
        cmocka_unit_test(test_list_scrolls_with_bar),
        cmocka_unit_test(test_list_pages),
        cmocka_unit_test(test_list_empty),
        cmocka_unit_test(test_list_probe),
        cmocka_unit_test(test_list_pending_shows_placeholder),
        cmocka_unit_test(test_list_countdown_runs_out),
        cmocka_unit_test(test_list_countdown_stopped_by_key),
        cmocka_unit_test(test_list_station_snapshot),
        cmocka_unit_test(test_list_idle),
        cmocka_unit_test(test_busy),
        cmocka_unit_test(test_boot_page),
        cmocka_unit_test(test_handoff),
        cmocka_unit_test(test_page_text_wraps_and_scrolls),
        cmocka_unit_test(test_tick_turns_spinner),
        cmocka_unit_test(test_failure_over_menu),
        cmocka_unit_test(test_report_after_handoff_repaints),
        cmocka_unit_test(test_ask),
        cmocka_unit_test(test_dialog_long_text),
        cmocka_unit_test(test_wait_idle_and_pause),
        cmocka_unit_test(test_power_off),
        cmocka_unit_test(test_secret_typed),
        cmocka_unit_test(test_secret_snapshot),
        cmocka_unit_test(test_secret_silent_full_and_escape),
        cmocka_unit_test(test_secret_idle_wipes),
        cmocka_unit_test(test_secret_lines_refresh),
        cmocka_unit_test(test_glyphs),
        cmocka_unit_test(test_title_gives_way),
        cmocka_unit_test(test_init_small_and_large),
    };

    return cmocka_run_group_tests_name("tui", tests, NULL, NULL);
}
