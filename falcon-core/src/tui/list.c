/* falcon-core/src/tui: the lists. See tui.h.
 *
 * A list is a panel of rows, one highlighted, with the probe filling the
 * rows' tags in the background (DESIGN Section 7.8), a countdown on the
 * panel's bottom border, and a status box beside it. Only the rows that
 * changed are flushed: a move of the selection is two rows, a scroll the
 * window.
 */

#include <tui/internal.h>

#include <lib/string.h>

const uint16_t tui_list_empty[] = u"(no ISOs found on the exFAT partition)";
const uint16_t tui_tag_pending[] = u"...";
const uint16_t tui_validation_off[] =
    u"shim signature validation is DISABLED (MokSBState): every unsigned "
    u"EFI binary runs here";

/* A name this narrow is where aligned tag columns stop being worth it. */
#define NAME_MIN 12u

/* The status box's width, widened to its longest word up to SIDE_MAX, and
 * the gap between it and the panel. */
#define SIDE_W   23u
#define SIDE_MAX 40u
#define SIDE_GAP 3u

/* What one call draws, bundled because the painters are called from
 * several places. */
typedef struct {
    const tui_list *l;
    size_t pl, pr, pt, pb;       /* the panel */
    size_t sl, sr;               /* the status box, when there is one */
    size_t win;                  /* rows the window shows */
    size_t top;
    size_t sel;
    size_t sig_w;
    size_t why_w;
    int    busy;                 /* the probe has more to do */
    unsigned spin;
    unsigned left;               /* the countdown's seconds, 0 when off */
    tui_status shown;
    int    have_status;
} view;

static int pending(const view *v, size_t idx)
{
    return v->l->fill != 0 && !v->l->rows[idx].probed;
}

/* The tag columns, sized to the window on screen, a row not probed yet
 * counting as its placeholder. Nonzero when either changed, which repaints
 * the whole window. */
static int widths(view *v)
{
    size_t sig_w = 0;
    size_t why_w = 0;
    size_t i;
    int changed;

    for (i = v->top; i < v->top + v->win && i < v->l->count; i++) {
        size_t a;
        size_t b = 0;

        if (pending(v, i)) {
            a = len16(tui_tag_pending);
        } else {
            a = len16(v->l->rows[i].sig);
            b = len16(v->l->rows[i].why);
        }
        if (a > sig_w)
            sig_w = a;
        if (b > why_w)
            why_w = b;
    }
    changed = sig_w != v->sig_w || why_w != v->why_w;
    v->sig_w = sig_w;
    v->why_w = why_w;
    return changed;
}

/* One slot of the window: the row it shows, or blank past the end. */
static void paint_slot(const view *v, size_t slot)
{
    size_t idx = v->top + slot;
    size_t row = v->pt + 1u + slot;
    size_t bar_l = v->pl + 1u;
    size_t bar_w = v->pr - v->pl - 1u;
    size_t text_l = v->pl + 3u;
    size_t text_end = v->pr - 1u;          /* one blank before the border */
    const tui_row *w;
    const uint16_t *sig;
    const uint16_t *why;
    size_t sig_n;
    size_t why_n;
    size_t cols;
    size_t own;
    size_t name_end = text_end;
    size_t tag_at;
    int sel;
    uint8_t a_name;
    uint8_t a_sig;
    uint8_t a_why;

    if (idx >= v->l->count) {
        fill(row, bar_l, 1, bar_w, u' ', A_BODY);
        if (idx == 0)
            put_s(row, text_l, v->l->empty != (const uint16_t *)0
                                   ? v->l->empty : tui_list_empty, A_DIM);
        return;
    }
    w = &v->l->rows[idx];
    sel = idx == v->sel;
    if (pending(v, idx)) {
        sig = tui_tag_pending;
        why = (const uint16_t *)0;
    } else {
        sig = w->sig;
        why = w->why;
    }
    sig_n = len16(sig);
    why_n = len16(why);

    if (sel) {
        a_name = w->caution ? A_CAUTION_SEL : A_SEL;
        a_sig = a_name;
        a_why = a_name;
    } else if (w->caution) {
        a_name = A_CAUTION;
        a_sig = A_CAUTION;
        a_why = A_CAUTION;
    } else {
        a_name = A_BODY;
        a_sig = pending(v, idx) ? A_DIM : A_SIG;
        a_why = A_CAUTION;
    }
    fill(row, bar_l, 1, bar_w, u' ', a_name);
    if (sel) {
        uint16_t m = u'>';

        put_n(row, bar_l, &m, 1, a_name);
    }

    /* The tags in aligned columns when the name keeps NAME_MIN beside
     * them; each row's own tags at the right edge when it does not; the
     * name alone on a panel too narrow for that. The name is what gets
     * cut, never a tag. */
    cols = v->sig_w + (v->why_w != 0 ? 2u + v->why_w : 0u);
    own = sig_n + (why_n != 0 ? 2u + why_n : 0u);
    if (cols != 0 && text_end - text_l >= NAME_MIN + 2u + cols) {
        tag_at = text_end - cols;
        name_end = tag_at - 2u;
        put_n(row, tag_at, sig, sig_n, a_sig);
        if (why_n != 0)
            put_n(row, tag_at + v->sig_w + 2u, why, why_n, a_why);
    } else if (own != 0 && text_end - text_l >= 3u + own) {
        tag_at = text_end - own;
        name_end = tag_at - 2u;
        put_n(row, tag_at, sig, sig_n, a_sig);
        if (why_n != 0)
            put_n(row, tag_at + sig_n + 2u, why, why_n, a_why);
    }
    {
        size_t n = len16(w->label);

        if (n > name_end - text_l)
            n = name_end - text_l;
        put_n(row, text_l, w->label, n, a_name);
    }
}

