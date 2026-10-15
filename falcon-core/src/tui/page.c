/* falcon-core/src/tui: pages, steps, the startup box, dialogs and the text
 * that goes into them. See tui.h. */

#include <tui/internal.h>

#include <lib/string.h>

const uint16_t tui_busy_disks[]   = u"finding disks";
const uint16_t tui_busy_keys[]    = u"reading the Secure Boot keys";
const uint16_t tui_busy_shelf[]   = u"reading the shelf";
const uint16_t tui_busy_library[] = u"reading the ISO library";
const uint16_t tui_busy_tpm[]     = u"reading the TPM";
const uint16_t tui_busy_entries[] = u"finding boot entries";
const uint16_t tui_busy_floppy[]  = u"reading the floppy";
const uint16_t tui_busy_image[]   = u"checking the image";

const uint16_t tui_press_any[] = u"press any key to return to the menu.";

static const uint16_t k_ok[] = u"OK";

static const tui_hint k_report_hints[] = {
    { u"any key", u"return to the menu" },
};

/* ---- colours by style ---- */

uint8_t page_attr(int style)
{
    if (style == TUI_DIM)
        return A_DIM;
    if (style == TUI_EMPH)
        return A_EMPH;
    return A_BODY;
}

uint8_t dialog_attr(int kind, int style)
{
    if (kind == TUI_FAIL) {
        if (style == TUI_DIM)
            return A_FAIL_DIM;
        if (style == TUI_EMPH)
            return A_FAIL_EMPH;
        return A_FAIL;
    }
    if (style == TUI_DIM)
        return A_INFO_DIM;
    if (style == TUI_EMPH)
        return A_INFO_EMPH;
    return A_INFO;
}

void tui_style(int style)
{
    g_tui.style = style;
}

/* ---- the page ---- */

/* The page's body: the rows and columns its text uses. One blank row under
 * the top border and none over the bottom one; three columns in from each
 * border, which leaves the status column room right of a step. */
static size_t body_top(void)    { return g_tui.pt + 2u; }
static size_t body_bottom(void) { return g_tui.pb - 1u; }
static size_t body_left(void)   { return g_tui.pl + 3u; }
static size_t body_width(void)  { return g_tui.pr - g_tui.pl - 5u; }

/* A step's status, right-aligned to end four columns in from the border,
 * and the spinner in the cell where a short status starts. */
static size_t status_end(void)  { return g_tui.pr - 4u; }
static size_t spin_col(void)    { return g_tui.pr - 5u; }

static void page_frame(const uint16_t *title)
{
    size_t l;
    size_t r;

    area(&l, &r);
    g_tui.pl = l;
    g_tui.pr = r;
    g_tui.pt = panel_top();
    g_tui.pb = panel_bottom();
    backdrop();
    chrome((const tui_hint *)0, 0);
    box(g_tui.pt, l, g_tui.pb - g_tui.pt + 1u, r - l + 1u, BOX_SINGLE,
        A_BORDER, A_BODY);
    if (title != (const uint16_t *)0)
        box_title(g_tui.pt, l, r - l + 1u, title, A_PTITLE);
    g_tui.body_row = body_top();
    g_tui.body_col = 0;
    g_tui.step_open = 0;
    g_tui.step_shown = 0;
    g_tui.surface = SURF_PAGE;
    g_tui.style = TUI_PLAIN;
}

void tui_page(const uint16_t *title, const uint16_t *code)
{
    if (g_tui.loglen != 0)
        log_end();
    if (code != (const uint16_t *)0)
        g_tui.code = code;
    page_frame(title);
    if (title != (const uint16_t *)0)
        log_line(title, (const uint16_t *)0);
    tui_flush();
}

void tui_boot_page(const uint16_t *label, const uint16_t *code)
{
    static uint16_t title[STEP_MAX];
    static const uint16_t head[] = u"booting: ";
    size_t n = 0;
    size_t i;

    for (i = 0; head[i] != 0; i++)
        title[n++] = head[i];
    for (i = 0; label != (const uint16_t *)0 && label[i] != 0 &&
                n + 1u < STEP_MAX; i++)
        title[n++] = label[i];
    title[n] = 0;
    tui_page(title, code);
}

