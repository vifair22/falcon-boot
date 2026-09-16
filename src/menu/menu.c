#include <menu/menu.h>

/* Layout: banner on row 0, list window from LIST_TOP, hint line at the
 * bottom. Redraws are cursor-repositioned overwrites, not ClearScreen, to
 * avoid flicker on slow console paths (serial mirrors). */
#define LIST_TOP 2u

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

/* The console state a repaint needs. Bundled because paint_slot is called
 * from two places and the alternative is a seven-argument function. */
typedef struct {
    EFI_SYSTEM_TABLE *st;
    const menu_entry *entries;
    UINTN             count;
    UINTN             cols;
} menu_view;

static void out(EFI_SYSTEM_TABLE *st, CHAR16 *s)
{
    st->ConOut->OutputString(st->ConOut, s);
}

/* Columns a row may actually occupy. The last one is left alone: writing the
 * bottom-right cell auto-wraps on a conforming console and would scroll the
 * screen out from under the layout. */
static UINTN row_width(UINTN cols)
{
    UINTN w = cols - 1;

    return (w < ROW_MAX) ? w : ROW_MAX - 1u;
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

/* Overwrite one screen row with s, padded to width. One OutputString. */
static void draw_row(EFI_SYSTEM_TABLE *st, UINTN row, UINTN cols,
                     const CHAR16 *s)
{
    UINTN w = row_width(cols);

    row_pad(row_put(s, 0, w), w);
    st->ConOut->SetCursorPosition(st->ConOut, 0, row);
    out(st, g_row);
}

/* Title in the menu's own colour, state suffix in whatever attribute the
 * caller chose for it. Three calls rather than one, because SetAttribute
 * applies to subsequent output and the two segments differ; the banner is
 * painted once per screen, so the split costs nothing that matters. */
static void draw_banner(EFI_SYSTEM_TABLE *st, UINTN cols,
                        const menu_banner *banner)
{
    UINTN title_attr = EFI_TEXT_ATTR(EFI_LIGHTCYAN, EFI_BLACK);
    UINTN w = row_width(cols);
    UINTN col;

    st->ConOut->SetCursorPosition(st->ConOut, 0, 0);
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

static void screen_dims(EFI_SYSTEM_TABLE *st, UINTN *cols, UINTN *rows)
{
    if (st->ConOut->QueryMode(st->ConOut,
                              (UINTN)st->ConOut->Mode->Mode,
                              cols, rows) != EFI_SUCCESS ||
        *cols < 10 || *rows < 6) {
        /* Spec-guaranteed minimum mode. */
        *cols = 80;
        *rows = 25;
    }
    if (*cols > ROW_MAX)
        *cols = ROW_MAX;
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
        draw_row(v->st, LIST_TOP + slot, v->cols, v->entries[idx].label);
    } else {
        v->st->ConOut->SetAttribute(
            v->st->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
        draw_row(v->st, LIST_TOP + slot, v->cols,
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
    UINTN rows, win, top = 0, sel = 0, painted = 0;
    /* Set whenever every slot has to be redrawn: the first paint, and any
     * keystroke that scrolled the window. A plain highlight move does not
     * set it, and repaints two rows instead of the whole window. */
    BOOLEAN full = TRUE;

    v.st = st;
    v.entries = entries;
    v.count = count;
    screen_dims(st, &v.cols, &rows);
    win = rows - LIST_TOP - 2; /* list rows between banner and hint */

    st->ConOut->EnableCursor(st->ConOut, FALSE);
    st->ConOut->ClearScreen(st->ConOut);

    draw_banner(st, v.cols, banner);
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
    draw_row(st, rows - 1, v.cols,
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
        if (sel >= top + win) {
            top = sel - win + 1;
            full = TRUE;
        }

        if (full) {
            for (i = 0; i < win; i++)
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