/* The scroll bar is the panel's right border: the thumb in full blocks,
 * sized to the window's share of the list, and an arrow in the first or
 * last cell while there is more that way. */
static void paint_bar(const view *v)
{
    size_t h = v->win;
    size_t total = v->l->count;
    size_t th;
    size_t tp;
    size_t i;
    uint16_t line = TUI_VLINE;
    uint16_t blk = TUI_BLOCK;
    uint16_t up = TUI_UP;
    uint16_t dn = TUI_DOWN;

    for (i = 0; i < h; i++)
        put_n(v->pt + 1u + i, v->pr, &line, 1, A_BORDER);
    if (total <= h || h == 0)
        return;
    th = (h * h + total / 2u) / total;
    if (th < 1u)
        th = 1u;
    tp = ((h - th) * v->top + (total - h) / 2u) / (total - h);
    for (i = 0; i < th; i++)
        put_n(v->pt + 1u + tp + i, v->pr, &blk, 1, A_SPIN);
    if (v->top > 0)
        put_n(v->pt + 1u, v->pr, &up, 1, A_SPIN);
    if (v->top + h < total)
        put_n(v->pt + h, v->pr, &dn, 1, A_SPIN);
}

static size_t put_dec(uint16_t *dst, size_t at, size_t cap, size_t v)
{
    uint16_t d[20];
    size_t n = 0;

    do {
        d[n++] = (uint16_t)(u'0' + v % 10u);
        v /= 10u;
    } while (v != 0 && n < 20u);
    while (n > 0 && at + 1u < cap)
        dst[at++] = d[--n];
    dst[at] = 0;
    return at;
}

/* The bottom border: the countdown or the highlighted row's detail at its
 * left, the position at its right. */
static void paint_bottom(const view *v)
{
    uint16_t hline = TUI_HLINE;
    uint16_t buf[TUI_COLS_MAX];
    size_t end = v->pr - 2u;
    size_t i;

    for (i = v->pl + 1u; i < v->pr; i++)
        put_n(v->pb, i, &hline, 1, A_BORDER);
    if (v->l->count > v->win) {
        size_t n = put_dec(buf, 0, sizeof buf / sizeof buf[0], v->sel + 1u);

        if (n + 1u < sizeof buf / sizeof buf[0])
            buf[n++] = u'/';
        n = put_dec(buf, n, sizeof buf / sizeof buf[0], v->l->count);
        if (n + 4u < end - v->pl) {
            (void)border_text(v->pb, end - n - 2u, end, buf, A_BODY);
            end = end - n - 3u;
        }
    }
    if (v->left != 0) {
        static const uint16_t head[] = u"Booting in ";
        size_t n = 0;

        for (i = 0; head[i] != 0; i++)
            buf[n++] = head[i];
        n = put_dec(buf, n, sizeof buf / sizeof buf[0], v->left);
        buf[n++] = u' ';
        buf[n++] = u's';
        buf[n] = 0;
        (void)border_text(v->pb, v->pl + 2u, end, buf, A_COUNT);
        return;
    }
    if (v->l->detail != 0 && v->sel < v->l->count) {
        buf[0] = 0;
        v->l->detail(v->l->ctx, v->sel, buf, sizeof buf / sizeof buf[0]);
        buf[sizeof buf / sizeof buf[0] - 1u] = 0;
        (void)border_text(v->pb, v->pl + 2u, end, buf, A_BODY);
    }
}

