/* falcon-core/src/tui: the screen buffer, its primitives and the chrome.
 * See tui.h. */

#include <tui/internal.h>

#include <lib/string.h>

tui_state g_tui;

/* ---- glyphs ---- */

/* The box-drawing set the boxes use, single and double, in the order
 * top left, top right, bottom left, bottom right, horizontal, vertical. */
static const uint16_t k_box[2][6] = {
    { 0x250Cu, 0x2510u, 0x2514u, 0x2518u, 0x2500u, 0x2502u },
    { 0x2554u, 0x2557u, 0x255Au, 0x255Du, 0x2550u, 0x2551u },
};

/* Code page 437's bytes for them, in the same order. */
static const uint8_t k_box437[2][6] = {
    { 0xDAu, 0xBFu, 0xC0u, 0xD9u, 0xC4u, 0xB3u },
    { 0xC9u, 0xBBu, 0xC8u, 0xBCu, 0xCDu, 0xBAu },
};

uint8_t tui_cp437(uint16_t c, uint8_t fallback)
{
    size_t s;
    size_t i;

    if (c < 0x80u)
        return (uint8_t)c;
    for (s = 0; s < 2u; s++) {
        for (i = 0; i < 6u; i++) {
            if (k_box[s][i] == c)
                return k_box437[s][i];
        }
    }
    switch (c) {
    case TUI_SHADE: return 0xB0u;
    case TUI_BLOCK: return 0xDBu;
    case TUI_UP:    return 0x1Eu;
    case TUI_DOWN:  return 0x1Fu;
    default:        return fallback;
    }
}

char tui_ascii(uint16_t c, char fallback)
{
    size_t s;

    if (c < 0x80u)
        return (char)c;
    for (s = 0; s < 2u; s++) {
        if (c == k_box[s][0] || c == k_box[s][1] || c == k_box[s][2] ||
            c == k_box[s][3])
            return '+';
        if (c == k_box[s][4])
            return s == 0 ? '-' : '=';
        if (c == k_box[s][5])
            return '|';
    }
    switch (c) {
    case TUI_SHADE: return ' ';
    case TUI_BLOCK: return '#';
    case TUI_UP:    return '^';
    case TUI_DOWN:  return 'v';
    default:        return fallback;
    }
}

uint16_t tui_spin(unsigned phase)
{
    static const uint16_t glyph[4] = { u'/', TUI_HLINE, u'\\', u'|' };

    return glyph[phase % 4u];
}

unsigned tui_unlock_delay(unsigned failures)
{
    /* The first is free: it is nearly always the owner's typo, and the
     * power-off after TUI_UNLOCK_TRIES is what bounds a guesser. */
    if (failures <= 1u)
        return 0;
    if (failures > 4u)
        return 16u;
    return 2u << (failures - 1u);
}

size_t len16(const uint16_t *s)
{
    size_t n = 0;

    if (s == (const uint16_t *)0)
        return 0;
    while (s[n] != 0)
        n++;
    return n;
}

/* ---- the buffer ---- */

tui_cell *cell_at(size_t row, size_t col)
{
    if (g_tui.cells == (tui_cell *)0 || row >= g_tui.rows ||
        col >= g_tui.cols)
        return (tui_cell *)0;
    return &g_tui.cells[row * g_tui.cols + col];
}

void mark(size_t row)
{
    if (row < g_tui.rows)
        g_tui.dirty[row] = 1;
}

void mark_all(void)
{
    size_t r;

    for (r = 0; r < g_tui.rows; r++)
        g_tui.dirty[r] = 1;
}

void put_n(size_t row, size_t col, const uint16_t *s, size_t n, uint8_t a)
{
    size_t i;

    for (i = 0; i < n; i++) {
        tui_cell *c = cell_at(row, col + i);

        if (c == (tui_cell *)0)
            break;
        if (c->ch != s[i] || c->attr != a) {
            c->ch = s[i];
            c->attr = a;
            g_tui.dirty[row] = 1;
        }
    }
}

size_t put_s(size_t row, size_t col, const uint16_t *s, uint8_t a)
{
    size_t n = len16(s);

    put_n(row, col, s, n, a);
    return n;
}

