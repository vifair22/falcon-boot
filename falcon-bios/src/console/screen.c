/* falcon-bios: the screen. See screen.h.
 *
 * Why VGA memory and not INT 10h. Teletype output (AH=0Eh) cannot colour a
 * cell, and the UEFI menu's banner, selection bar and route code are colour;
 * AH=09h can, one character per call, and a call is a round trip through the
 * thunk down to real mode and back. Text memory at 0xB8000 is an ordinary
 * address in flat protected mode, a cell is a character and an attribute
 * byte, and the attribute byte is EFI_TEXT_ATTR's encoding already
 * (falcon-core/src/tui/tui.h). So a composed row goes in as a loop of stores,
 * and the BIOS is asked only for what it alone knows: the mode, the cursor
 * shape and position, and the keyboard.
 *
 * Why the serial mirror speaks ANSI. OVMF's terminal driver turns each
 * Simple Text Output call into an escape sequence on COM1 - a cursor move,
 * an attribute change, a clear - and the coverage harness reads the UEFI menu
 * back out of those (tools/coverage/run-case.sh finds the selected row by
 * its reverse-video attribute). The sequences here are that driver's, in its
 * order, so one reader serves both loaders.
 */

#include <start/bios.h>
#include <console/screen.h>
#include <console/cdclock.h>

#include <tui/tui.h>

#define VGA_TEXT  0xB8000u
#define COLS      ((size_t)80u)
#define ROWS      ((size_t)25u)

/* Every row is composed here, as in the UEFI console. */
static uint16_t g_row[TUI_ROW_MAX];

static tui_frame g_frame;

/* The cursor, as this module keeps it: the BIOS's own is only told where it
 * is when a caller stops writing, since every update is a thunk call. */
static size_t g_cur_row;
static size_t g_cur_col;
static size_t g_wrap;         /* glyphs on this line, for wrapping */
/* Rows the current line has wrapped onto since its last line break, so a
 * backspace at the start of one knows there is more of the same line above
 * it. The passphrase prompt is what needs this: its retry line is longer
 * than an 80-column screen's text area, and its echo and erase run across
 * the wrap. */
static size_t g_cont;

/* The route code screen_mode set, or NULL. */
static const char *g_mode;

/* The last attribute sent to serial, or 0x100 when none has been, so the
 * first run always sends one. */
static unsigned g_ser_attr = 0x100u;

static volatile uint16_t *vga(void)
{
    return (volatile uint16_t *)(uintptr_t)VGA_TEXT;
}

static void cell(size_t row, size_t col, uint16_t c, uint8_t attr)
{
    char ch = bios_ascii(c, '?');

    if (row >= ROWS || col >= COLS)
        return;
    if ((unsigned char)ch < 0x20u)
        ch = '?';
    vga()[row * COLS + col] =
        (uint16_t)(((unsigned)attr << 8) | (unsigned char)ch);
}

/* ---- the serial mirror ---- */

static void ser_dec2(size_t v)
{
    bios_putc((char)('0' + (v / 10u) % 10u));
    bios_putc((char)('0' + v % 10u));
}

/* One-based, two digits each, as EDK2's TerminalDxe writes it. */
static void ser_goto(size_t row, size_t col)
{
    bios_puts("\x1b[");
    ser_dec2(row + 1u);
    bios_putc(';');
    ser_dec2(col + 1u);
    bios_putc('H');
}

/* Brightness, then foreground, then background. The palette index is EFI's
 * and VGA's (blue is 1, red is 4); ANSI's has red and blue the other way
 * round, which is what the table is for. */
static void ser_attr(uint8_t attr)
{
    static const char ansi[8] = { '0', '4', '2', '6', '1', '5', '3', '7' };

    if (g_ser_attr == attr)
        return;
    g_ser_attr = attr;
    bios_puts("\x1b[");
    bios_putc((attr & 0x08u) ? '1' : '0');
    bios_puts("m\x1b[3");
    bios_putc(ansi[attr & 0x07u]);
    bios_puts("m\x1b[4");
    bios_putc(ansi[(attr >> 4) & 0x07u]);
    bios_putc('m');
}