/* The line under the panel. */
static void paint_note(const view *v)
{
    uint16_t buf[TUI_COLS_MAX];

    fill(v->pb + 1u, 0, 1, g_tui.cols, TUI_SHADE, A_BACKDROP);
    if (v->l->alert != (const uint16_t *)0) {
        (void)border_text(v->pb + 1u, v->pl + 2u, v->pr, v->l->alert,
                          TUI_ATTR_ALERT);
        return;
    }
    if (v->l->note == 0)
        return;
    buf[0] = 0;
    v->l->note(v->l->ctx, buf, sizeof buf / sizeof buf[0]);
    buf[sizeof buf / sizeof buf[0] - 1u] = 0;
    (void)border_text(v->pb + 1u, v->pl + 2u, v->pr, buf, A_BODY);
}

/* The spinner, on the top border's right end while the probe has work. */
static void paint_spin(const view *v)
{
    uint16_t s[3];

    s[0] = u' ';
    s[1] = tui_spin(v->spin);
    s[2] = u' ';
    if (v->busy) {
        put_n(v->pt, v->pr - 4u, s, 3, A_BORDER);
        put_n(v->pt, v->pr - 3u, &s[1], 1, A_SPIN);
    } else {
        uint16_t h = TUI_HLINE;

        put_n(v->pt, v->pr - 4u, &h, 1, A_BORDER);
        put_n(v->pt, v->pr - 3u, &h, 1, A_BORDER);
        put_n(v->pt, v->pr - 2u, &h, 1, A_BORDER);
    }
}

static void log_row(const view *v, size_t i);

int status_same(const tui_status *a, const tui_status *b)
{
    size_t i;
    size_t k;

    if (a->n != b->n)
        return 0;
    for (i = 0; i < a->n && i < TUI_STATUS_ROWS; i++) {
        if (a->attr[i] != b->attr[i])
            return 0;
        for (k = 0; k < TUI_STATUS_MAX; k++) {
            if (a->line[i][k] != b->line[i][k])
                return 0;
            if (a->line[i][k] == 0)
                break;
        }
    }
    return 1;
}

size_t wrap_next(const uint16_t *s, size_t at, size_t w, size_t *len)
{
    size_t end = at;
    size_t brk = (size_t)-1;

    while (s[end] != 0 && end - at < w) {
        if (s[end] == u' ')
            brk = end;
        end++;
    }
    if (s[end] == 0) {
        *len = end - at;
        return end;
    }
    if (s[end] == u' ') {
        *len = end - at;
        return end + 1u;
    }
    if (brk != (size_t)-1 && brk > at) {
        *len = brk - at;
        return brk + 1u;
    }
    *len = end - at;
    return end;
}

/* The status box: each line wrapped to the box, the box as tall as they
 * make it, at most to the panel's bottom. The box's old rows go back to
 * the backdrop first, since a shorter answer leaves a shorter box. */
static size_t g_status_h;

static void paint_status(view *v)
{
    size_t n = v->shown.n < TUI_STATUS_ROWS ? v->shown.n : TUI_STATUS_ROWS;
    size_t w = v->sr - v->sl + 1u;
    size_t tw = w - 4u;
    size_t rows = 0;
    size_t max = v->pb - v->pt + 1u - 4u;
    size_t i;

    for (i = 0; i < n; i++) {
        size_t at = 0;
        size_t len;

        do {
            at = wrap_next(v->shown.line[i], at, tw, &len);
            rows++;
        } while (v->shown.line[i][at] != 0);
    }
    if (rows > max)
        rows = max;
    if (g_status_h != 0)
        fill(v->pt, v->sl, g_status_h, w, TUI_SHADE, A_BACKDROP);
    g_status_h = rows + 4u;
    box(v->pt, v->sl, g_status_h, w, BOX_SINGLE, A_BORDER, A_BODY);
    if (v->l->status_title != (const uint16_t *)0)
        box_title(v->pt, v->sl, w, v->l->status_title, A_PTITLE);
    rows = 0;
    for (i = 0; i < n; i++) {
        size_t at = 0;
        size_t len;

        do {
            size_t next = wrap_next(v->shown.line[i], at, tw, &len);

            if (rows < max)
                put_n(v->pt + 2u + rows, v->sl + 2u, &v->shown.line[i][at],
                      len, v->shown.attr[i]);
            rows++;
            at = next;
        } while (v->shown.line[i][at] != 0);
    }
}