size_t put_a(size_t row, size_t col, const char *s, uint8_t a)
{
    size_t n = 0;

    if (s == (const char *)0)
        return 0;
    while (s[n] != '\0') {
        uint16_t c = (uint16_t)(unsigned char)s[n];

        put_n(row, col + n, &c, 1, a);
        n++;
    }
    return n;
}

void fill(size_t row, size_t col, size_t h, size_t w, uint16_t ch,
          uint8_t a)
{
    size_t r;
    size_t c;

    for (r = row; r < row + h; r++) {
        for (c = col; c < col + w; c++)
            put_n(r, c, &ch, 1, a);
    }
}

void box(size_t row, size_t col, size_t h, size_t w, int style, uint8_t a,
         uint8_t fill_a)
{
    const uint16_t *k = k_box[style == BOX_DOUBLE ? 1 : 0];
    size_t i;

    if (h < 2u || w < 2u)
        return;
    fill(row + 1u, col + 1u, h - 2u, w - 2u, u' ', fill_a);
    put_n(row, col, &k[0], 1, a);
    put_n(row, col + w - 1u, &k[1], 1, a);
    put_n(row + h - 1u, col, &k[2], 1, a);
    put_n(row + h - 1u, col + w - 1u, &k[3], 1, a);
    for (i = col + 1u; i < col + w - 1u; i++) {
        put_n(row, i, &k[4], 1, a);
        put_n(row + h - 1u, i, &k[4], 1, a);
    }
    for (i = row + 1u; i < row + h - 1u; i++) {
        put_n(i, col, &k[5], 1, a);
        put_n(i, col + w - 1u, &k[5], 1, a);
    }
}

size_t border_text(size_t row, size_t at, size_t end, const uint16_t *s,
                   uint8_t a)
{
    size_t n = len16(s);
    uint16_t sp = u' ';

    if (s == (const uint16_t *)0 || n == 0 || at + 2u >= end)
        return at;
    if (at + n + 2u > end)
        n = end - at - 2u;
    put_n(row, at, &sp, 1, a);
    put_n(row, at + 1u, s, n, a);
    put_n(row, at + 1u + n, &sp, 1, a);
    return at + n + 2u;
}

void box_title(size_t row, size_t col, size_t w, const uint16_t *title,
               uint8_t a)
{
    if (w > 4u)
        (void)border_text(row, col + 2u, col + w - 2u, title, a);
}

/* What is under a box goes dark rather than being drawn over: one row
 * under it, and two columns right of it, since a cell is twice as tall as
 * it is wide. */
void shadow(size_t row, size_t col, size_t h, size_t w)
{
    size_t r;
    size_t c;

    for (r = row + 1u; r <= row + h; r++) {
        for (c = col + w; c < col + w + 2u; c++) {
            tui_cell *x = cell_at(r, c);

            if (x == (tui_cell *)0 || r + 1u >= g_tui.rows)
                continue;
            if (x->ch == TUI_SHADE)
                x->ch = u' ';
            x->attr = A_DIM;
            mark(r);
        }
    }
    for (c = col + 2u; c < col + w; c++) {
        tui_cell *x = cell_at(row + h, c);

        if (x == (tui_cell *)0 || row + h + 1u >= g_tui.rows)
            continue;
        if (x->ch == TUI_SHADE)
            x->ch = u' ';
        x->attr = A_DIM;
        mark(row + h);
    }
}

/* ---- geometry ---- */

void area(size_t *l, size_t *r)
{
    size_t w = g_tui.cols - 4u;

    if (w > AREA_MAX)
        w = AREA_MAX;
    *l = (g_tui.cols - w) / 2u;
    *r = *l + w - 1u;
}

size_t panel_top(void)
{
    return 2u;
}

/* The panel ends three rows above the hint bar: a row for the line under
 * it, and the backdrop around both. */
size_t panel_bottom(void)
{
    return g_tui.rows - 5u;
}

/* ---- the layers ---- */

void backdrop(void)
{
    fill(1, 0, g_tui.rows - 2u, g_tui.cols, TUI_SHADE, A_BACKDROP);
}

