#include <menu/menu.h>

/* The frame.
 *
 * A margin is only worth drawing on a grid that covers the panel, which is
 * why menu_init takes the largest mode rather than a comfortable one. The
 * font is a fixed 8x19 cell whatever mode is set, so a smaller mode does not
 * make the text bigger; it makes the firmware's own console driver centre a
 * smaller grid and pad the rest itself. Measured under OVMF at 1280x800:
 * 80x25 leaves 320 pixels of firmware padding per side and 100x31 leaves
 * 240, next to which a six-column margin of ours is invisible. On the
 * largest mode there is no padding, and every pixel of black around the menu
 * is one this module put there.
 *
 * The content is then a BLOCK, sized by what it holds and centred in the
 * grid, rather than a layout stretched to the grid's corners. Stretching is
 * what put five entries at the top of a 42-row screen with the hint pinned
 * to the bottom of the panel. CHROME_ROWS is the block's fixed part: banner,
 * blank, blank, hint.
 *
 * MARGIN_X and MARGIN_Y are the floor on how close the block may come to an
 * edge, not the layout itself. WIDTH_MAX caps its width, because a
 * 160-column row of menu entries is harder to scan than a 100-column one.
 *
 * ClearScreen blanks everything on entry and nothing writes outside the
 * block afterwards, so the surround stays dark and the highlight bar stops
 * at the block. Redraws are cursor-repositioned overwrites, not ClearScreen,
 * to avoid flicker on slow console paths (serial mirrors). */
#define MARGIN_X    6u
#define MARGIN_Y    1u
#define WIDTH_MAX 100u
#define CHROME_ROWS 4u

/* Every screen row is composed here and then handed to the console in ONE
 * OutputString call.
 *
 * This is the whole reason the module has a buffer. The first version emitted
 * a two-CHAR16 string per character, padding included, which at the
 * spec-minimum 80x25 is about 1650 protocol calls to repaint a 21-row window.
 * Each one is a ConSplitter dispatch to every registered console, and on the
 * GOP-backed one an HII glyph lookup plus a single-cell blt into a
 * write-combining framebuffer. Composing first turns a row into one call.
 *
 * Sized well past any console mode worth setting; screen_dims clamps the
 * drawn width to it so a wider mode loses its rightmost columns rather than
 * its row composition. */
#define ROW_MAX 512u

static CHAR16 g_row[ROW_MAX];

/* The console state and the resolved frame. Bundled because paint_slot is
 * called from two places and the alternative is a nine-argument function. */
typedef struct {
    EFI_SYSTEM_TABLE *st;
    const menu_entry *entries;
    UINTN             count;
    UINTN             left;      /* first column of the content box */
    UINTN             width;     /* columns in the box */
    UINTN             banner_row;
    UINTN             list_top;  /* first list row */
    UINTN             win;       /* list rows */
    UINTN             hint_row;
} menu_view;

static void out(EFI_SYSTEM_TABLE *st, CHAR16 *s)
{
    st->ConOut->OutputString(st->ConOut, s);
}

/* Append s into g_row at col, clipped to w so a long path cannot wrap and
 * shift every row below it by one. Returns the column reached. */
static UINTN row_put(const CHAR16 *s, UINTN col, UINTN w)
{
    if (s == (void *)0)
        return col;
    while (*s != L'\0' && col < w)
        g_row[col++] = *s++;
    return col;
}

/* Fill g_row from col to w with spaces, erasing whatever the row held
 * before, and terminate. Returns the column reached. */
static UINTN row_pad(UINTN col, UINTN w)
{
    while (col < w)
        g_row[col++] = L' ';
    g_row[col] = L'\0';
    return col;
}

/* Overwrite one row of the content box with s, padded to the box. One
 * OutputString. The right margin is what makes writing the last cell safe:
 * the box never reaches the final column, so nothing can auto-wrap. */
static void draw_row(EFI_SYSTEM_TABLE *st, UINTN row, UINTN left, UINTN w,
                     const CHAR16 *s)
{
    row_pad(row_put(s, 0, w), w);
    st->ConOut->SetCursorPosition(st->ConOut, left, row);
    out(st, g_row);
}

/* Title in the menu's own colour, state suffix in whatever attribute the
 * caller chose for it. Three calls rather than one, because SetAttribute
 * applies to subsequent output and the two segments differ; the banner is
 * painted once per screen, so the split costs nothing that matters. */