static void ser_clear(void)
{
    bios_puts("\x1b[2J");
    ser_goto(0, 0);
}

/* ---- the BIOS's own cursor ---- */

static void hw_cursor_at(size_t row, size_t col)
{
    thunk_regs.eax = 0x0200u;          /* AH=02h, set cursor position */
    thunk_regs.ebx = 0;                /* page 0 */
    thunk_regs.edx = ((uint32_t)row << 8) | (uint32_t)col;
    thunk_vector = 0x10;
    thunk_bios();
}

void screen_cursor(int on)
{
    thunk_regs.eax = 0x0100u;          /* AH=01h, cursor shape */
    thunk_regs.ecx = on ? 0x0607u : 0x2000u;  /* underline, or none */
    thunk_vector = 0x10;
    thunk_bios();
    bios_puts(on ? "\x1b[?25h" : "\x1b[?25l");
}

/* ---- drawing ---- */

/* Cells [from, to) of g_row at (row, col + from), in attr. The caller has
 * already moved the serial cursor to the first of them. */
static void run(size_t row, size_t col, size_t from, size_t to, uint8_t attr)
{
    size_t i;

    ser_attr(attr);
    for (i = from; i < to; i++) {
        cell(row, col + i, g_row[i], attr);
        bios_putc(bios_ascii(g_row[i], '?'));
    }
}

/* The composed row at (row, col), in one attribute: the attribute first and
 * then the cursor move, which is the order the UEFI menu's SetAttribute and
 * SetCursorPosition put them on the wire. */
static void emit_row(size_t row, size_t col, uint8_t attr)
{
    size_t n = 0;

    while (g_row[n] != 0)
        n++;
    ser_attr(attr);
    ser_goto(row, col);
    run(row, col, 0, n, attr);
}

static void clear(void)
{
    size_t i;

    for (i = 0; i < ROWS * COLS; i++)
        vga()[i] = (uint16_t)((unsigned)TUI_ATTR_TEXT << 8 | ' ');
    ser_attr(TUI_ATTR_TEXT);
    ser_clear();
}

/* Everything moves up a row and the bottom one is blank: what the firmware
 * does when the UEFI console's cursor runs off the bottom. A terminal on the
 * serial line does the same to itself on the line break that caused it. */
static void scroll(void)
{
    size_t i;

    for (i = 0; i < (ROWS - 1u) * COLS; i++)
        vga()[i] = vga()[i + COLS];
    for (i = (ROWS - 1u) * COLS; i < ROWS * COLS; i++)
        vga()[i] = (uint16_t)((unsigned)TUI_ATTR_TEXT << 8 | ' ');
}

static void newline(void)
{
    bios_puts("\r\n");
    if (g_cur_row + 1u >= ROWS)
        scroll();
    else
        g_cur_row++;
    g_cur_col = g_frame.text_col;
    ser_goto(g_cur_row, g_cur_col);
    g_wrap = 0;
}

static void glyph(uint16_t c)
{
    ser_attr(TUI_ATTR_TEXT);
    cell(g_cur_row, g_cur_col, c, TUI_ATTR_TEXT);
    bios_putc(bios_ascii(c, '?'));
    if (g_cur_col + 1u < COLS)
        g_cur_col++;
}

/* The route code, drawn around the cursor rather than through it, as the
 * UEFI console draws it. */
