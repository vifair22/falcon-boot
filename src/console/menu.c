#include <console/menu.h>

#include <lib/string.h>

/* The frame.
 *
 * The console font is a fixed 8x19 cell in every mode, so a smaller mode does
 * not enlarge the text; it makes the firmware's own console driver centre a
 * smaller grid and pad the rest itself. Under OVMF at 1280x800 that padding
 * is 320 pixels a side at 80x25 and 240 at 100x31, which is why menu_init
 * takes the largest mode: only on that grid is the surrounding black ours to
 * place.
 *
 * Content is a block, CHROME_ROWS (banner, blank, blank, hint) plus its
 * entries, centred in the grid rather than stretched to its corners.
 * MARGIN_X and MARGIN_Y are the floor on how close it may come to an edge,
 * and WIDTH_MAX caps its width because a 160-column row of entries is harder
 * to scan than a 100-column one.
 *
 * ClearScreen blanks everything on entry and nothing writes outside the block
 * afterwards, so the surround stays dark. Redraws are cursor-repositioned
 * overwrites, not ClearScreen, to avoid flicker on slow console paths. */
#define MARGIN_X    6u
#define MARGIN_Y    1u
#define WIDTH_MAX 100u
#define CHROME_ROWS 4u

/* Blank cells at each end of every row. Rows are padded to the block's full
 * width so the selection bar covers them, so without this the bar would start
 * on the first glyph. Applied to banner and hint too, which carry no bar, so
 * that all three share one text column. */
#define PAD_X 2u

/* How far the list steps in from the block's edge, selection bar and all.
 * Banner and hint are chrome and stay at the edge; the entries indent under
 * them. */
#define LIST_INDENT 2u

/* Every screen row is composed here and emitted in one OutputString.
 *
 * Emitting per character instead costs a call per column: ~1650 to repaint a
 * 21-row window at 80x25, each one a ConSplitter dispatch to every registered
 * console plus, on the GOP-backed one, an HII glyph lookup and a single-cell
 * blt into a write-combining framebuffer.
 *
 * Sized past any mode worth setting; screen_dims clamps the drawn width to it
 * so a wider mode loses its rightmost columns rather than its composition. */
#define ROW_MAX 512u

static CHAR16 g_row[ROW_MAX];

/* The content block, fixed for the run.
 *
 * It used to live in menu_run's locals, which meant the menu was the only
 * screen that had one: the passphrase prompt, every confirmation and every
 * error drew from column zero while the menu sat in a centred block, and on
 * a wide console they did not look like the same program. The frame is
 * module state now and menu_screen/menu_out draw inside it, so a caller gets
 * the layout by writing text rather than by knowing about it.
 *
 * left/width are the block. text_col is where a line's first glyph goes,
 * which is the block plus PAD_X. rows is the console height, kept so the
 * menu can centre itself in it. */
typedef struct {
    UINTN left;
    UINTN width;
    UINTN text_col;
    UINTN text_width;
    UINTN rows;
    UINTN col;          /* cursor column within the text area, for wrapping */
    BOOLEAN ready;
} menu_frame;

static menu_frame g_frame;

/* The console state and the resolved frame. Bundled because paint_slot is
 * called from two places and the alternative is a nine-argument function. */