/* The body moves up a row to make room at its bottom; the open step's line
 * moves with it, or leaves the page. */
static void body_scroll(void)
{
    size_t top = body_top();
    size_t bot = body_bottom();
    size_t l = g_tui.pl + 1u;
    size_t w = g_tui.pr - g_tui.pl - 1u;
    size_t r;

    for (r = top; r < bot; r++) {
        memcpy(cell_at(r, l), cell_at(r + 1u, l), w * sizeof(tui_cell));
        mark(r);
    }
    fill(bot, l, 1, w, u' ', A_BODY);
    if (g_tui.step_shown) {
        if (g_tui.step_row == top)
            g_tui.step_shown = 0;
        else
            g_tui.step_row--;
    }
}

static void body_newline(void)
{
    g_tui.body_col = 0;
    if (g_tui.body_row < body_bottom())
        g_tui.body_row++;
    else
        body_scroll();
}

static void page_unit(uint16_t c)
{
    log_unit(c);
    if (c == u'\r')
        return;
    if (c == u'\n') {
        body_newline();
        return;
    }
    if (g_tui.body_col >= body_width())
        body_newline();
    put_n(g_tui.body_row, body_left() + g_tui.body_col, &c, 1,
          page_attr(g_tui.style));
    g_tui.body_col++;
}

/* Starts a fresh line unless the cursor is at the start of one. */
static void page_line_start(void)
{
    if (g_tui.body_col != 0)
        body_newline();
}

void page_close_step(const uint16_t *detail)
{
    size_t n;
    size_t w;
    uint16_t blank = u' ';

    if (!g_tui.step_open)
        return;
    g_tui.step_open = 0;
    if (!g_tui.step_shown)
        return;
    if (detail == (const uint16_t *)0)
        detail = k_ok;
    n = len16(detail);
    w = body_width();
    /* The step's text goes grey, and the status replaces the spinner. */
    put_n(g_tui.step_row, spin_col(), &blank, 1, A_BODY);
    put_n(g_tui.step_row, body_left(), g_tui.step,
          len16(g_tui.step) < w ? len16(g_tui.step) : w, A_BODY);
    if (n < status_end() - body_left())
        put_n(g_tui.step_row, status_end() + 1u - n, detail, n,
              detail == k_ok ? A_OK : A_BODY);
}

static void open_step(const uint16_t *what, size_t n)
{
    size_t w;
    size_t i;

    if (g_tui.surface != SURF_PAGE)
        tui_page((const uint16_t *)0, g_tui.code);
    page_close_step((const uint16_t *)0);
    page_line_start();
    for (i = 0; i < n && i + 1u < STEP_MAX; i++)
        g_tui.step[i] = what[i];
    g_tui.step[i] = 0;
    /* The text stops two columns short of the status column. */
    w = spin_col() - 2u - body_left();
    if (w > body_width())
        w = body_width();
    put_n(g_tui.body_row, body_left(), g_tui.step,
          len16(g_tui.step) < w ? len16(g_tui.step) : w, A_EMPH);
    g_tui.step_row = g_tui.body_row;
    g_tui.step_open = 1;
    g_tui.step_shown = 1;
    g_tui.spin = 0;
    if (g_tui.d->clock != 0)
        g_tui.spin_at = g_tui.d->clock(g_tui.d->ctx);
    {
        uint16_t s = tui_spin(0);

        put_n(g_tui.step_row, spin_col(), &s, 1, A_SPIN);
    }
    log_line(g_tui.step, u"...");
    body_newline();
    tui_flush();
}

void tui_step(const uint16_t *what)
{
    open_step(what, len16(what));
}

void tui_step_ascii(const char *what)
{
    uint16_t w[STEP_MAX];
    size_t n = 0;

    while (what != (const char *)0 && what[n] != '\0' && n + 1u < STEP_MAX) {
        w[n] = (uint16_t)(unsigned char)what[n];
        n++;
    }
    w[n] = 0;
    open_step(w, n);
}

void tui_step_done(const uint16_t *detail)
{
    if (g_tui.surface != SURF_PAGE)
        return;
    page_close_step(detail);
    tui_flush();
}

