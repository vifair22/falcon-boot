#include <console/menu.h>
#include <console/idle.h>

#include <lib/string.h>
#include <lib/ucs2.h>
#include <tui/tui.h>

/* The frame.
 *
 * The console font is a fixed 8x19 cell in every mode, so a smaller mode does
 * not enlarge the text; it makes the firmware's own console driver centre a
 * smaller grid and pad the rest itself. Under OVMF at 1280x800 that padding
 * is 320 pixels a side at 80x25 and 240 at 100x31, which is why menu_init
 * takes the largest mode: only on that grid is the surrounding black ours to
 * place.
 *
 * The block itself - where it sits, how wide it is, where a menu's banner,
 * list and hint go - is falcon-core/src/tui, which falcon-bios draws from
 * too, so the two loaders' screens are the same cell for cell. What stays
 * here is getting those cells onto a Simple Text Output console.
 *
 * ClearScreen blanks everything on entry and nothing writes outside the block
 * afterwards, so the surround stays dark. Redraws are cursor-repositioned
 * overwrites, not ClearScreen, to avoid flicker on slow console paths. */

/* Every screen row is composed here and emitted in one OutputString.
 *
 * Emitting per character instead costs a call per column: ~1650 to repaint a
 * 21-row window at 80x25, each one a ConSplitter dispatch to every registered
 * console plus, on the GOP-backed one, an HII glyph lookup and a single-cell
 * blt into a write-combining framebuffer. */
static CHAR16 g_row[TUI_ROW_MAX];

/* The content block, fixed for the run.
 *
 * It used to live in menu_run's locals, which meant the menu was the only
 * screen that had one: the passphrase prompt, every confirmation and every
 * error drew from column zero while the menu sat in a centred block, and on
 * a wide console they did not look like the same program. The frame is
 * module state now and menu_screen/menu_out draw inside it, so a caller gets
 * the layout by writing text rather than by knowing about it. */
typedef struct {
    tui_frame f;
    UINTN col;          /* cursor column within the text area, for wrapping */
    BOOLEAN ready;
} menu_frame;

static menu_frame g_frame;

/* The route code menu_mode set, or null. A string literal: callers pass
 * constants, and holding the pointer is what lets menu_screen put the code
 * back after a module clears the screen under it. */
static const CHAR16 *g_mode;

/* The console state and the resolved layout. Bundled because paint_slot is
 * called from two places and the alternative is a nine-argument function. */
typedef struct {
    EFI_SYSTEM_TABLE *st;
    const menu_entry *entries;
    UINTN             count;
    tui_menu          m;
    UINTN             sig_w;     /* widest sig tag in the window on screen */
    UINTN             why_w;     /* widest why tag in the window on screen */
    /* The same rows, writable, and what fills their tags, for
     * menu_run_probed; NULL for every other caller. */
    menu_entry       *rows;
    const menu_probe *probe;
    /* How far the probe has got through the window on screen: the key line
     * says "checking done+1 of total" while a row in it is unprobed. */
    UINTN             pend_top;
    UINTN             pend_total;
    UINTN             pend_done;
    /* The status lines under the banner, and what they were last drawn
     * as, for menu_run_timed; status NULL for every other caller. */
    const menu_status *status;
    menu_status_text   shown;
} menu_view;

static void out(EFI_SYSTEM_TABLE *st, CHAR16 *s)
{
    st->ConOut->OutputString(st->ConOut, s);
}

/* Put the composed row at (left, row). One OutputString. The block never
 * reaches the final column, so writing its last cell cannot auto-wrap. */
static void emit_row(EFI_SYSTEM_TABLE *st, UINTN row, UINTN left)
{
    st->ConOut->SetCursorPosition(st->ConOut, left, row);
    out(st, g_row);
}

static void draw_row(EFI_SYSTEM_TABLE *st, UINTN row, UINTN left, UINTN w,
                     const CHAR16 *s)
{
    tui_row_text(g_row, w, s);
    emit_row(st, row, left);
}