/* Asks for the status lines and redraws the box when they changed (always
 * when `all`). */
static void refresh_status(view *v, int all)
{
    tui_status now;
    size_t i;

    if (!v->have_status)
        return;
    memset(&now, 0, sizeof now);
    v->l->status(v->l->ctx, &now);
    for (i = 0; i < TUI_STATUS_ROWS; i++)
        now.line[i][TUI_STATUS_MAX - 1u] = 0;
    if (!all && status_same(&now, &v->shown))
        return;
    v->shown = now;
    /* The transcript hears each new state of the box: a line wrapped in
     * the box is whole there. */
    for (i = 0; i < now.n && i < TUI_STATUS_ROWS; i++)
        log_line(now.line[i], (const uint16_t *)0);
    paint_status(v);
}

/* Every pending row in the window takes what the probe knows. Nonzero
 * when any changed; *full when a tag column widened. */
static int fill_window(view *v, int *full)
{
    int any = 0;
    size_t i;

    if (v->l->fill == 0)
        return 0;
    for (i = v->top; i < v->top + v->win && i < v->l->count; i++) {
        if (!pending(v, i))
            continue;
        if (v->l->fill(v->l->ctx, i)) {
            v->l->rows[i].probed = 1;
            any = 1;
            log_row(v, i);
        }
    }
    if (any && widths(v))
        *full = 1;
    return any;
}

static int is_commit(const char *keys, int c)
{
    if (keys == (const char *)0 || c <= 0 || c > 0x7E)
        return 0;
    if (c >= 'A' && c <= 'Z')
        c = c - 'A' + 'a';
    for (; *keys != '\0'; keys++) {
        if (*keys == c)
            return 1;
    }
    return 0;
}

/* The status box's width: SIDE_W, or wide enough for the longest word its
 * lines start with (an enrollment's label has no space to wrap at), at most
 * SIDE_MAX. Later lines wrap at their spaces. */
static size_t status_width(const tui_list *l)
{
    static tui_status s;
    size_t w = SIDE_W;
    size_t i;

    memset(&s, 0, sizeof s);
    l->status(l->ctx, &s);
    for (i = 0; i < s.n && i < TUI_STATUS_ROWS; i++) {
        size_t k;
        size_t run = 0;

        for (k = 0; k < TUI_STATUS_MAX && s.line[i][k] != 0; k++) {
            run = s.line[i][k] == u' ' ? 0 : run + 1u;
            if (run + 4u > w)
                w = run + 4u;
        }
    }
    return w < SIDE_MAX ? w : SIDE_MAX;
}

static void layout(view *v, const tui_list *l)
{
    size_t al;
    size_t ar;

    memset(v, 0, sizeof *v);
    v->l = l;
    area(&al, &ar);
    v->pl = al;
    v->pr = ar;
    v->pt = panel_top();
    v->pb = panel_bottom();
    v->have_status = l->status != 0;
    if (v->have_status && ar - al + 1u >= 60u) {
        v->sr = ar;
        v->sl = ar + 1u - status_width(l);
        v->pr = v->sl - SIDE_GAP - 1u;
    } else {
        v->have_status = 0;
    }
    v->win = v->pb - v->pt - 1u;
    v->sel = l->sel < l->count ? l->sel : 0;
    v->busy = l->step != 0;
    v->left = l->count != 0 ? l->seconds : 0;
    g_status_h = 0;
}

/* The hint bar: the list's own, or while the countdown runs, the same with
 * its last pair (escape) giving way to "any key stops the count". */
#define HINTS_MAX 8u
static tui_hint g_hints[HINTS_MAX];

static void set_hints(const view *v)
{
    size_t n = v->l->nhints < HINTS_MAX ? v->l->nhints : HINTS_MAX;
    size_t i;

    for (i = 0; i < n; i++)
        g_hints[i] = v->l->hints[i];
    if (v->left != 0) {
        if (n == HINTS_MAX || (n != 0 && n == v->l->nhints))
            n--;
        g_hints[n].key = u"any key";
        g_hints[n].word = u"stops the count";
        n++;
    }
    chrome(g_hints, n);
}