static void draw_banner(const menu_view *v, const menu_banner *banner)
{
    EFI_SYSTEM_TABLE *st = v->st;
    UINTN title_attr = EFI_TEXT_ATTR(EFI_LIGHTCYAN, EFI_BLACK);
    UINTN w = v->width;
    UINTN col;

    st->ConOut->SetCursorPosition(st->ConOut, v->left, v->banner_row);
    st->ConOut->SetAttribute(st->ConOut, title_attr);
    if (banner == (void *)0) {
        row_pad(0, w);
        out(st, g_row);
        return;
    }

    col = row_put(banner->title, 0, w);
    g_row[col] = L'\0';
    out(st, g_row);

    if (banner->state != (void *)0) {
        UINTN seg = col;

        col = row_put(L"  ", col, w);
        col = row_put(banner->state, col, w);
        g_row[col] = L'\0';
        st->ConOut->SetAttribute(st->ConOut, banner->state_attr);
        /* The cursor is already parked at seg, so emitting the tail alone
         * continues the row rather than restarting it. */
        out(st, g_row + seg);
        st->ConOut->SetAttribute(st->ConOut, title_attr);
    }
    row_pad(col, w);
    out(st, g_row + col);
}

/* The frame spends four rows and 2 * MARGIN_X columns, so the floor here is
 * what keeps the list window from collapsing on a mode that reports
 * something absurd. 80x25 is guaranteed to exist, so falling back to it is
 * always an option. */
static void screen_dims(EFI_SYSTEM_TABLE *st, UINTN *cols, UINTN *rows)
{
    if (st->ConOut->QueryMode(st->ConOut,
                              (UINTN)st->ConOut->Mode->Mode,
                              cols, rows) != EFI_SUCCESS ||
        *cols < 40 || *rows < 12) {
        /* Spec-guaranteed minimum mode. */
        *cols = 80;
        *rows = 25;
    }
    if (*cols > ROW_MAX)
        *cols = ROW_MAX;
}

/* Resolve the console size and the entry count into the content block.
 *
 * v->count must already be set: the block's height is how many entries there
 * are, which is the whole point of sizing it here rather than at the edges
 * of the screen. */
static void layout(menu_view *v)
{
    UINTN cols, rows, w, avail, list, block;

    screen_dims(v->st, &cols, &rows);

    w = cols - (UINTN)2u * MARGIN_X;
    if (w > WIDTH_MAX)
        w = WIDTH_MAX;
    if (w > ROW_MAX - 1u)
        w = ROW_MAX - 1u;
    v->width = w;
    v->left = (cols - w) / 2u;

    /* What the margins leave, minus the chrome, is the most list the screen
     * can hold. A shorter list shrinks the block instead of padding it out;
     * a longer one fills the screen and scrolls, exactly as before. */
    avail = rows - (UINTN)2u * MARGIN_Y;
    list = (avail > CHROME_ROWS) ? avail - CHROME_ROWS : 1u;
    if (v->count == 0)
        list = 1u;               /* the "nothing found" row still needs one */
    else if (v->count < list)
        list = v->count;
    v->win = list;

    block = list + CHROME_ROWS;
    v->banner_row = (rows - block) / 2u;
    v->list_top   = v->banner_row + 2u;      /* banner, then a blank row */
    v->hint_row   = v->list_top + list + 1u; /* blank, then the hint */
}

/* Sanity rails only. Nothing real reaches these; they exist so a firmware
 * reporting nonsense cannot hand layout() a grid it will subtract from. */
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

    /* Firmware boots in whatever mode it likes, usually the spec-minimum
     * 80x25 even on a panel with room for several times that, and the loader
     * took that as given. Mode 0 is 80x25 by spec and mode 1 is 80x50 when
     * present; 2 and up are OEM-defined and are where the useful ones live.
     *
     * The largest mode wins, by rows first because the block that sits in it
     * is a list and columns past WIDTH_MAX become gutter either way. Taking
     * the largest is also what makes MARGIN_X mean anything; the frame
     * comment at the top of this file has the measurement.
     *
     * Every step is allowed to fail and none of them is checked twice:
     * QueryMode returns EFI_UNSUPPORTED for modes below MaxMode on firmware
     * that miscounts, and SetMode can refuse a mode it just described. Either
     * way the console keeps what it already had, which is a worse layout and
     * not a broken one. */
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
}

/* Paint one slot of the list window. slot is 0-based within the window; the
 * entry it shows is top + slot, which may be past the end. */