void chrome(const tui_hint *hints, size_t n)
{
    g_tui.hints = hints;
    g_tui.nhints = n;
    draw_chrome();
}

void draw_chrome(void)
{
    size_t last = g_tui.rows - 1u;
    size_t cols = g_tui.cols;
    size_t state_n = len16(g_tui.state);
    size_t stop = cols - 1u;
    size_t col = 1;
    size_t i;

    fill(0, 0, 1, cols, u' ', A_BAR);
    if (state_n != 0 && state_n + 2u < cols) {
        stop = cols - 1u - state_n;
        put_s(0, stop, g_tui.state, g_tui.state_attr);
        stop = stop > 2u ? stop - 2u : 0;
    }
    /* The title: its first word in the product's colour, the rest grey,
     * clipped where the state starts. */
    if (g_tui.title != (const uint16_t *)0) {
        for (i = 0; g_tui.title[i] != 0 && col < stop; i++, col++) {
            uint8_t a = A_BAR;
            size_t j;
            int first = 1;

            for (j = 0; j < i; j++) {
                if (g_tui.title[j] == u' ')
                    first = 0;
            }
            if (first && g_tui.title[i] != u' ')
                a = A_PRODUCT;
            put_n(0, col, &g_tui.title[i], 1, a);
        }
    }
    if (g_tui.code != (const uint16_t *)0 && col + 2u < stop) {
        size_t n = len16(g_tui.code);

        if (col + 2u + n <= stop)
            put_s(0, col + 2u, g_tui.code, A_CODE);
    }

    fill(last, 0, 1, cols, u' ', A_BAR);
    col = 1;
    for (i = 0; i < g_tui.nhints; i++) {
        size_t kn = len16(g_tui.hints[i].key);
        size_t wn = len16(g_tui.hints[i].word);

        if (col + kn + 1u + wn >= cols)
            break;
        put_s(last, col, g_tui.hints[i].key, A_HINT_KEY);
        col += kn + 1u;
        put_s(last, col, g_tui.hints[i].word, A_BAR);
        col += wn + 3u;
    }
}

/* ---- the display ---- */

/* What no cell is: the shown copy says this when the display's contents
 * are not known, so every cell differs from it. */
#define CELL_UNKNOWN 0xFFFFu

static void forget_shown(void)
{
    size_t i;

    for (i = 0; i < g_tui.cols * g_tui.rows; i++) {
        g_tui.shown[i].ch = CELL_UNKNOWN;
        g_tui.shown[i].attr = 0;
    }
}

void tui_init(const tui_display *d, size_t cols, size_t rows,
              tui_cell *screen, tui_cell *shown, tui_cell *keep, size_t cap)
{
    size_t i;

    memset(&g_tui, 0, sizeof g_tui);
    if (cols < 80u || rows < 25u) {
        cols = 80u;
        rows = 25u;
    }
    if (cols > TUI_COLS_MAX)
        cols = TUI_COLS_MAX;
    if (rows > TUI_ROWS_MAX)
        rows = TUI_ROWS_MAX;
    while (rows > 25u && cols * rows > cap)
        rows--;
    g_tui.d = d;
    g_tui.cols = cols;
    g_tui.rows = rows;
    g_tui.cells = screen;
    g_tui.shown = shown;
    g_tui.keep = keep;
    g_tui.state_attr = TUI_ATTR_STATE;
    for (i = 0; i < cols * rows && i < cap; i++) {
        screen[i].ch = u' ';
        screen[i].attr = TUI_ATTR_TEXT;
    }
    forget_shown();
    if (d->cursor != 0)
        d->cursor(d->ctx, 0, 0, 0);
    backdrop();
    chrome((const tui_hint *)0, 0);
    mark_all();
}