/* A row the probe has not reached yet: its tags are not known, so it shows
 * the placeholder in the first tag column instead of what the row holds. */
static BOOLEAN pending(const menu_view *v, UINTN idx)
{
    return v->rows != (menu_entry *)0 && v->probe != (const menu_probe *)0 &&
           v->probe->fn != (menu_probe_fn)0 && !v->rows[idx].probed;
}

static void draw_entry(const menu_view *v, UINTN row, UINTN idx)
{
    const menu_entry *e = &v->entries[idx];
    BOOLEAN p = pending(v, idx);

    tui_row_entry(g_row, v->m.list_width, e->label,
                  p ? tui_tag_pending : e->sig, p ? (const CHAR16 *)L"" : e->why,
                  v->sig_w, v->why_w);
    emit_row(v->st, row, v->m.list_left);
}

/* Title in the menu's own colour, state suffix in whatever attribute the
 * caller chose for it. Three calls rather than one, because SetAttribute
 * applies to subsequent output and the two segments differ; the banner is
 * painted once per screen, so the split costs nothing that matters. */
static void draw_banner(const menu_view *v, const menu_banner *banner)
{
    EFI_SYSTEM_TABLE *st = v->st;
    UINTN at = 0;
    UINTN end = 0;
    CHAR16 keep;
    size_t i;

    st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_TITLE);
    for (i = 0; i < v->m.logo_rows; i++) {
        st->ConOut->SetCursorPosition(st->ConOut, v->m.left,
                                      v->m.logo_row + i);
        tui_row_text(g_row, v->m.width, tui_logo[i]);
        out(st, g_row);
    }
    st->ConOut->SetCursorPosition(st->ConOut, v->m.left, v->m.banner_row);
    if (banner == (void *)0) {
        tui_row_text(g_row, v->m.width, (const CHAR16 *)0);
        out(st, g_row);
        return;
    }

    tui_row_banner(g_row, v->m.width, banner->title, banner->state, &at,
                   &end);
    /* The cursor is left where each run ends, so emitting the next run
     * alone continues the row rather than restarting it. */
    keep = g_row[at];
    g_row[at] = 0;
    out(st, g_row);
    g_row[at] = keep;
    if (end > at) {
        keep = g_row[end];
        g_row[end] = 0;
        st->ConOut->SetAttribute(st->ConOut, banner->state_attr);
        out(st, g_row + at);
        st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_TITLE);
        g_row[end] = keep;
    }
    out(st, g_row + end);
}

/* The console's size, or 0x0 when the firmware will not say, which
 * tui_frame_init takes as the spec-guaranteed 80x25. */
static void screen_dims(EFI_SYSTEM_TABLE *st, UINTN *cols, UINTN *rows)
{
    if (st->ConOut->QueryMode(st->ConOut,
                              (UINTN)st->ConOut->Mode->Mode,
                              cols, rows) != EFI_SUCCESS) {
        *cols = 0;
        *rows = 0;
    }
}

/* Fix the frame from the console size. Idempotent, and safe to call from any
 * screen: menu_init sets it at startup, and anything that draws before that
 * gets it on first use rather than drawing at column zero. */
static void frame_ensure(EFI_SYSTEM_TABLE *st)
{
    UINTN cols;
    UINTN rows;

    if (g_frame.ready) {
        return;
    }
    screen_dims(st, &cols, &rows);
    tui_frame_init(&g_frame.f, cols, rows);
    g_frame.col = 0;
    g_frame.ready = TRUE;
}

/* The route code, where tui_mode_at puts it.
 *
 * It is drawn around the cursor rather than through it: a caller part way
 * through a line of menu_out, or a boot path that falls through to the next
 * one and redraws the code, must find the cursor where it left it. Every code
 * is the same width, so a redraw overwrites the last one completely. */
