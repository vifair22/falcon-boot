/* falcon-core/src/tui: the passphrase prompt. See tui.h.
 *
 * Its own translation unit on purpose: this one handles a secret and the
 * rest of the module does not, so "which code has ever held the
 * passphrase" stays a question with a one-file answer. The buffer is the
 * caller's; nothing here keeps a byte of it.
 */

#include <tui/internal.h>

#include <crypto/crypto.h>
#include <lib/string.h>

#define FIELD_W 44u

static const tui_hint k_hints[] = {
    { u"enter", u"unlock" },
    { u"tab", u"no echo" },
    { u"esc", u"back" },
};

/* The cells under the dialog and its shadow, kept from when the prompt
 * opened and put back when it closes: the page under it goes on. */
#define SAVE_W (FIELD_W + 6u + 2u)
#define SAVE_H (9u + TUI_STATUS_ROWS * 2u + 1u + 1u)

static tui_cell g_saved[SAVE_H][SAVE_W];
static size_t g_save_row;
static size_t g_save_col;

static void geometry(size_t *row, size_t *col)
{
    *row = (g_tui.rows - (SAVE_H - 1u)) / 2u;
    *col = (g_tui.cols - (SAVE_W - 2u)) / 2u;
}

static void save(void)
{
    size_t r;
    size_t c;

    geometry(&g_save_row, &g_save_col);
    for (r = 0; r < SAVE_H; r++) {
        for (c = 0; c < SAVE_W; c++) {
            const tui_cell *x = cell_at(g_save_row + r, g_save_col + c);

            if (x != (const tui_cell *)0)
                g_saved[r][c] = *x;
        }
    }
}

static void restore(void)
{
    size_t r;
    size_t c;

    for (r = 0; r < SAVE_H; r++) {
        for (c = 0; c < SAVE_W; c++) {
            tui_cell *x = cell_at(g_save_row + r, g_save_col + c);

            if (x != (tui_cell *)0) {
                *x = g_saved[r][c];
                mark(g_save_row + r);
            }
        }
    }
}

/* The dialog, the field showing `shown` asterisks (the last ones, when
 * more than fit), and the cursor after them. */
static void draw(const tui_secret_spec *s, size_t shown,
                 const tui_status *lines)
{
    const tui_display *d = g_tui.d;
    size_t extra = lines != (const tui_status *)0 ? lines->n : 0;
    size_t w = FIELD_W + 6u;
    size_t h;
    size_t row;
    size_t col;
    size_t r;
    size_t i;
    size_t n;
    uint16_t star = u'*';

    if (extra > TUI_STATUS_ROWS)
        extra = TUI_STATUS_ROWS;
    n = 0;
    for (i = 0; i < extra; i++) {
        size_t at = 0;
        size_t len;

        do {
            at = wrap_next(lines->line[i], at, FIELD_W, &len);
            n++;
        } while (lines->line[i][at] != 0);
    }
    if (n > (size_t)TUI_STATUS_ROWS * 2u)
        n = (size_t)TUI_STATUS_ROWS * 2u;
    h = 9u + (n != 0 ? n + 1u : 0u);
    geometry(&row, &col);
    restore();
    g_tui.hints = k_hints;
    g_tui.nhints = sizeof k_hints / sizeof k_hints[0];
    draw_chrome();
    box(row, col, h, w, BOX_DOUBLE, A_INFO, A_INFO);
    box_title(row, col, w, s->title, A_INFO_EMPH);
    shadow(row, col, h, w);
    r = row + 2u;
    if (s->prompt != (const uint16_t *)0) {
        n = len16(s->prompt);
        put_n(r, col + 3u, s->prompt, n < FIELD_W ? n : FIELD_W, A_INFO);
    }
    r++;
    fill(r, col + 3u, 1, FIELD_W, u' ', A_FIELD);
    n = shown < FIELD_W - 1u ? shown : FIELD_W - 1u;
    for (i = 0; i < n; i++)
        put_n(r, col + 4u + i, &star, 1, A_FIELD);
    if (d->cursor != 0)
        d->cursor(d->ctx, 1, r, col + 4u + n);
    r++;
    if (s->status != (const uint16_t *)0) {
        n = len16(s->status);
        put_n(r, col + 3u, s->status, n < FIELD_W ? n : FIELD_W,
              TUI_ATTR(TUI_RED, TUI_LIGHTGRAY));
    }
    r += 2u;
    for (i = 0; i < extra && i < TUI_STATUS_ROWS; i++) {
        size_t at = 0;
        size_t len;

        do {
            size_t next = wrap_next(lines->line[i], at, FIELD_W, &len);

            if (r + 2u < row + h)
                put_n(r, col + 3u, &lines->line[i][at], len, A_INFO);
            r++;
            at = next;
        } while (lines->line[i][at] != 0);
    }
    r = row + h - 3u;
    put_s(r, col + 3u, u"enter", A_INFO_EMPH);
    put_s(r, col + 9u, u"unlock", A_INFO);
    put_s(r, col + 18u, u"tab", A_INFO_EMPH);
    put_s(r, col + 22u, u"no echo", A_INFO);
    put_s(r, col + 32u, u"esc", A_INFO_EMPH);
    put_s(r, col + 36u, u"back", A_INFO);
    tui_flush();
}