typedef struct {
    EFI_SYSTEM_TABLE *st;
    const menu_entry *entries;
    UINTN             count;
    UINTN             left;       /* first column of the content block */
    UINTN             width;      /* columns in the block */
    UINTN             list_left;  /* the block, stepped in by LIST_INDENT */
    UINTN             list_width;
    UINTN             banner_row;
    UINTN             list_top;  /* first list row */
    UINTN             win;       /* list rows */
    UINTN             hint_row;
    UINTN             sig_w;     /* widest sig tag in this list */
    UINTN             why_w;     /* widest why tag in this list */
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

/* Where the text in a row starts and where it must stop, leaving PAD_X clear
 * at each end. A box too narrow to inset gives the text the whole row rather
 * than nothing. */
static UINTN text_end(UINTN w)
{
    return (w > (UINTN)2u * PAD_X) ? w - PAD_X : w;
}

static UINTN text_start(UINTN w)
{
    return (w > (UINTN)2u * PAD_X) ? PAD_X : 0u;
}

/* Overwrite one row of the content box with s, inset and padded to the box.
 * One OutputString. The right margin is what makes writing the last cell
 * safe: the box never reaches the final column, so nothing can auto-wrap. */
static void draw_row(EFI_SYSTEM_TABLE *st, UINTN row, UINTN left, UINTN w,
                     const CHAR16 *s)
{
    UINTN col = row_pad(0, text_start(w));

    col = row_put(s, col, text_end(w));
    row_pad(col, w);
    st->ConOut->SetCursorPosition(st->ConOut, left, row);
    out(st, g_row);
}

/* A name this narrow is where aligned tag columns stop being worth it. */
#define NAME_MIN 12u

/* One list row: the name, then the tag columns (DESIGN Section 7.8).
 *
 * Aligned when the row has room: every row's sig starts at the column the
 * widest sig and why on this screen fix, so the tags read as columns. The
 * NAME is what gets cut, never a tag: a clipped name is still recognisable,
 * a clipped tag misstates what was found. When aligning would squeeze the
 * name below NAME_MIN, each row places its own tags at the right edge
 * instead, and only on a console too narrow for even that does the name win
 * and the tags go. */
static void draw_entry(const menu_view *v, UINTN row, const menu_entry *e)
{
    UINTN w = v->list_width;
    UINTN start = text_start(w);
    UINTN end = text_end(w);
    UINTN sig = ucs2_len(e->sig);
    UINTN why = ucs2_len(e->why);
    UINTN cols = v->sig_w + (v->why_w != 0 ? 2u + v->why_w : 0u);
    UINTN own = sig + (why != 0 ? 2u + why : 0u);
    UINTN col = row_pad(0, start);

    if (cols != 0 && end - start >= NAME_MIN + 2u + cols) {
        UINTN tag_at = end - cols;

        col = row_put(e->label, col, tag_at - 2u);
        col = row_pad(col, tag_at);
        col = row_put(e->sig, col, end);
        if (why != 0) {
            col = row_pad(col, tag_at + v->sig_w + 2u);
            col = row_put(e->why, col, end);
        }
    } else if (own != 0 && end - start >= 1u + 2u + own) {
        UINTN tag_at = end - own;

        col = row_put(e->label, col, tag_at - 2u);
        col = row_pad(col, tag_at);
        col = row_put(e->sig, col, end);
        if (why != 0) {
            col = row_put(L"  ", col, end);
            col = row_put(e->why, col, end);
        }
    } else {
        col = row_put(e->label, col, end);
    }
    row_pad(col, w);
    v->st->ConOut->SetCursorPosition(v->st->ConOut, v->list_left, row);
    out(v->st, g_row);
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

    /* When both do not fit, the title gives way: the state is the part a
     * user needs (DESIGN Section 7.4 wants it unmissable, 7.8 puts what the
     * machine trusts in it), and a clipped version string loses nothing. */
    {
        UINTN end = text_end(w);
        UINTN need = (banner->state != (void *)0)
                         ? ucs2_len(banner->state) + 2u
                         : 0u;

        if (need < end - text_start(w))
            end -= need;
        col = row_put(banner->title, row_pad(0, text_start(w)), end);
    }
    g_row[col] = L'\0';
    out(st, g_row);

    if (banner->state != (void *)0) {
        UINTN seg = col;

        col = row_put(L"  ", col, text_end(w));
        col = row_put(banner->state, col, text_end(w));
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

/* Fix the frame from the console size. Idempotent, and safe to call from any
 * screen: menu_init sets it at startup, and anything that draws before that
 * gets it on first use rather than drawing at column zero. */
static void frame_ensure(EFI_SYSTEM_TABLE *st)
{
    UINTN cols;
    UINTN rows;
    UINTN w;

    if (g_frame.ready) {
        return;
    }
    screen_dims(st, &cols, &rows);

    w = cols - (UINTN)2u * MARGIN_X;
    if (w > WIDTH_MAX) {
        w = WIDTH_MAX;
    }
    if (w > ROW_MAX - 1u) {
        w = ROW_MAX - 1u;
    }
    g_frame.left = (cols - w) / 2u;
    g_frame.width = w;
    g_frame.text_col = g_frame.left + text_start(w);
    g_frame.text_width = text_end(w) - text_start(w);
    g_frame.rows = rows;
    g_frame.col = 0;
    g_frame.ready = TRUE;
}

/* Start a screen: clear it and park the cursor at the block's first text
 * column. Everything written with menu_out lands inside the block from there.
 *
 * The row is a third of the way down rather than at the top margin, and that
 * is the vertical half of the same problem the block solved horizontally. A
 * menu centres itself, so a prompt or an error pinned to row one sat in a
 * different place on the screen from the menu it replaced and the two did not
 * read as one program. A third down puts a short screen, which is nearly all
 * of them, at about the height a menu block sits at, while still leaving two
 * thirds below for a long one to run into. */
void menu_screen(EFI_SYSTEM_TABLE *st)
{
    UINTN row;

    if (st == (void *)0) {
        return;
    }
    frame_ensure(st);
    row = g_frame.rows / 3u;
    if (row < MARGIN_Y) {
        row = MARGIN_Y;
    }
    st->ConOut->SetAttribute(st->ConOut,
                             EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
    st->ConOut->ClearScreen(st->ConOut);
    st->ConOut->SetCursorPosition(st->ConOut, g_frame.text_col, row);
    g_frame.col = 0;
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
            st->ConOut->SetCursorPosition(st->ConOut, g_frame.text_col,
                                          (UINTN)st->ConOut->Mode->CursorRow);
            g_frame.col = 0;
            i++;
            continue;
        }
        if (g_frame.text_width != 0 && g_frame.col >= g_frame.text_width) {
            out(st, L"\r\n");
            st->ConOut->SetCursorPosition(st->ConOut, g_frame.text_col,
                                          (UINTN)st->ConOut->Mode->CursorRow);
            g_frame.col = 0;
        }
        chunk[0] = c;
        out(st, chunk);
        g_frame.col++;
        i++;
    }
}

/* Resolve the console size and the entry count into the content block.
 *
 * v->count must already be set: the block's height is how many entries there
 * are, which is the whole point of sizing it here rather than at the edges
 * of the screen. */
static void layout(menu_view *v)
{
    UINTN rows, w, avail, list, block;

    frame_ensure(v->st);
    w = g_frame.width;
    rows = g_frame.rows;
    v->width = w;
    v->left = g_frame.left;

    /* Symmetrical, so the bar's right edge steps in by as much as its left.
     * A block too narrow to indent and still hold an inset row keeps the
     * full width rather than collapsing to nothing. */
    if (w > (UINTN)2u * LIST_INDENT + (UINTN)2u * PAD_X) {
        v->list_left  = v->left + LIST_INDENT;
        v->list_width = w - (UINTN)2u * LIST_INDENT;
    } else {
        v->list_left  = v->left;
        v->list_width = w;
    }

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

    /* Mode 0 is 80x25 by spec and mode 1 is 80x50 when present; 2 and up are
     * OEM-defined and are where the useful ones live. Largest wins, rows
     * first, because the block is a list and columns past WIDTH_MAX become
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
        UINTN attr;

        /* Caution is the whole row, not the tag. Selected, the bar itself
         * turns: yellow text on the light-gray bar is unreadable, and black
         * on brown (which firmware draws as dark yellow) is the same warning
         * in a form that still reads. */
        if (e->caution)
            attr = (idx == sel) ? EFI_TEXT_ATTR(EFI_BLACK, EFI_BROWN)
                                : EFI_TEXT_ATTR(EFI_YELLOW, EFI_BLACK);
        else
            attr = (idx == sel) ? EFI_TEXT_ATTR(EFI_BLACK, EFI_LIGHTGRAY)
                                : EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK);
        v->st->ConOut->SetAttribute(v->st->ConOut, attr);
        draw_entry(v, v->list_top + slot, e);
    } else {
        v->st->ConOut->SetAttribute(
            v->st->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
        draw_row(v->st, v->list_top + slot, v->list_left, v->list_width,
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
    v.sig_w = 0;
    v.why_w = 0;
    {
        UINTN i;

        for (i = 0; i < count; i++) {
            UINTN a = ucs2_len(entries[i].sig);
            UINTN b = ucs2_len(entries[i].why);

            if (a > v.sig_w)
                v.sig_w = a;
            if (b > v.why_w)
                v.why_w = b;
        }
    }

    st->ConOut->EnableCursor(st->ConOut, FALSE);
    /* The one ClearScreen per screen, and what blanks the margins. Nothing
     * below writes outside the content box, so they stay blank. */
    st->ConOut->ClearScreen(st->ConOut);

    draw_banner(&v, banner);
    st->ConOut->SetAttribute(st->ConOut,
                             EFI_TEXT_ATTR(EFI_DARKGRAY, EFI_BLACK));
    /* "expected" is the whole claim: the tags are a prediction and the
     * platform decides at load time (DESIGN Section 7.8).
     *
     * The two key words stay general because what they do is the caller's:
     * enter opens a folder or boots an image on the main menu and picks an
     * image in the personal-OS chooser, and esc leaves a folder, the chooser
     * or the loader depending on where the caller is. Naming one of those
     * here would be wrong on the other screens. */
    draw_row(st, v.hint_row, v.left, v.width,
             count ? L"up/down select | enter choose | esc back | "
                     L"[signed by] {expected refusal}"
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