static void mode_draw(void)
{
    size_t len = 0;
    size_t row;
    size_t col;
    size_t i;

    if (g_mode == (const char *)0)
        return;
    while (len < TUI_MODE_CODE_MAX && g_mode[len] != '\0')
        len++;
    if (!tui_mode_at(&g_frame, len, &row, &col))
        return;
    ser_goto(row, col);
    ser_attr(TUI_ATTR_MODE);
    for (i = 0; i < len; i++) {
        cell(row, col + i, (uint16_t)(unsigned char)g_mode[i], TUI_ATTR_MODE);
        bios_putc(g_mode[i]);
    }
    ser_attr(TUI_ATTR_TEXT);
    ser_goto(g_cur_row, g_cur_col);
}

void screen_init(void)
{
    thunk_regs.eax = 0x0003u;          /* AH=00h AL=03h: 80x25 colour text */
    thunk_vector = 0x10;
    thunk_bios();
    tui_frame_init(&g_frame, COLS, ROWS);
    clear();
}

void screen_page(void)
{
    clear();
    g_cur_row = tui_page_row(&g_frame);
    g_cur_col = g_frame.text_col;
    g_wrap = 0;
    g_cont = 0;
    ser_goto(g_cur_row, g_cur_col);
    mode_draw();
    hw_cursor_at(g_cur_row, g_cur_col);
}

void screen_mode(const char *code)
{
    g_mode = code;
    if (code != (const char *)0)
        mode_draw();
}

/* One unit of text: the line break re-indents, the right edge wraps. */
static void out_unit(uint16_t c)
{
    if (c == '\r')
        return;                         /* the \n that follows does the work */
    if (c == '\n') {
        newline();
        g_cont = 0;
        return;
    }
    if (g_frame.text_width != 0 && g_wrap >= g_frame.text_width) {
        newline();
        g_cont++;
    }
    glyph(c);
    g_wrap++;
}

/* One step back, across a wrap when the line has one: to the last text
 * column of the row above. Serial gets the move as a cursor position, since
 * a terminal's own backspace stops at its left edge. */
static void back_unit(void)
{
    if (g_wrap > 0) {
        g_wrap--;
        g_cur_col--;
    } else if (g_cont > 0 && g_cur_row > 0 && g_frame.text_width != 0) {
        g_cont--;
        g_cur_row--;
        g_wrap = g_frame.text_width - 1u;
        g_cur_col = g_frame.text_col + g_wrap;
    } else {
        return;
    }
    ser_goto(g_cur_row, g_cur_col);
}

void screen_out(const char *s)
{
    if (s == (const char *)0)
        return;
    while (*s != '\0')
        out_unit((uint16_t)(unsigned char)*s++);
    hw_cursor_at(g_cur_row, g_cur_col);
}

void screen_out16(const uint16_t *s)
{
    if (s == (const uint16_t *)0)
        return;
    while (*s != 0)
        out_unit(*s++);
    hw_cursor_at(g_cur_row, g_cur_col);
}

void screen_out_dec(uint64_t v)
{
    char buf[21];
    size_t n = sizeof buf - 1u;

    buf[n] = '\0';
    do {
        buf[--n] = (char)('0' + (unsigned)(v % 10u));
        v /= 10u;
    } while (v != 0 && n > 0);
    screen_out(&buf[n]);
}

void screen_raw(const char *s)
{
    if (s == (const char *)0)
        return;
    for (; *s != '\0'; s++) {
        if (*s == '\b')
            back_unit();
        else
            out_unit((uint16_t)(unsigned char)*s);
    }
    hw_cursor_at(g_cur_row, g_cur_col);
}

/* ---- the keyboard ---- */

int screen_key(void)
{
    return bios_getkey();
}

/* INT 16h AH=01h: the zero flag is clear when a key is waiting. It does not
 * take the key, so the read that follows is what consumes it. */
#define FLAG_ZERO 0x0040u

static int key_waiting(void)
{
    thunk_regs.eax = 0x0100u;
    thunk_vector = 0x16;
    thunk_bios();
    return (thunk_regs.flags & FLAG_ZERO) == 0;
}

/* ---- the menu ---- */

