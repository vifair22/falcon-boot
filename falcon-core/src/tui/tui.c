#include <tui/tui.h>

#include <lib/ucs2.h>

/* figlet -f slant falcon. */
const uint16_t *const tui_logo[TUI_LOGO_ROWS] = {
    u"    ____      __",
    u"   / __/___ _/ /________  ____",
    u"  / /_/ __ `/ / ___/ __ \\/ __ \\",
    u" / __/ /_/ / / /__/ /_/ / / / /",
    u"/_/  \\__,_/_/\\___/\\____/_/ /_/",
};

const uint16_t tui_hint_default[] =
    u"up/down select | enter choose | esc back | "
    u"[signed by] {expected refusal}";
const uint16_t tui_hint_plain[] =
    u"up/down select | enter choose | esc back";
const uint16_t tui_hint_empty[] = u"no entries | esc back";
const uint16_t tui_list_empty[] = u"(no ISOs found on the exFAT partition)";

uint8_t tui_entry_attr(int caution, int selected)
{
    if (caution)
        return selected ? TUI_ATTR(TUI_BLACK, TUI_BROWN)
                        : TUI_ATTR(TUI_YELLOW, TUI_BLACK);
    return selected ? TUI_ATTR(TUI_BLACK, TUI_LIGHTGRAY) : TUI_ATTR_TEXT;
}

/* Where the text in a row starts and where it must stop, leaving TUI_PAD_X
 * clear at each end. A row too narrow to inset gives the text the whole row
 * rather than nothing. */
static size_t text_start(size_t w)
{
    return (w > (size_t)2u * TUI_PAD_X) ? TUI_PAD_X : 0u;
}

static size_t text_end(size_t w)
{
    return (w > (size_t)2u * TUI_PAD_X) ? w - TUI_PAD_X : w;
}

/* Append s into row at col, clipped to w so a long path cannot wrap and
 * shift every row below it by one. Returns the column reached. */
static size_t row_put(uint16_t *row, const uint16_t *s, size_t col, size_t w)
{
    if (s == (const uint16_t *)0)
        return col;
    while (*s != 0 && col < w)
        row[col++] = *s++;
    return col;
}

/* Fill row from col to w with spaces, erasing whatever the row held before,
 * and terminate. Returns the column reached. */
static size_t row_pad(uint16_t *row, size_t col, size_t w)
{
    while (col < w)
        row[col++] = u' ';
    row[col] = 0;
    return col;
}

static size_t len_of(const uint16_t *s)
{
    return (s == (const uint16_t *)0) ? 0u : ucs2_len(s);
}

void tui_frame_init(tui_frame *f, size_t cols, size_t rows)
{
    size_t w;
    size_t margin;

    /* The frame spends four rows and 2 * TUI_MARGIN_X columns, so the floor
     * is what keeps the list window from collapsing on a console that
     * reports something absurd. */
    if (cols < 40u || rows < 12u) {
        cols = 80u;
        rows = 25u;
    }
    if (cols > TUI_ROW_MAX)
        cols = TUI_ROW_MAX;

    margin = (cols <= TUI_NARROW_COLS) ? TUI_MARGIN_X_NARROW : TUI_MARGIN_X;
    w = cols - (size_t)2u * margin;
    if (w > TUI_WIDTH_MAX)
        w = TUI_WIDTH_MAX;
    if (w > TUI_ROW_MAX - 1u)
        w = TUI_ROW_MAX - 1u;
    f->left = (cols - w) / 2u;
    f->width = w;
    f->text_col = f->left + text_start(w);
    f->text_width = text_end(w) - text_start(w);
    f->rows = rows;
}

size_t tui_page_row(const tui_frame *f)
{
    size_t row = f->rows / 3u;

    return (row < TUI_MARGIN_Y) ? TUI_MARGIN_Y : row;
}

int tui_mode_at(const tui_frame *f, size_t len, size_t *row, size_t *col)
{
    size_t top = tui_page_row(f);

    if (top == 0u || len > f->text_width)
        return 0;
    *row = top - 1u;
    *col = f->text_col + f->text_width - len;
    return 1;
}