/* The hint bar as the transcript hears it. */
static void log_hints(void)
{
    uint16_t line[LOG_MAX];
    size_t n = 0;
    size_t i;

    for (i = 0; i < g_tui.nhints; i++) {
        const uint16_t *k = g_tui.hints[i].key;
        const uint16_t *w = g_tui.hints[i].word;

        if (i != 0 && n + 3u < LOG_MAX) {
            line[n++] = u' ';
            line[n++] = u' ';
            line[n++] = u' ';
        }
        while (*k != 0 && n + 1u < LOG_MAX)
            line[n++] = *k++;
        if (n + 1u < LOG_MAX)
            line[n++] = u' ';
        while (*w != 0 && n + 1u < LOG_MAX)
            line[n++] = *w++;
    }
    line[n] = 0;
    if (n != 0)
        log_line(line, (const uint16_t *)0);
}

/* One row as the transcript hears it: '>' when selected, the label, then
 * its tags, two spaces apart. */
static void log_row(const view *v, size_t i)
{
    uint16_t line[LOG_MAX];
    const tui_row *w = &v->l->rows[i];
    const uint16_t *parts[3];
    size_t n = 0;
    size_t p;

    parts[0] = w->label;
    parts[1] = pending(v, i) ? tui_tag_pending : w->sig;
    parts[2] = pending(v, i) ? (const uint16_t *)0 : w->why;
    line[n++] = i == v->sel ? u'>' : u' ';
    line[n++] = u' ';
    for (p = 0; p < 3u; p++) {
        const uint16_t *s = parts[p];

        if (s == (const uint16_t *)0 || s[0] == 0)
            continue;
        if (p != 0 && n + 2u < LOG_MAX) {
            line[n++] = u' ';
            line[n++] = u' ';
        }
        while (*s != 0 && n + 1u < LOG_MAX)
            line[n++] = *s++;
    }
    line[n] = 0;
    log_line(line, (const uint16_t *)0);
}

/* "Booting in <n> s" as the transcript hears it, each second. */
static void log_count(const view *v)
{
    uint16_t line[32];
    size_t n = 0;
    size_t i;

    for (i = 0; u"Booting in "[i] != 0; i++)
        line[n++] = u"Booting in "[i];
    n = put_dec(line, n, sizeof line / sizeof line[0], v->left);
    line[n++] = u' ';
    line[n++] = u's';
    line[n] = 0;
    log_line(line, (const uint16_t *)0);
}

/* The list as the transcript hears it when it opens: its title, the rows
 * the window shows with their tags, and the key hints as the bar draws
 * them, each a line of its own; a script reads a menu off serial by these
 * rather than out of the screen's colour runs. */
static void log_list(const view *v)
{
    size_t i;

    log_line(u"menu: ", v->l->title);
    for (i = v->top; i < v->top + v->win && i < v->l->count; i++)
        log_row(v, i);
    if (v->l->count == 0)
        log_line(u"  ", v->l->empty != (const uint16_t *)0 ? v->l->empty
                                                       : tui_list_empty);
    log_hints();
    if (v->left != 0)
        log_count(v);
}

static void paint_all(view *v)
{
    size_t i;

    backdrop();
    set_hints(v);
    box(v->pt, v->pl, v->pb - v->pt + 1u, v->pr - v->pl + 1u, BOX_SINGLE,
        A_BORDER, A_BODY);
    if (v->l->title != (const uint16_t *)0)
        box_title(v->pt, v->pl, v->pr - v->pl + 1u, v->l->title, A_PTITLE);
    (void)widths(v);
    for (i = 0; i < v->win; i++)
        paint_slot(v, i);
    paint_bar(v);
    paint_bottom(v);
    paint_note(v);
    paint_spin(v);
    if (v->have_status)
        refresh_status(v, 1);
}

/* Keeps the selection inside the window. Nonzero when the window moved. */
static int scroll(view *v)
{
    if (v->sel < v->top) {
        v->top = v->sel;
        return 1;
    }
    if (v->win != 0 && v->sel >= v->top + v->win) {
        v->top = v->sel - v->win + 1u;
        return 1;
    }
    return 0;
}

static void finish(view *v)
{
    const tui_display *d = g_tui.d;

    tui_flush();
    memcpy(g_tui.keep, g_tui.cells,
           g_tui.cols * g_tui.rows * sizeof(tui_cell));
    g_tui.keep_valid = 1;
    if (d->wait != 0)
        d->wait(d->ctx, 0);
    (void)v;
}