static void draw_banner(const tui_menu *m, const screen_banner *banner)
{
    size_t at = 0;
    size_t end = 0;
    size_t i;

    for (i = 0; i < m->logo_rows; i++) {
        ser_goto(m->logo_row + i, m->left);
        ser_attr(TUI_ATTR_TITLE);
        tui_row_text(g_row, m->width, tui_logo[i]);
        run(m->logo_row + i, m->left, 0, m->width, TUI_ATTR_TITLE);
    }
    ser_goto(m->banner_row, m->left);
    ser_attr(TUI_ATTR_TITLE);
    if (banner == (const screen_banner *)0) {
        tui_row_text(g_row, m->width, (const uint16_t *)0);
        run(m->banner_row, m->left, 0, m->width, TUI_ATTR_TITLE);
        return;
    }
    tui_row_banner(g_row, m->width, banner->title, banner->state, &at, &end);
    run(m->banner_row, m->left, 0, at, TUI_ATTR_TITLE);
    run(m->banner_row, m->left, at, end, banner->state_attr);
    run(m->banner_row, m->left, end, m->width, TUI_ATTR_TITLE);
}

static void paint_slot(const tui_menu *m, const uint16_t *const *labels,
                       size_t count, size_t slot, size_t top, size_t sel,
                       const uint16_t *empty)
{
    size_t idx = top + slot;

    if (idx < count) {
        tui_row_entry(g_row, m->list_width, labels[idx], (const uint16_t *)0,
                      (const uint16_t *)0, 0, 0);
        emit_row(m->list_top + slot, m->list_left,
                 tui_entry_attr(0, idx == sel));
    } else {
        tui_row_text(g_row, m->list_width,
                     (idx == 0) ? empty : (const uint16_t *)0);
        emit_row(m->list_top + slot, m->list_left, TUI_ATTR_TEXT);
    }
}

static int is_commit_key(const char *keys, int c)
{
    if (keys == (const char *)0)
        return 0;
    if (c >= 'A' && c <= 'Z')
        c = c - 'A' + 'a';
    for (; *keys != '\0'; keys++) {
        if (*keys == c)
            return 1;
    }
    return 0;
}

static void restore(void)
{
    ser_attr(TUI_ATTR_TEXT);
    screen_cursor(1);
}

/* ---- the countdown (DESIGN Section 14.7) ----
 *
 * Timed by console/cdclock.c: the BIOS's tick count in the data area, 18.2
 * a second, which IRQ0 advances while the thunk is in real mode, and the
 * RTC's seconds through INT 1Ah, whichever says more has gone, since a
 * machine can reach the menu with IRQ0 never delivered (cdclock.h). */
#define BDA_TICKS      0x46Cu

/* The RTC's seconds byte, or CDCLOCK_NO_RTC when the call says the clock
 * is not running. */
static int rtc_seconds(void)
{
    thunk_regs.eax = 0x0200u;
    thunk_regs.ecx = 0;
    thunk_regs.edx = 0;
    thunk_vector = 0x1A;
    thunk_bios();
    if ((thunk_regs.flags & BIOS_FLAG_CARRY) != 0)
        return CDCLOCK_NO_RTC;
    return (int)((thunk_regs.edx >> 8) & 0xFFu);
}

/* "Booting in 5 seconds. Any key stops the countdown." in the key line. */
static void paint_countdown(const tui_menu *m, unsigned left)
{
    static const char head[] = "Booting in ";
    static const char tail[] = ". Any key stops the countdown.";
    uint16_t text[80];
    char digits[6];
    size_t n = 0;
    size_t d = 0;
    size_t i;

    do {
        digits[d++] = (char)('0' + left % 10u);
        left /= 10u;
    } while (left != 0 && d < sizeof digits);
    for (i = 0; head[i] != '\0'; i++)
        text[n++] = (uint16_t)head[i];
    while (d > 0)
        text[n++] = (uint16_t)digits[--d];
    text[n++] = (uint16_t)' ';
    text[n++] = (uint16_t)'s';
    for (i = 0; tail[i] != '\0' && n + 1u < sizeof text / sizeof text[0]; i++)
        text[n++] = (uint16_t)tail[i];
    text[n] = 0;
    tui_row_text(g_row, m->width, text);
    emit_row(m->hint_row, m->left, TUI_ATTR_TEXT);
}