const uint16_t *tui_stage(void)
{
    return g_tui.step_open ? g_tui.step : (const uint16_t *)0;
}

void tui_note(const uint16_t *s)
{
    if (g_tui.surface != SURF_PAGE)
        tui_page((const uint16_t *)0, g_tui.code);
    page_line_start();
    g_tui.body_col = 2;
    g_tui.style = TUI_DIM;
    while (s != (const uint16_t *)0 && *s != 0)
        page_unit(*s++);
    log_end();
    g_tui.style = TUI_PLAIN;
    body_newline();
    tui_flush();
}

void tui_note_ascii(const char *s)
{
    uint16_t w[LOG_MAX];
    size_t n = 0;

    while (s != (const char *)0 && s[n] != '\0' && n + 1u < LOG_MAX) {
        w[n] = (uint16_t)(unsigned char)s[n];
        n++;
    }
    w[n] = 0;
    tui_note(w);
}

void tui_tick(void)
{
    const tui_display *d = g_tui.d;
    uint32_t now;
    uint16_t s;

    if (d == (const tui_display *)0 || d->clock == 0)
        return;
    if (g_tui.surface == SURF_BUSY) {
        now = d->clock(d->ctx);
        if (now == g_tui.spin_at)
            return;
        g_tui.spin_at = now;
        g_tui.spin++;
        tui_busy((const uint16_t *)0);
        return;
    }
    if (g_tui.surface != SURF_PAGE || !g_tui.step_open ||
        !g_tui.step_shown)
        return;
    now = d->clock(d->ctx);
    if (now == g_tui.spin_at)
        return;
    g_tui.spin_at = now;
    g_tui.spin++;
    s = tui_spin(g_tui.spin);
    put_n(g_tui.step_row, spin_col(), &s, 1, A_SPIN);
    tui_flush();
}

void tui_handoff(void)
{
    const tui_display *d = g_tui.d;
    size_t below = g_tui.pb + 1u;

    if (g_tui.surface == SURF_PAGE)
        page_close_step((const uint16_t *)0);
    if (g_tui.loglen != 0)
        log_end();
    if (g_tui.surface != SURF_PAGE)
        below = g_tui.rows - 4u;
    fill(below, 0, g_tui.rows - below, g_tui.cols, u' ', TUI_ATTR_TEXT);
    tui_flush();
    if (d->cursor != 0)
        d->cursor(d->ctx, 1, below, 0);
    g_tui.surface = SURF_GONE;
}

/* ---- the startup box ---- */

void tui_busy(const uint16_t *step)
{
    size_t w = 44u;
    size_t h;
    size_t row;
    size_t col;
    size_t i;

    if (g_tui.surface != SURF_BUSY) {
        g_tui.nbusy = 0;
        g_tui.surface = SURF_BUSY;
        g_tui.spin = 0;
    }
    if (step != (const uint16_t *)0) {
        if (g_tui.nbusy == BUSY_MAX) {
            memmove(g_tui.busy[0], g_tui.busy[1],
                    (BUSY_MAX - 1u) * sizeof g_tui.busy[0]);
            g_tui.nbusy--;
        }
        for (i = 0; step[i] != 0 && i + 1u < STEP_MAX; i++)
            g_tui.busy[g_tui.nbusy][i] = step[i];
        g_tui.busy[g_tui.nbusy][i] = 0;
        g_tui.nbusy++;
        log_line(step, u"...");
    }
    h = g_tui.nbusy + 4u;
    row = (g_tui.rows - h) / 2u;
    col = (g_tui.cols - w) / 2u;
    backdrop();
    chrome((const tui_hint *)0, 0);
    box(row, col, h, w, BOX_SINGLE, A_BORDER, A_BODY);
    box_title(row, col, w, u"starting", A_PTITLE);
    shadow(row, col, h, w);
    for (i = 0; i < g_tui.nbusy; i++) {
        size_t r = row + 2u + i;
        int last = i + 1u == g_tui.nbusy;

        put_s(r, col + 3u, g_tui.busy[i], last ? A_EMPH : A_BODY);
        if (last) {
            uint16_t s = tui_spin(g_tui.spin);

            put_n(r, col + w - 5u, &s, 1, A_SPIN);
        } else {
            put_s(r, col + w - 6u, k_ok, A_OK);
        }
    }
    tui_flush();
}