static void mode_draw(EFI_SYSTEM_TABLE *st)
{
    UINTN col = (UINTN)st->ConOut->Mode->CursorColumn;
    UINTN row = (UINTN)st->ConOut->Mode->CursorRow;
    CHAR16 code[TUI_MODE_CODE_MAX + 1u];
    UINTN at_row;
    UINTN at_col;
    UINTN len;

    if (g_mode == (const CHAR16 *)0) {
        return;
    }
    len = ucs2_copy(code, TUI_MODE_CODE_MAX + 1u, g_mode);
    if (!tui_mode_at(&g_frame.f, len, &at_row, &at_col)) {
        return;
    }
    st->ConOut->SetCursorPosition(st->ConOut, at_col, at_row);
    st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_MODE);
    out(st, code);
    st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_TEXT);
    st->ConOut->SetCursorPosition(st->ConOut, col, row);
}

/* Start a screen: clear it and park the cursor at the block's first text
 * column, on tui_page_row. Everything written with menu_out lands inside the
 * block from there. */
void menu_screen(EFI_SYSTEM_TABLE *st)
{
    if (st == (void *)0) {
        return;
    }
    frame_ensure(st);
    st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_TEXT);
    st->ConOut->ClearScreen(st->ConOut);
    st->ConOut->SetCursorPosition(st->ConOut, g_frame.f.text_col,
                                  tui_page_row(&g_frame.f));
    g_frame.col = 0;
    mode_draw(st);
}

/* One line at tui_page_row, overwritten by the next: the screen is whatever
 * menu_screen left, and nothing else on it moves. */
void menu_busy(EFI_SYSTEM_TABLE *st, const CHAR16 *step)
{
    CHAR16 text[TUI_ROW_MAX + 1u];

    if (st == (void *)0) {
        return;
    }
    frame_ensure(st);
    tui_busy_text(text, sizeof text / sizeof text[0], step);
    st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_TEXT);
    draw_row(st, tui_page_row(&g_frame.f), g_frame.f.left, g_frame.f.width,
             text);
}

void menu_mode(EFI_SYSTEM_TABLE *st, const CHAR16 *code)
{
    g_mode = code;
    if (st != (void *)0 && code != (const CHAR16 *)0) {
        frame_ensure(st);
        mode_draw(st);
    }
}

/* Write text inside the block, re-indenting after every line break and
 * wrapping at the block's right edge.
 *
 * Wrapping is by character rather than by word. menu_out is called with
 * fragments - a label, then a number, then the rest of a sentence - so a word
 * can span several calls, and buffering one to break cleanly would mean a
 * flush that a caller could forget and a message that could lose its last
 * word. Clipping instead would silently truncate an error. At this block
 * width the strings in this loader do not reach the margin anyway; the wrap
 * is there so that one day a longer one stays readable rather than
 * disappearing. */
void menu_out(EFI_SYSTEM_TABLE *st, const CHAR16 *s)
{
    CHAR16 chunk[2] = { L'\0', L'\0' };
    UINTN i = 0;

    if (st == (void *)0 || s == (void *)0) {
        return;
    }
    frame_ensure(st);

    while (s[i] != L'\0') {
        CHAR16 c = s[i];

        if (c == L'\r') {
            i++;                    /* the \n that follows does the work */
            continue;
        }
        if (c == L'\n') {
            out(st, L"\r\n");
            st->ConOut->SetCursorPosition(st->ConOut, g_frame.f.text_col,
                                          (UINTN)st->ConOut->Mode->CursorRow);
            g_frame.col = 0;
            i++;
            continue;
        }
        if (g_frame.f.text_width != 0 && g_frame.col >= g_frame.f.text_width) {
            out(st, L"\r\n");
            st->ConOut->SetCursorPosition(st->ConOut, g_frame.f.text_col,
                                          (UINTN)st->ConOut->Mode->CursorRow);
            g_frame.col = 0;
        }
        chunk[0] = c;
        out(st, chunk);
        g_frame.col++;
        i++;
    }
}