static void paint_slot(const menu_view *v, UINTN slot, UINTN top, UINTN sel)
{
    UINTN idx = top + slot;

    if (idx < v->count) {
        v->st->ConOut->SetAttribute(
            v->st->ConOut,
            idx == sel ? EFI_TEXT_ATTR(EFI_BLACK, EFI_LIGHTGRAY)
                       : EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
        draw_row(v->st, v->list_top + slot, v->left, v->width,
                 v->entries[idx].label);
    } else {
        v->st->ConOut->SetAttribute(
            v->st->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
        draw_row(v->st, v->list_top + slot, v->left, v->width,
                 (idx == 0) ? L"(no ISOs found on the exFAT partition)"
                            : (void *)0);
    }
}

static void restore_console(EFI_SYSTEM_TABLE *st)
{
    st->ConOut->SetAttribute(st->ConOut,
                             EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
    st->ConOut->EnableCursor(st->ConOut, TRUE);
}

INTN menu_run(EFI_SYSTEM_TABLE *st, const menu_banner *banner,
              const menu_entry *entries, UINTN count)
{
    menu_view v;
    UINTN top = 0, sel = 0, painted = 0;
    /* Set whenever every slot has to be redrawn: the first paint, and any
     * keystroke that scrolled the window. A plain highlight move does not
     * set it, and repaints two rows instead of the whole window. */
    BOOLEAN full = TRUE;

    v.st = st;
    v.entries = entries;
    v.count = count;
    layout(&v);

    st->ConOut->EnableCursor(st->ConOut, FALSE);
    /* The one ClearScreen per screen, and what blanks the margins. Nothing
     * below writes outside the content box, so they stay blank. */
    st->ConOut->ClearScreen(st->ConOut);

    draw_banner(&v, banner);
    st->ConOut->SetAttribute(st->ConOut,
                             EFI_TEXT_ATTR(EFI_DARKGRAY, EFI_BLACK));
    /* The legend is part of the hint rather than a header row because the
     * per-entry label is easy to misread as a verdict, and the one place a
     * user is looking when they read a label is this screen. "presence" is
     * the whole claim: the menu never verifies anything, and neither does
     * anything else in the loader (DESIGN Section 7.3).
     *
     * The two key words stay general because what they do is the caller's:
     * enter opens a folder or boots an image on the main menu and picks an
     * image in the personal-OS chooser, and esc leaves a folder, the chooser
     * or the loader depending on where the caller is. Naming one of those
     * here would be wrong on the other screens. */
    draw_row(st, v.hint_row, v.left, v.width,
             count ? L"up/down select | enter choose | esc back | "
                     L"[family, signature presence]"
                   : L"no entries | esc back");

    for (;;) {
        UINTN i;
        EFI_INPUT_KEY key;

        /* Keep the selection inside the window before painting. Scrolling
         * changes what every slot shows, so it is the one case that still
         * costs a whole repaint. */
        if (sel < top) {
            top = sel;
            full = TRUE;
        }
        if (sel >= top + v.win) {
            top = sel - v.win + 1;
            full = TRUE;
        }

        if (full) {
            for (i = 0; i < v.win; i++)
                paint_slot(&v, i, top, sel);
            full = FALSE;
        } else if (painted != sel) {
            /* top did not move, so the previously highlighted entry is still
             * on screen and its slot is still painted - top. */
            paint_slot(&v, painted - top, top, sel);
            paint_slot(&v, sel - top, top, sel);
        }
        painted = sel;

        st->BootServices->WaitForEvent(1, &st->ConIn->WaitForKey, &i);

        /* Drain the queue before painting again.
         *
         * Firmware keyboard drivers auto-repeat into this queue, so a held
         * arrow arrives as a dozen keystrokes. Servicing one per repaint is
         * what made the highlight carry on moving after the key came up: the
         * repaints, not the keys, were the backlog. Collapsing the run into
         * a single paint also means a spurious WaitForKey signal, which
         * leaves sel untouched, repaints nothing at all. */
        for (;;) {
            if (st->ConIn->ReadKeyStroke(st->ConIn, &key) != EFI_SUCCESS)
                break; /* EFI_NOT_READY: the queue is empty */

            if (key.ScanCode == EFI_SCAN_ESC) {
                restore_console(st);
                return -1;
            }
            if (count == 0)
                continue;
            if (key.UnicodeChar == L'\r') {
                /* Commit on the key as it is read, discarding whatever was
                 * typed behind it: the user chose an entry they could see. */
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