/* ---- the dialog ---- */

/* The dialog's text laid out: where each line starts in dtext, and how
 * long it is. */
static size_t g_line_at[DLG_LINES_MAX];
static size_t g_line_n[DLG_LINES_MAX];

static size_t layout(size_t tw)
{
    size_t lines = 0;
    size_t i = 0;

    while (i <= g_tui.dlen && lines < DLG_LINES_MAX) {
        size_t start = i;
        size_t end = i;
        size_t brk = (size_t)-1;

        /* The longest run that fits, broken at the last space in it. */
        while (end < g_tui.dlen && g_tui.dtext[end] != u'\n' &&
               end - start < tw) {
            if (g_tui.dtext[end] == u' ')
                brk = end;
            end++;
        }
        if (end < g_tui.dlen && g_tui.dtext[end] != u'\n' &&
            brk != (size_t)-1 && brk > start) {
            g_line_at[lines] = start;
            g_line_n[lines] = brk - start;
            lines++;
            i = brk + 1u;
            continue;
        }
        g_line_at[lines] = start;
        g_line_n[lines] = end - start;
        lines++;
        if (end >= g_tui.dlen)
            break;
        i = (g_tui.dtext[end] == u'\n') ? end + 1u : end;
    }
    /* A trailing line break leaves an empty last line; it is not drawn. */
    while (lines > 0 && g_line_n[lines - 1u] == 0)
        lines--;
    return lines;
}

void dialog_render(void)
{
    size_t l;
    size_t r;
    size_t aw;
    size_t w;
    size_t tw;
    size_t lines;
    size_t room;
    size_t h;
    size_t row;
    size_t col;
    size_t i;
    uint8_t a = dialog_attr(g_tui.dkind, TUI_PLAIN);

    /* What the dialog sits on: the last list, or the screen as it was. */
    memcpy(g_tui.cells, g_tui.keep,
           g_tui.cols * g_tui.rows * sizeof(tui_cell));
    mark_all();
    draw_chrome();

    area(&l, &r);
    aw = r - l + 1u;
    w = aw > 76u ? 68u : aw - 8u;
    tw = w - 6u;
    lines = layout(tw);
    room = g_tui.rows - 4u;           /* rows 2 .. rows-3 */
    /* Too long to show whole: the head, a line of "...", and the last two
     * lines, which are the blank and the key line under it. */
    if (lines + 4u > room && room > 9u) {
        size_t keep = room - 4u;
        size_t head = keep - 3u;

        g_line_at[head] = 0;
        g_line_n[head] = (size_t)-1;
        g_line_at[head + 1u] = g_line_at[lines - 2u];
        g_line_n[head + 1u] = g_line_n[lines - 2u];
        g_line_at[head + 2u] = g_line_at[lines - 1u];
        g_line_n[head + 2u] = g_line_n[lines - 1u];
        lines = keep;
    }
    h = lines + 4u;
    row = (g_tui.rows - h) / 2u;
    col = (g_tui.cols - w) / 2u;

    box(row, col, h, w, BOX_DOUBLE, a, a);
    box_title(row, col, w, g_tui.dtitle, dialog_attr(g_tui.dkind, TUI_EMPH));
    shadow(row, col, h, w);
    for (i = 0; i < lines; i++) {
        size_t k;
        size_t at = g_line_at[i];

        if (g_line_n[i] == (size_t)-1) {
            put_s(row + 2u + i, col + 3u, u"...",
                  dialog_attr(g_tui.dkind, TUI_DIM));
            continue;
        }
        for (k = 0; k < g_line_n[i]; k++)
            put_n(row + 2u + i, col + 3u + k, &g_tui.dtext[at + k], 1,
                  dialog_attr(g_tui.dkind, g_tui.dstyle[at + k]));
    }
}