void tui_menu_layout(const tui_frame *f, size_t count, tui_menu *m)
{
    size_t avail;
    size_t list;
    size_t block;

    m->width = f->width;
    m->left = f->left;

    /* Symmetrical, so the bar's right edge steps in by as much as its left.
     * A block too narrow to indent and still hold an inset row keeps the
     * full width rather than collapsing to nothing. */
    if (f->width > (size_t)2u * TUI_LIST_INDENT + (size_t)2u * TUI_PAD_X) {
        m->list_left = f->left + TUI_LIST_INDENT;
        m->list_width = f->width - (size_t)2u * TUI_LIST_INDENT;
    } else {
        m->list_left = f->left;
        m->list_width = f->width;
    }

    /* What the margins leave, minus the chrome, is the most list the screen
     * can hold. A shorter list shrinks the block instead of padding it out;
     * a longer one fills the screen and scrolls. */
    avail = f->rows - (size_t)2u * TUI_MARGIN_Y;
    list = (avail > TUI_CHROME_ROWS) ? avail - TUI_CHROME_ROWS : 1u;
    if (count == 0)
        list = 1u;               /* the "nothing found" row still needs one */
    else if (count < list)
        list = count;
    m->win = list;

    /* The logo and the blank under it, when the whole list fits with them
     * and the lettering fits across. */
    m->logo_rows = 0;
    if (list + TUI_CHROME_ROWS + TUI_LOGO_ROWS + 1u <= avail &&
        list == (count == 0 ? 1u : count) && f->text_width >= TUI_LOGO_WIDTH)
        m->logo_rows = TUI_LOGO_ROWS;

    block = list + TUI_CHROME_ROWS +
            (m->logo_rows != 0 ? m->logo_rows + 1u : 0u);
    m->logo_row = (f->rows - block) / 2u;
    m->banner_row = m->logo_row +
                    (m->logo_rows != 0 ? m->logo_rows + 1u : 0u);
    m->list_top = m->banner_row + 2u;      /* banner, then a blank row */
    m->hint_row = m->list_top + list + 1u; /* blank, then the hint */
}

int tui_menu_scroll(size_t *top, size_t sel, size_t win)
{
    if (sel < *top) {
        *top = sel;
        return 1;
    }
    if (win != 0 && sel >= *top + win) {
        *top = sel - win + 1u;
        return 1;
    }
    return 0;
}

void tui_row_text(uint16_t *row, size_t w, const uint16_t *s)
{
    size_t col = row_pad(row, 0, text_start(w));

    col = row_put(row, s, col, text_end(w));
    row_pad(row, col, w);
}

void tui_row_entry(uint16_t *row, size_t w, const uint16_t *label,
                   const uint16_t *sig, const uint16_t *why,
                   size_t sig_w, size_t why_w)
{
    size_t start = text_start(w);
    size_t end = text_end(w);
    size_t sig_n = len_of(sig);
    size_t why_n = len_of(why);
    size_t cols = sig_w + (why_w != 0 ? 2u + why_w : 0u);
    size_t own = sig_n + (why_n != 0 ? 2u + why_n : 0u);
    size_t col = row_pad(row, 0, start);

    if (cols != 0 && end - start >= TUI_NAME_MIN + 2u + cols) {
        size_t tag_at = end - cols;

        col = row_put(row, label, col, tag_at - 2u);
        col = row_pad(row, col, tag_at);
        col = row_put(row, sig, col, end);
        if (why_n != 0) {
            col = row_pad(row, col, tag_at + sig_w + 2u);
            col = row_put(row, why, col, end);
        }
    } else if (own != 0 && end - start >= 1u + 2u + own) {
        size_t tag_at = end - own;

        col = row_put(row, label, col, tag_at - 2u);
        col = row_pad(row, col, tag_at);
        col = row_put(row, sig, col, end);
        if (why_n != 0) {
            col = row_put(row, u"  ", col, end);
            col = row_put(row, why, col, end);
        }
    } else {
        col = row_put(row, label, col, end);
    }
    row_pad(row, col, w);
}

void tui_row_banner(uint16_t *row, size_t w, const uint16_t *title,
                    const uint16_t *state, size_t *state_at,
                    size_t *state_end)
{
    size_t end = text_end(w);
    size_t need = (state != (const uint16_t *)0) ? len_of(state) + 2u : 0u;
    size_t col;

    if (need < end - text_start(w))
        end -= need;
    col = row_put(row, title, row_pad(row, 0, text_start(w)), end);
    *state_at = col;
    if (state != (const uint16_t *)0) {
        col = row_put(row, u"  ", col, text_end(w));
        col = row_put(row, state, col, text_end(w));
    }
    *state_end = col;
    row_pad(row, col, w);
}