int screen_menu(const screen_banner *banner, const uint16_t *const *labels,
                size_t count, const char *keys, const uint16_t *hint,
                int *which)
{
    return screen_menu_timed(banner, labels, count, keys, hint, which,
                             tui_list_empty, 0, 0);
}

int screen_menu_timed(const screen_banner *banner,
                      const uint16_t *const *labels, size_t count,
                      const char *keys, const uint16_t *hint, int *which,
                      const uint16_t *empty, size_t sel, unsigned seconds)
{
    tui_menu m;
    size_t top = 0;
    size_t painted = 0;
    int full = 1;
    const uint16_t *line = count == 0 ? tui_hint_empty
                           : hint != (const uint16_t *)0 ? hint
                           : tui_hint_default;

    if (sel >= count)
        sel = 0;
    if (count == 0)
        seconds = 0;

    tui_menu_layout(&g_frame, count, &m);
    screen_cursor(0);
    clear();

    draw_banner(&m, banner);
    tui_row_text(g_row, m.width, line);
    emit_row(m.hint_row, m.left, TUI_ATTR_TEXT);

    for (;;) {
        size_t i;
        int key;

        if (tui_menu_scroll(&top, sel, m.win))
            full = 1;
        if (full) {
            for (i = 0; i < m.win; i++)
                paint_slot(&m, labels, count, i, top, sel, empty);
            full = 0;
        } else if (painted != sel) {
            paint_slot(&m, labels, count, painted - top, top, sel, empty);
            paint_slot(&m, labels, count, sel - top, top, sel, empty);
        }
        painted = sel;

        /* The countdown runs until it ends or a key arrives, whichever is
         * first; a key stops it for good, and is then taken as any key is. */
        if (seconds != 0) {
            uint32_t total = seconds * 10u;
            unsigned shown = seconds + 1u;
            cdclock clk;

            cdclock_start(&clk, peek32(BDA_TICKS), rtc_seconds());
            while (!key_waiting()) {
                uint32_t gone = cdclock_tenths(&clk, peek32(BDA_TICKS),
                                               rtc_seconds());
                unsigned left;

                if (gone >= total) {
                    restore();
                    if (which != (int *)0)
                        *which = '\r';
                    return (int)sel;
                }
                left = (unsigned)((total - gone + 9u) / 10u);
                if (left > seconds)
                    left = seconds;
                if (left != shown) {
                    paint_countdown(&m, left);
                    shown = left;
                }
            }
            seconds = 0;
            tui_row_text(g_row, m.width, line);
            emit_row(m.hint_row, m.left, TUI_ATTR_TEXT);
        }

        /* Drain the queue, then paint once: a held arrow arrives as a run
         * of keystrokes, and one repaint each - a couple of kilobytes on a
         * 115200 serial line - would make the repaints the backlog. */
        key = screen_key();
        for (;;) {
            if (key == BIOS_KEY_ESC) {
                restore();
                return -1;
            }
            if (count != 0) {
                if (key == BIOS_KEY_ENTER || is_commit_key(keys, key)) {
                    if (which != (int *)0)
                        *which = (key >= 'A' && key <= 'Z')
                                     ? key - 'A' + 'a' : key;
                    restore();
                    return (int)sel;
                }
                if (key == BIOS_KEY_UP && sel > 0)
                    sel--;
                else if (key == BIOS_KEY_DOWN && sel + 1u < count)
                    sel++;
            }
            if (!key_waiting())
                break;
            key = screen_key();
        }
    }
}