static void dialog_unit(uint16_t c)
{
    log_unit(c);
    if (c == u'\r')
        return;
    if (g_tui.dlen < DLG_TEXT_MAX) {
        g_tui.dtext[g_tui.dlen] = c;
        g_tui.dstyle[g_tui.dlen] = (uint8_t)g_tui.style;
        g_tui.dlen++;
    }
}

void tui_report(int kind, const uint16_t *title, const uint16_t *label)
{
    size_t i;

    if (g_tui.loglen != 0)
        log_end();
    if (g_tui.surface == SURF_PAGE)
        page_close_step(kind == TUI_FAIL ? u"FAILED" : (const uint16_t *)0);
    if (g_tui.surface == SURF_GONE)
        tui_repaint();
    if (!g_tui.keep_valid) {
        memcpy(g_tui.keep, g_tui.cells,
               g_tui.cols * g_tui.rows * sizeof(tui_cell));
    }
    g_tui.dkind = kind;
    for (i = 0; title != (const uint16_t *)0 && title[i] != 0 &&
                i + 1u < STEP_MAX; i++)
        g_tui.dtitle[i] = title[i];
    g_tui.dtitle[i] = 0;
    g_tui.dlen = 0;
    g_tui.surface = SURF_DIALOG;
    g_tui.hints = k_report_hints;
    g_tui.nhints = sizeof k_report_hints / sizeof k_report_hints[0];
    if (title != (const uint16_t *)0)
        log_line(u"== ", title);
    g_tui.style = TUI_EMPH;
    if (label != (const uint16_t *)0) {
        tui_out(label);
        tui_out(u"\n\n");
    }
    g_tui.style = TUI_PLAIN;
}

void tui_report_failed(const uint16_t *what, const uint16_t *stage,
                       uint64_t status, int with_status)
{
    tui_style(TUI_PLAIN);
    tui_out(what);
    tui_out(u" failed while ");
    tui_out(stage != (const uint16_t *)0 ? stage : u"starting");
    if (with_status) {
        tui_out(u": ");
        tui_out_hex(status);
    } else {
        tui_out(u".");
    }
    tui_out(u"\n");
}

void tui_report_exited(const uint16_t *what, uint64_t status)
{
    tui_style(TUI_PLAIN);
    tui_out(what);
    tui_out(u" exited, status ");
    tui_out_hex(status);
    tui_out(u"\n");
}

void tui_report_advice(const uint16_t *advice)
{
    if (advice == (const uint16_t *)0)
        return;
    tui_style(TUI_PLAIN);
    tui_out(u"\n");
    tui_style(TUI_DIM);
    tui_out(advice);
    tui_out(u"\n");
    tui_style(TUI_PLAIN);
}

void tui_report_end(const uint16_t *keys)
{
    /* A dialog asking for its own keys has no "any key" in the bar. */
    if (keys != (const uint16_t *)0) {
        g_tui.hints = (const tui_hint *)0;
        g_tui.nhints = 0;
    }
    tui_style(TUI_PLAIN);
    tui_out(u"\n");
    tui_style(TUI_EMPH);
    tui_out(keys != (const uint16_t *)0 ? keys : tui_press_any);
    tui_out(u"\n");
    tui_style(TUI_PLAIN);
    dialog_render();
    tui_flush();
}

int tui_ask(const uint16_t *title, const uint16_t *text,
            const uint16_t *keys)
{
    int k;

    tui_report(TUI_INFO, title, (const uint16_t *)0);
    g_tui.hints = (const tui_hint *)0;
    g_tui.nhints = 0;
    tui_out(text);
    tui_out(u"\n\n");
    tui_style(TUI_EMPH);
    tui_out(keys);
    tui_out(u"\n");
    tui_style(TUI_PLAIN);
    dialog_render();
    k = tui_wait();
    if (k >= 'A' && k <= 'Z')
        k = k - 'A' + 'a';
    return k;
}

/* ---- text ---- */