int tui_list_run(const tui_list *l, int *which)
{
    const tui_display *d = g_tui.d;
    static view v;
    size_t painted;
    uint32_t sec_at;
    uint32_t spin_at;
    int full = 0;

    if (g_tui.loglen != 0)
        log_end();
    layout(&v, l);
    g_tui.surface = SURF_LIST;
    g_tui.code = (const uint16_t *)0;
    if (d->cursor != 0)
        d->cursor(d->ctx, 0, 0, 0);
    (void)scroll(&v);
    paint_all(&v);
    log_list(&v);
    painted = v.sel;
    tui_flush();
    if (d->wait != 0)
        d->wait(d->ctx, 1);
    if (d->touch != 0)
        d->touch(d->ctx);
    sec_at = d->clock(d->ctx);
    spin_at = sec_at;

    for (;;) {
        uint32_t now;
        uint32_t wait;
        int k;
        size_t i;

        /* What moved since the last pass, then the probe's finds. */
        if (scroll(&v))
            full = 1;
        if (fill_window(&v, &full) && !full) {
            for (i = 0; i < v.win; i++)
                paint_slot(&v, i);
            paint_note(&v);
        }
        if (full) {
            (void)widths(&v);
            for (i = 0; i < v.win; i++)
                paint_slot(&v, i);
            paint_bar(&v);
            paint_note(&v);
            full = 0;
        } else if (painted != v.sel) {
            if (painted >= v.top && painted < v.top + v.win)
                paint_slot(&v, painted - v.top);
            paint_slot(&v, v.sel - v.top);
        }
        if (painted != v.sel)
            paint_bottom(&v);
        painted = v.sel;

        /* Each second: the status lines, and the countdown. */
        now = d->clock(d->ctx);
        if (now - sec_at >= 10u) {
            sec_at += 10u * ((now - sec_at) / 10u);
            refresh_status(&v, 0);
            if (v.left != 0) {
                if (d->touch != 0)
                    d->touch(d->ctx);
                if (--v.left == 0) {
                    if (which != (int *)0)
                        *which = TUI_KEY_ENTER;
                    finish(&v);
                    return (int)v.sel;
                }
                paint_bottom(&v);
                log_count(&v);
            }
        }
        tui_flush();

        /* The probe, a slice at a time whenever no key is waiting. */
        if (v.busy) {
            if (now != spin_at) {
                spin_at = now;
                v.spin++;
                paint_spin(&v);
            }
            if (!l->step(l->ctx, v.sel)) {
                v.busy = 0;
                paint_spin(&v);
                paint_note(&v);
            }
            wait = 0;
        } else if (v.left != 0 || v.have_status) {
            wait = 10u - (d->clock(d->ctx) - sec_at) % 10u;
        } else {
            wait = TUI_FOREVER;
        }

        k = d->key(d->ctx, wait);
        if (k == TUI_KEY_NONE)
            continue;
        if (k == TUI_KEY_GONE) {
            finish(&v);
            return -1;
        }
        if (k == TUI_KEY_IDLE) {
            if (d->idle_off != 0)
                d->idle_off(d->ctx);
            tui_repaint();
            continue;
        }

        /* Any key stops the countdown for good, and the hints go back to
         * the list's own. */
        if (v.left != 0) {
            v.left = 0;
            paint_bottom(&v);
            set_hints(&v);
            log_hints();
        }

        /* Drain the queue, then paint once: a held arrow arrives as a run
         * of keys, and one repaint each would make the repaints the
         * backlog. */
        while (k != TUI_KEY_NONE) {
            if (k == TUI_KEY_ESC || k == TUI_KEY_GONE) {
                finish(&v);
                return -1;
            }
            if (k == TUI_KEY_IDLE)
                break;
            if (l->count != 0) {
                if (k == TUI_KEY_ENTER || is_commit(l->keys, k)) {
                    if (which != (int *)0)
                        *which = (k >= 'A' && k <= 'Z') ? k - 'A' + 'a' : k;
                    finish(&v);
                    return (int)v.sel;
                }
                if (k == TUI_KEY_UP && v.sel > 0)
                    v.sel--;
                else if (k == TUI_KEY_DOWN && v.sel + 1u < l->count)
                    v.sel++;
                else if (k == TUI_KEY_PGUP)
                    v.sel = v.sel > v.win ? v.sel - v.win : 0;
                else if (k == TUI_KEY_PGDN)
                    v.sel = v.sel + v.win < l->count ? v.sel + v.win
                                                     : l->count - 1u;
                else if (k == TUI_KEY_HOME)
                    v.sel = 0;
                else if (k == TUI_KEY_END)
                    v.sel = l->count - 1u;
            }
            k = d->key(d->ctx, 0);
        }
        if (scroll(&v)) {
            full = 1;
        }
    }
}