long tui_secret(const tui_secret_spec *s, char *buf, size_t cap)
{
    const tui_display *d = g_tui.d;
    size_t len = 0;
    /* Tab erases the asterisks already shown and silences the rest of the
     * entry, so an onlooker cannot even count the passphrase's length.
     * Display only: the buffer and every key behave the same. */
    int silent = 0;
    tui_status lines;
    tui_status *lp = (tui_status *)0;
    const tui_hint *hints;
    size_t nhints;
    uint32_t sec_at;
    long r = -1;

    if (buf == (char *)0 || cap == 0)
        return -1;
    buf[0] = '\0';
    if (g_tui.loglen != 0)
        log_end();
    if (g_tui.surface == SURF_GONE)
        tui_repaint();
    hints = g_tui.hints;
    nhints = g_tui.nhints;
    save();
    if (s->title != (const uint16_t *)0)
        log_line(u"== ", s->title);
    if (s->lines != 0) {
        size_t i;

        memset(&lines, 0, sizeof lines);
        s->lines(s->ctx, &lines);
        lp = &lines;
        for (i = 0; i < lines.n && i < TUI_STATUS_ROWS; i++)
            log_line(lines.line[i], (const uint16_t *)0);
    }
    draw(s, 0, lp);
    if (d->wait != 0)
        d->wait(d->ctx, 1);
    if (d->touch != 0)
        d->touch(d->ctx);
    sec_at = d->clock(d->ctx);

    for (;;) {
        uint32_t wait = TUI_FOREVER;
        int k;

        if (lp != (tui_status *)0)
            wait = 10u - (d->clock(d->ctx) - sec_at) % 10u;
        k = d->key(d->ctx, wait);
        if (k == TUI_KEY_NONE) {
            uint32_t now = d->clock(d->ctx);

            if (lp != (tui_status *)0 && now - sec_at >= 10u) {
                tui_status now_lines;

                sec_at += 10u * ((now - sec_at) / 10u);
                memset(&now_lines, 0, sizeof now_lines);
                s->lines(s->ctx, &now_lines);
                if (!status_same(&now_lines, &lines)) {
                    size_t i;

                    lines = now_lines;
                    for (i = 0; i < lines.n && i < TUI_STATUS_ROWS; i++)
                        log_line(lines.line[i], (const uint16_t *)0);
                    draw(s, silent ? 0 : len, lp);
                }
            }
            continue;
        }
        if (k == TUI_KEY_IDLE) {
            /* Nobody finished typing: what was typed goes before the
             * machine does (DESIGN Section 7.9). */
            crypto_wipe(buf, cap);
            len = 0;
            if (d->idle_off != 0)
                d->idle_off(d->ctx);
            tui_repaint();
            draw(s, 0, lp);
            continue;
        }
        if (k == TUI_KEY_GONE || k == TUI_KEY_ESC) {
            /* Cancel wipes here rather than leaving it to the caller: a
             * cancelled prompt is the one exit where the caller has no
             * reason to think it ever held anything. */
            crypto_wipe(buf, cap);
            r = -1;
            break;
        }
        if (k == TUI_KEY_ENTER) {
            buf[len] = '\0';
            r = (long)len;
            break;
        }
        if (k == TUI_KEY_TAB) {
            silent = 1;
            draw(s, 0, lp);
            continue;
        }
        if (k == TUI_KEY_BACKSPACE) {
            if (len > 0) {
                len--;
                /* Overwrite the byte as it leaves the buffer: a passphrase
                 * retyped after a correction would otherwise leave its
                 * earlier tail sitting past len. */
                buf[len] = '\0';
                if (!silent)
                    draw(s, len, lp);
            }
            continue;
        }
        /* Printable ASCII only, and only while there is room for the byte
         * and its terminator. An over-long passphrase stops echoing, which
         * is the honest signal that the key did not land. */
        if (k < 0x20 || k > 0x7E || len + 1u >= cap)
            continue;
        buf[len++] = (char)k;
        if (!silent)
            draw(s, len, lp);
    }
    if (d->cursor != 0)
        d->cursor(d->ctx, 0, 0, 0);
    restore();
    g_tui.hints = hints;
    g_tui.nhints = nhints;
    draw_chrome();
    tui_flush();
    if (d->wait != 0)
        d->wait(d->ctx, 0);
    return r;
}