void tui_out(const uint16_t *s)
{
    if (s == (const uint16_t *)0)
        return;
    if (g_tui.surface == SURF_DIALOG) {
        while (*s != 0)
            dialog_unit(*s++);
        return;
    }
    if (g_tui.surface == SURF_GONE) {
        /* The payload has the display; the transcript still hears it. */
        while (*s != 0)
            log_unit(*s++);
        return;
    }
    if (g_tui.surface != SURF_PAGE)
        tui_page((const uint16_t *)0, g_tui.code);
    while (*s != 0)
        page_unit(*s++);
    /* A page's text is seen as it is written: a route may write a line and
     * then take seconds before its next step. */
    tui_flush();
}

void tui_out_ascii(const char *s)
{
    uint16_t w[2] = { 0, 0 };

    if (s == (const char *)0)
        return;
    while (*s != '\0') {
        w[0] = (uint16_t)(unsigned char)*s++;
        tui_out(w);
    }
}

void tui_out_dec(uint64_t v)
{
    uint16_t buf[21];
    size_t i = sizeof buf / sizeof buf[0] - 1u;

    buf[i] = 0;
    do {
        buf[--i] = (uint16_t)(u'0' + (unsigned)(v % 10u));
        v /= 10u;
    } while (v != 0 && i > 0);
    tui_out(&buf[i]);
}

void tui_out_hex(uint64_t v)
{
    uint16_t buf[19];
    size_t i = sizeof buf / sizeof buf[0] - 1u;

    buf[i] = 0;
    do {
        unsigned d = (unsigned)(v & 0xFu);

        buf[--i] = (uint16_t)(d < 10u ? u'0' + d : u'a' + (d - 10u));
        v >>= 4;
    } while (v != 0 && i > 2u);
    buf[--i] = u'x';
    buf[--i] = u'0';
    tui_out(&buf[i]);
}

void tui_size(uint16_t *dst, size_t cap, uint64_t bytes)
{
    static const uint16_t *const unit[5] = {
        u"B", u"kB", u"MB", u"GB", u"TB",
    };
    const uint16_t *name;
    uint64_t whole = bytes;
    uint64_t tenth = 0;
    size_t u = 0;
    uint16_t d[24];
    size_t n = 0;
    size_t at = 0;

    if (cap == 0)
        return;
    while (whole >= 1000u && u < 4u) {
        tenth = (whole % 1000u) / 100u;
        whole /= 1000u;
        u++;
    }
    do {
        d[n++] = (uint16_t)(u'0' + (unsigned)(whole % 10u));
        whole /= 10u;
    } while (whole != 0 && n < 20u);
    while (n > 0 && at + 1u < cap)
        dst[at++] = d[--n];
    if (u != 0 && at < 3u && at + 3u < cap) {
        dst[at++] = u'.';
        dst[at++] = (uint16_t)(u'0' + (unsigned)tenth);
    }
    if (at + 1u < cap)
        dst[at++] = u' ';
    for (name = unit[u]; *name != 0 && at + 1u < cap; name++)
        dst[at++] = *name;
    dst[at] = 0;
}

/* ---- waiting ---- */

int tui_wait(void)
{
    const tui_display *d = g_tui.d;
    int k;

    tui_flush();
    if (d->wait != 0)
        d->wait(d->ctx, 1);
    if (d->touch != 0)
        d->touch(d->ctx);
    for (;;) {
        k = d->key(d->ctx, TUI_FOREVER);
        if (k == TUI_KEY_IDLE) {
            if (d->idle_off != 0)
                d->idle_off(d->ctx);
            tui_repaint();
            tui_flush();
            continue;
        }
        if (k != TUI_KEY_NONE)
            break;
    }
    if (d->wait != 0)
        d->wait(d->ctx, 0);
    return k;
}

void tui_pause(unsigned seconds)
{
    const tui_display *d = g_tui.d;
    uint32_t start = d->clock(d->ctx);
    uint32_t total = seconds * 10u;

    tui_flush();
    for (;;) {
        uint32_t gone = d->clock(d->ctx) - start;

        if (gone >= total)
            break;
        (void)d->key(d->ctx, total - gone);
    }
    while (d->key(d->ctx, 0) > 0)
        ;
}

void tui_power_off(void)
{
    const tui_display *d = g_tui.d;

    tui_flush();
    d->power_off(d->ctx);
    for (;;)
        ;
}