/* Sanity rails only. Nothing real reaches these; they exist so a firmware
 * reporting nonsense cannot hand the layout a grid it will subtract from. */
#define MODE_COLS_MAX 400u
#define MODE_ROWS_MAX 200u

void menu_init(EFI_SYSTEM_TABLE *st)
{
    INT32 best = -1;
    INT32 i;
    UINTN best_cols = 0;
    UINTN best_rows = 0;

    if (st == (void *)0 || st->ConOut == (void *)0 ||
        st->ConOut->Mode == (void *)0)
        return;

    /* Mode 0 is 80x25 by spec and mode 1 is 80x50 when present; 2 and up are
     * OEM-defined and are where the useful ones live. Largest wins, rows
     * first, because the block is a list and columns past TUI_WIDTH_MAX become
     * gutter either way. The frame comment has why largest and not merely
     * larger.
     *
     * Every step may fail: QueryMode returns EFI_UNSUPPORTED for modes below
     * MaxMode on firmware that miscounts, and SetMode can refuse a mode it
     * just described. The console then keeps what it had. */
    for (i = 0; i < st->ConOut->Mode->MaxMode; i++) {
        UINTN c = 0;
        UINTN r = 0;

        if (st->ConOut->QueryMode(st->ConOut, (UINTN)i, &c, &r) != EFI_SUCCESS)
            continue;
        if (c < 80u || r < 25u || c > MODE_COLS_MAX || r > MODE_ROWS_MAX)
            continue;
        if (r > best_rows || (r == best_rows && c > best_cols)) {
            best = i;
            best_cols = c;
            best_rows = r;
        }
    }

    if (best >= 0 && best != st->ConOut->Mode->Mode)
        st->ConOut->SetMode(st->ConOut, (UINTN)best);

    /* After the mode, never before: the frame is derived from the console
     * size, and SetMode is what changes it. */
    g_frame.ready = FALSE;
    frame_ensure(st);
}

/* Paint one slot of the list window. slot is 0-based within the window; the
 * entry it shows is top + slot, which may be past the end. */
static void paint_slot(const menu_view *v, UINTN slot, UINTN top, UINTN sel)
{
    UINTN idx = top + slot;

    if (idx < v->count) {
        const menu_entry *e = &v->entries[idx];

        v->st->ConOut->SetAttribute(v->st->ConOut,
                                    tui_entry_attr(e->caution, idx == sel));
        draw_entry(v, v->m.list_top + slot, idx);
    } else {
        v->st->ConOut->SetAttribute(v->st->ConOut, TUI_ATTR_TEXT);
        draw_row(v->st, v->m.list_top + slot, v->m.list_left,
                 v->m.list_width,
                 (idx == 0) ? tui_list_empty : (const CHAR16 *)0);
    }
}

static void restore_console(EFI_SYSTEM_TABLE *st)
{
    st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_TEXT);
    st->ConOut->EnableCursor(st->ConOut, TRUE);
}

INTN menu_run(EFI_SYSTEM_TABLE *st, const menu_banner *banner,
              const menu_entry *entries, UINTN count)
{
    return menu_run_keys(st, banner, entries, count, (const menu_keys *)0,
                         (CHAR16 *)0);
}

/* Is c one of the extra commit keys, either case? */
static BOOLEAN is_commit_key(const menu_keys *keys, CHAR16 c)
{
    const CHAR16 *k;

    if (keys == (const menu_keys *)0 || keys->keys == (const CHAR16 *)0)
        return FALSE;
    if (c >= L'A' && c <= L'Z')
        c = (CHAR16)(c - L'A' + L'a');
    for (k = keys->keys; *k != 0; k++) {
        if (*k == c)
            return TRUE;
    }
    return FALSE;
}