void tui_title(const uint16_t *title, const uint16_t *state,
               uint8_t state_attr)
{
    /* The transcript hears the title bar when it changes: the version a
     * photo of the screen would show, and the Secure Boot state. */
    if (title != g_tui.title || state != g_tui.state) {
        const uint16_t *s;

        if (g_tui.loglen != 0)
            log_end();
        for (s = title; s != (const uint16_t *)0 && *s != 0; s++)
            log_unit(*s);
        if (state != (const uint16_t *)0) {
            log_unit(u' ');
            log_unit(u' ');
            for (s = state; *s != 0; s++)
                log_unit(*s);
        }
        log_end();
    }
    g_tui.title = title;
    g_tui.state = state;
    g_tui.state_attr = state_attr;
    if (g_tui.cells != (tui_cell *)0)
        draw_chrome();
}

void tui_code(const uint16_t *code)
{
    g_tui.code = code;
    if (g_tui.cells != (tui_cell *)0)
        draw_chrome();
}

void tui_code_ascii(const char *code)
{
    size_t i;

    if (code == (const char *)0) {
        tui_code((const uint16_t *)0);
        return;
    }
    for (i = 0; code[i] != '\0' && i + 1u < sizeof g_tui.code_buf / 2u; i++)
        g_tui.code_buf[i] = (uint16_t)(unsigned char)code[i];
    g_tui.code_buf[i] = 0;
    tui_code(g_tui.code_buf);
}

/* A run may carry a few cells that did not change rather than end and
 * start again: one call for a row's changes, not one per gap in them. */
#define GAP_MAX 4u

static int same(const tui_cell *a, const tui_cell *b)
{
    return a->ch == b->ch && a->attr == b->attr;
}

void tui_flush(void)
{
    static uint16_t run[TUI_COLS_MAX];
    const tui_display *d = g_tui.d;
    size_t r;

    if (d == (const tui_display *)0 || d->put == 0)
        return;
    for (r = 0; r < g_tui.rows; r++) {
        const tui_cell *x = &g_tui.cells[r * g_tui.cols];
        tui_cell *s = &g_tui.shown[r * g_tui.cols];
        size_t end = g_tui.cols;
        size_t c = 0;

        if (!g_tui.dirty[r])
            continue;
        g_tui.dirty[r] = 0;
        /* Never the bottom right cell: a console scrolls when it is
         * written. */
        if (r + 1u == g_tui.rows)
            end--;
        while (c < end) {
            uint8_t a;
            size_t n = 0;
            size_t last = 0;

            if (same(&x[c], &s[c])) {
                c++;
                continue;
            }
            /* From the first changed cell, while the colour holds and the
             * unchanged cells in a row stay few; the run ends at its last
             * changed cell. */
            a = x[c].attr;
            while (c + n < end && x[c + n].attr == a &&
                   n - last <= GAP_MAX) {
                if (!same(&x[c + n], &s[c + n]))
                    last = n;
                n++;
            }
            n = last + 1u;
            for (last = 0; last < n; last++) {
                run[last] = x[c + last].ch;
                s[c + last] = x[c + last];
            }
            d->put(d->ctx, r, c, run, n, a);
            c += n;
        }
    }
}

void tui_repaint(void)
{
    const tui_display *d = g_tui.d;

    if (d != (const tui_display *)0 && d->reclaim != 0)
        d->reclaim(d->ctx);
    if (d != (const tui_display *)0 && d->cursor != 0)
        d->cursor(d->ctx, 0, 0, 0);
    forget_shown();
    mark_all();
}

/* ---- the transcript ---- */

void log_end(void)
{
    const tui_display *d = g_tui.d;

    g_tui.logline[g_tui.loglen] = 0;
    if (d != (const tui_display *)0 && d->log != 0)
        d->log(d->ctx, g_tui.logline);
    g_tui.loglen = 0;
}

void log_unit(uint16_t c)
{
    if (c == u'\r')
        return;
    if (c == u'\n') {
        log_end();
        return;
    }
    if (g_tui.loglen + 1u < LOG_MAX)
        g_tui.logline[g_tui.loglen++] = c;
}

/* A whole line, a then b, with whatever was in progress ended first. */
void log_line(const uint16_t *a, const uint16_t *b)
{
    if (g_tui.loglen != 0)
        log_end();
    while (a != (const uint16_t *)0 && *a != 0)
        log_unit(*a++);
    while (b != (const uint16_t *)0 && *b != 0)
        log_unit(*b++);
    log_end();
}