/* ---- the countdown (DESIGN Section 13.7) ----
 *
 * One periodic timer at a second, waited on beside the keyboard. The key
 * line carries the count while it runs, in falcon-bios's words
 * (screen_menu_timed), and goes back to the ordinary hint when a key stops
 * it. */
#define TICK_100NS 10000000u   /* one second in SetTimer's unit */

static void paint_countdown(const menu_view *v, UINTN left)
{
    static const CHAR16 head[] = L"Booting in ";
    static const CHAR16 tail[] = L" s. Any key stops the countdown.";
    CHAR16 text[80];
    CHAR16 digits[6];
    UINTN n = 0;
    UINTN d = 0;
    UINTN i;

    do {
        digits[d++] = (CHAR16)(L'0' + left % 10u);
        left /= 10u;
    } while (left != 0 && d < sizeof digits / sizeof digits[0]);
    for (i = 0; head[i] != 0; i++)
        text[n++] = head[i];
    while (d > 0)
        text[n++] = digits[--d];
    for (i = 0; tail[i] != 0 && n + 1u < sizeof text / sizeof text[0]; i++)
        text[n++] = tail[i];
    text[n] = 0;
    v->st->ConOut->SetAttribute(v->st->ConOut, TUI_ATTR_TEXT);
    draw_row(v->st, v->m.hint_row, v->m.left, v->m.width, text);
}

static EFI_EVENT countdown_start(EFI_SYSTEM_TABLE *st)
{
    EFI_EVENT ev = (EFI_EVENT)0;

    if (st->BootServices->CreateEvent(EVT_TIMER, 0, (EFI_EVENT_NOTIFY)0,
                                      (void *)0, &ev) != EFI_SUCCESS)
        return (EFI_EVENT)0;
    if (st->BootServices->SetTimer(ev, TimerPeriodic, TICK_100NS) !=
        EFI_SUCCESS) {
        st->BootServices->CloseEvent(ev);
        return (EFI_EVENT)0;
    }
    return ev;
}

static void countdown_stop(EFI_SYSTEM_TABLE *st, EFI_EVENT *ev)
{
    if (*ev == (EFI_EVENT)0)
        return;
    st->BootServices->SetTimer(*ev, TimerCancel, 0);
    st->BootServices->CloseEvent(*ev);
    *ev = (EFI_EVENT)0;
}

/* ---- the status lines ---- */

static BOOLEAN status_same(const CHAR16 *a, const CHAR16 *b)
{
    UINTN i;

    for (i = 0; i < MENU_STATUS_MAX; i++) {
        if (a[i] != b[i])
            return FALSE;
        if (a[i] == 0)
            return TRUE;
    }
    return TRUE;
}

/* Asks for the lines and repaints each that changed (all of them when
 * `all`). The count stays what the layout gave them. */
static void status_refresh(menu_view *v, BOOLEAN all)
{
    menu_status_text now;
    UINTN i;

    if (v->status == (const menu_status *)0 || v->m.status_rows == 0)
        return;
    memset(&now, 0, sizeof now);
    v->status->fn(v->status->ctx, &now);
    for (i = 0; i < v->m.status_rows && i < MENU_STATUS_ROWS; i++) {
        now.line[i][MENU_STATUS_MAX - 1u] = 0;
        if (i >= now.n) {
            now.line[i][0] = 0;
            now.attr[i] = TUI_ATTR_TEXT;
        }
        if (!all && now.attr[i] == v->shown.attr[i] &&
            status_same(now.line[i], v->shown.line[i]))
            continue;
        v->st->ConOut->SetAttribute(v->st->ConOut, now.attr[i]);
        draw_row(v->st, v->m.status_row + i, v->m.left, v->m.width,
                 now.line[i]);
    }
    v->st->ConOut->SetAttribute(v->st->ConOut, TUI_ATTR_TEXT);
    v->shown = now;
}

/* The tag columns, sized to the window on screen, so a page is laid out by
 * what is on it; a row not probed yet counts as its placeholder. Says
 * whether either width changed, which is when the whole window is
 * repainted rather than the one row a probe filled. */
static BOOLEAN window_widths(menu_view *v, UINTN top)
{
    UINTN sig_w = 0;
    UINTN why_w = 0;
    UINTN i;
    BOOLEAN changed;

    for (i = top; i < top + v->m.win && i < v->count; i++) {
        UINTN a;
        UINTN b = 0;

        if (pending(v, i)) {
            a = ucs2_len(tui_tag_pending);
        } else {
            a = ucs2_len(v->entries[i].sig);
            b = ucs2_len(v->entries[i].why);
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

/* The first row in the window the probe has not reached, or count. Top to
 * bottom, the order the page is read in. */
static UINTN window_next(const menu_view *v, UINTN top)
{
    UINTN i;

    for (i = top; i < top + v->m.win && i < v->count; i++) {
        if (pending(v, i))
            return i;
    }
    return v->count;
}

/* A window shown for the first time starts the count again. */
static void window_count(menu_view *v, UINTN top)
{
    UINTN i;

    if (v->pend_top == top && v->pend_total != 0)
        return;
    v->pend_top = top;
    v->pend_total = 0;
    v->pend_done = 0;
    for (i = top; i < top + v->m.win && i < v->count; i++) {
        if (pending(v, i))
            v->pend_total++;
    }
}

static void paint_checking(const menu_view *v)
{
    CHAR16 text[80];

    tui_hint_checking(text, sizeof text / sizeof text[0],
                      v->pend_done + 1u, v->pend_total);
    v->st->ConOut->SetAttribute(v->st->ConOut, TUI_ATTR_TEXT);
    draw_row(v->st, v->m.hint_row, v->m.left, v->m.width, text);
}

static INTN run(EFI_SYSTEM_TABLE *st, const menu_banner *banner,
                const menu_entry *entries, menu_entry *rows, UINTN count,
                const menu_keys *keys, CHAR16 *which, UINTN sel,
                UINTN seconds, const menu_probe *probe,
                const menu_status *status);

INTN menu_run_keys(EFI_SYSTEM_TABLE *st, const menu_banner *banner,
                   const menu_entry *entries, UINTN count,
                   const menu_keys *keys, CHAR16 *which)
{
    return run(st, banner, entries, (menu_entry *)0, count, keys, which, 0,
               0, (const menu_probe *)0, (const menu_status *)0);
}

INTN menu_run_timed(EFI_SYSTEM_TABLE *st, const menu_banner *banner,
                    const menu_entry *entries, UINTN count,
                    const menu_keys *keys, CHAR16 *which, UINTN sel,
                    UINTN seconds, const menu_status *status)
{
    return run(st, banner, entries, (menu_entry *)0, count, keys, which, sel,
               seconds, (const menu_probe *)0, status);
}

INTN menu_run_probed(EFI_SYSTEM_TABLE *st, const menu_banner *banner,
                     menu_entry *entries, UINTN count, const menu_keys *keys,
                     CHAR16 *which, const menu_probe *probe)
{
    return run(st, banner, entries, entries, count, keys, which, 0, 0,
               probe, (const menu_status *)0);
}

static INTN run(EFI_SYSTEM_TABLE *st, const menu_banner *banner,
                const menu_entry *entries, menu_entry *rows, UINTN count,
                const menu_keys *keys, CHAR16 *which, UINTN sel,
                UINTN seconds, const menu_probe *probe,
                const menu_status *status)
{
    static menu_view v;
    UINTN top = 0, painted = 0;
    const CHAR16 *line = !count ? tui_hint_empty
                         : (keys != (const menu_keys *)0 &&
                            keys->hint != (const CHAR16 *)0)
                             ? keys->hint
                             : tui_hint_default;
    EFI_EVENT tick = (EFI_EVENT)0;
    UINTN left = 0;
    /* The countdown is running; the timer may outlive it for the status
     * lines. */
    BOOLEAN counting = FALSE;
    UINTN nstatus = 0;
    /* Set whenever every slot has to be redrawn: the first paint, and any
     * keystroke that scrolled the window. A plain highlight move does not
     * set it, and repaints two rows instead of the whole window. */
    BOOLEAN full = TRUE;

    if (sel >= count)
        sel = 0;
    if (count == 0)
        seconds = 0;

    v.st = st;
    v.entries = entries;
    v.count = count;
    v.rows = rows;
    v.probe = probe;
    v.status = (status != (const menu_status *)0 &&
                status->fn != (menu_status_fn)0) ? status
                                                 : (const menu_status *)0;
    memset(&v.shown, 0, sizeof v.shown);
    if (v.status != (const menu_status *)0) {
        v.status->fn(v.status->ctx, &v.shown);
        nstatus = v.shown.n < MENU_STATUS_ROWS ? v.shown.n
                                               : MENU_STATUS_ROWS;
    }
    frame_ensure(st);
    tui_menu_layout_status(&g_frame.f, count, nstatus, &v.m);
    v.sig_w = 0;
    v.why_w = 0;
    v.pend_top = 0;
    v.pend_total = 0;
    v.pend_done = 0;

    st->ConOut->EnableCursor(st->ConOut, FALSE);
    /* The one ClearScreen per screen, and what blanks the margins. Nothing
     * below writes outside the content box, so they stay blank. */
    st->ConOut->ClearScreen(st->ConOut);

    draw_banner(&v, banner);
    st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_TEXT);
    /* "expected" is the whole claim of the default hint: the tags are a
     * prediction and the platform decides at load time (DESIGN Section
     * 7.8). */
    draw_row(st, v.m.hint_row, v.m.left, v.m.width, line);
    status_refresh(&v, TRUE);

    /* The page is up: the idle time starts (console/idle.h). */
    idle_touch();

    if (seconds != 0 || v.m.status_rows != 0) {
        tick = countdown_start(st);
        if (tick != (EFI_EVENT)0 && seconds != 0) {
            left = seconds;
            counting = TRUE;
            paint_countdown(&v, left);
        }
    }

    for (;;) {
        UINTN i;
        UINTN next;
        EFI_INPUT_KEY key;
        EFI_EVENT events[3];

        /* Keep the selection inside the window before painting. Scrolling
         * changes what every slot shows, so it is the one case that still
         * costs a whole repaint. */
        if (tui_menu_scroll(&top, sel, v.m.win))
            full = TRUE;

        if (full) {
            (void)window_widths(&v, top);
            window_count(&v, top);
            for (i = 0; i < v.m.win; i++)
                paint_slot(&v, i, top, sel);
            full = FALSE;
        } else if (painted != sel) {
            /* top did not move, so the previously highlighted entry is still
             * on screen and its slot is still painted - top. */
            paint_slot(&v, painted - top, top, sel);
            paint_slot(&v, sel - top, top, sel);
        }
        painted = sel;

        /* Rows in the window not looked inside yet: one per pass, with the
         * page already on screen and the keys read between rows, so neither
         * the page nor a keystroke ever waits on a whole window of probes.
         * The key line says how far it has got, unless the countdown has
         * it. */
        next = window_next(&v, top);
        if (next < count) {
            if (!counting)
                paint_checking(&v);
            v.probe->fn(v.probe->ctx, next, &v.rows[next]);
            v.rows[next].probed = 1;
            v.pend_done++;
            if (window_widths(&v, top))
                full = TRUE;
            else
                paint_slot(&v, next - top, top, sel);
            if (window_next(&v, top) >= count && !counting) {
                st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_TEXT);
                draw_row(st, v.m.hint_row, v.m.left, v.m.width, line);
            }
            if (tick != (EFI_EVENT)0 &&
                st->BootServices->CheckEvent(tick) == EFI_SUCCESS)
                i = 1u;
            else if (st->BootServices->CheckEvent(st->ConIn->WaitForKey) ==
                     EFI_SUCCESS)
                i = 0u;
            else if (idle_event() != (EFI_EVENT)0 &&
                     st->BootServices->CheckEvent(idle_event()) ==
                         EFI_SUCCESS)
                i = 2u;
            else
                continue;
        } else {
            /* A failed wait means there is no console input to wait on,
             * and the read below would then spin on EFI_NOT_READY forever.
             * Backing out is what ESC does, which on the main menu hands
             * the machine back to the firmware's next boot option. */
            UINTN n = 0;
            UINTN at_tick = 3u;
            UINTN at_idle = 3u;

            events[n++] = st->ConIn->WaitForKey;
            if (tick != (EFI_EVENT)0) {
                at_tick = n;
                events[n++] = tick;
            }
            if (idle_event() != (EFI_EVENT)0) {
                at_idle = n;
                events[n++] = idle_event();
            }
            if (st->BootServices->WaitForEvent(n, events, &i) !=
                EFI_SUCCESS) {
                countdown_stop(st, &tick);
                restore_console(st);
                return -1;
            }
            i = i == at_tick ? 1u : i == at_idle ? 2u : 0u;
        }

        /* No key for the idle time: the machine goes off. */
        if (i == 2u) {
            idle_expired();
            continue;
        }

        /* A second: the status lines are asked again, and the countdown
         * steps. When it runs out the highlighted row is taken as though
         * enter had been pressed on it. */
        if (i == 1u) {
            status_refresh(&v, FALSE);
            if (!counting)
                continue;
            /* A countdown ends in a boot; the idle time waits for it. */
            idle_touch();
            if (--left != 0) {
                paint_countdown(&v, left);
                continue;
            }
            countdown_stop(st, &tick);
            if (which != (CHAR16 *)0)
                *which = L'\r';
            restore_console(st);
            return (INTN)sel;
        }

        /* Any key stops the countdown for good, and is then taken as any
         * key is: enter on the default boots it at once. The timer stays
         * while there are status lines to keep current. */
        if (counting) {
            counting = FALSE;
            if (v.m.status_rows == 0)
                countdown_stop(st, &tick);
            st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_TEXT);
            draw_row(st, v.m.hint_row, v.m.left, v.m.width, line);
        }

        /* Drain the queue, then paint once.
         *
         * Firmware keyboard drivers auto-repeat into this queue, so a held
         * arrow arrives as a dozen keystrokes; one repaint each makes the
         * repaints, not the keys, the backlog. Collapsing the run also means
         * a spurious WaitForKey signal leaves sel untouched and paints
         * nothing. */
        for (;;) {
            if (st->ConIn->ReadKeyStroke(st->ConIn, &key) != EFI_SUCCESS)
                break; /* EFI_NOT_READY: the queue is empty */
            idle_touch();

            if (key.ScanCode == EFI_SCAN_ESC) {
                countdown_stop(st, &tick);
                restore_console(st);
                return -1;
            }
            if (count == 0)
                continue;
            if (key.UnicodeChar == L'\r' ||
                is_commit_key(keys, key.UnicodeChar)) {
                /* Commit on the key as it is read, discarding whatever was
                 * typed behind it: the user chose an entry they could see. */
                if (which != (CHAR16 *)0)
                    *which = (key.UnicodeChar >= L'A' &&
                              key.UnicodeChar <= L'Z')
                                 ? (CHAR16)(key.UnicodeChar - L'A' + L'a')
                                 : key.UnicodeChar;
                countdown_stop(st, &tick);
                restore_console(st);
                return (INTN)sel;
            }
            if (key.ScanCode == EFI_SCAN_UP && sel > 0)
                sel--;
            else if (key.ScanCode == EFI_SCAN_DOWN && sel + 1 < count)
                sel++;
        }
    }
}
