/* falcon-core/src/tui: what the module's files share. Not for callers. */

#ifndef FALCON_TUI_INTERNAL_H
#define FALCON_TUI_INTERNAL_H

#include <tui/tui.h>

/* The surface tui_out writes to. */
#define SURF_NONE   0
#define SURF_BUSY   1
#define SURF_PAGE   2
#define SURF_DIALOG 3
#define SURF_LIST   4
#define SURF_GONE   5     /* handed off: the payload has the display */

/* The widest the panels and the area they sit in grow on a large console:
 * a list row wider than this is harder to read, not easier. */
#define AREA_MAX 116u

/* The dialog's text, and how many lines of it are laid out. */
#define DLG_TEXT_MAX  3072u
#define DLG_LINES_MAX 96u

/* A step's text, kept for tui_stage and its redraw. */
#define STEP_MAX 96u

/* Lines the startup box keeps. */
#define BUSY_MAX 8u

/* A line of the serial transcript. */
#define LOG_MAX 256u

/* Box styles. */
#define BOX_SINGLE 0
#define BOX_DOUBLE 1

/* The colours of the layers. */
#define A_BACKDROP  TUI_ATTR(TUI_DARKGRAY, TUI_BLACK)
#define A_BAR       TUI_ATTR(TUI_LIGHTGRAY, TUI_BLACK)
#define A_PRODUCT   TUI_ATTR(TUI_LIGHTCYAN, TUI_BLACK)
#define A_CODE      TUI_ATTR(TUI_WHITE, TUI_BLACK)
#define A_HINT_KEY  TUI_ATTR(TUI_LIGHTCYAN, TUI_BLACK)
#define A_BORDER    TUI_ATTR(TUI_CYAN, TUI_BLACK)
#define A_PTITLE    TUI_ATTR(TUI_WHITE, TUI_BLACK)
#define A_BODY      TUI_ATTR(TUI_LIGHTGRAY, TUI_BLACK)
#define A_DIM       TUI_ATTR(TUI_DARKGRAY, TUI_BLACK)
#define A_EMPH      TUI_ATTR(TUI_WHITE, TUI_BLACK)
#define A_OK        TUI_ATTR(TUI_LIGHTGREEN, TUI_BLACK)
#define A_SPIN      TUI_ATTR(TUI_LIGHTCYAN, TUI_BLACK)
#define A_COUNT     TUI_ATTR(TUI_YELLOW, TUI_BLACK)
#define A_SEL       TUI_ATTR(TUI_BLACK, TUI_CYAN)
#define A_CAUTION   TUI_ATTR(TUI_YELLOW, TUI_BLACK)
#define A_CAUTION_SEL TUI_ATTR(TUI_BLACK, TUI_BROWN)
#define A_SIG       TUI_ATTR(TUI_CYAN, TUI_BLACK)
#define A_FIELD     TUI_ATTR(TUI_WHITE, TUI_BLACK)

/* A dialog's colours, by kind and style. */
#define A_INFO        TUI_ATTR(TUI_BLACK, TUI_LIGHTGRAY)
#define A_INFO_DIM    TUI_ATTR(TUI_DARKGRAY, TUI_LIGHTGRAY)
#define A_INFO_EMPH   TUI_ATTR(TUI_BLUE, TUI_LIGHTGRAY)
#define A_FAIL        TUI_ATTR(TUI_WHITE, TUI_RED)
#define A_FAIL_DIM    TUI_ATTR(TUI_LIGHTGRAY, TUI_RED)
#define A_FAIL_EMPH   TUI_ATTR(TUI_YELLOW, TUI_RED)

typedef struct {
    const tui_display *d;
    size_t    cols;
    size_t    rows;
    tui_cell *cells;
    tui_cell *shown;
    tui_cell *keep;
    int       keep_valid;      /* keep holds the last list */
    uint8_t   dirty[TUI_ROWS_MAX];

    const uint16_t *title;
    const uint16_t *state;
    uint8_t         state_attr;
    const uint16_t *code;
    uint16_t        code_buf[8];
    const tui_hint *hints;
    size_t          nhints;

    int  surface;
    int  style;

    /* The page: its panel, and where the next line of its body goes. */
    size_t pl, pr, pt, pb;
    size_t body_row;
    size_t body_col;
    int    step_open;
    int    step_shown;         /* the step's line is still on the page */
    size_t step_row;
    uint16_t step[STEP_MAX];
    unsigned spin;
    uint32_t spin_at;

    /* The startup box. */
    uint16_t busy[BUSY_MAX][STEP_MAX];
    size_t   nbusy;

    /* The dialog: its text, each unit's style, and its kind. */
    int      dkind;
    uint16_t dtitle[STEP_MAX];
    uint16_t dtext[DLG_TEXT_MAX];
    uint8_t  dstyle[DLG_TEXT_MAX];
    size_t   dlen;

    /* The transcript's line in progress. */
    uint16_t logline[LOG_MAX];
    size_t   loglen;
} tui_state;

extern tui_state g_tui;

/* ---- screen.c: the buffer and its primitives ---- */

/* The cell at (row, col), or NULL off the screen. */
tui_cell *cell_at(size_t row, size_t col);

void put_n(size_t row, size_t col, const uint16_t *s, size_t n, uint8_t a);
size_t put_s(size_t row, size_t col, const uint16_t *s, uint8_t a);
size_t put_a(size_t row, size_t col, const char *s, uint8_t a);
void fill(size_t row, size_t col, size_t h, size_t w, uint16_t ch,
          uint8_t a);
void box(size_t row, size_t col, size_t h, size_t w, int style, uint8_t a,
         uint8_t fill_a);
/* " title " on a box's top border at col + 2, clipped to the box. */
void box_title(size_t row, size_t col, size_t w, const uint16_t *title,
               uint8_t a);
/* Text set into a border row from `at`, " text ", clipped before `end`. */
size_t border_text(size_t row, size_t at, size_t end, const uint16_t *s,
                   uint8_t a);
void shadow(size_t row, size_t col, size_t h, size_t w);
void mark(size_t row);
void mark_all(void);

/* The layers under every surface: the backdrop, and the chrome with the
 * hints given. */
void backdrop(void);
void chrome(const tui_hint *hints, size_t n);
void draw_chrome(void);

/* The area panels sit in: columns [*l, *r], and the panel's top and bottom
 * rows. */
void area(size_t *l, size_t *r);
size_t panel_top(void);
size_t panel_bottom(void);

size_t len16(const uint16_t *s);

/* ---- the transcript ---- */
void log_unit(uint16_t c);
void log_end(void);
void log_line(const uint16_t *a, const uint16_t *b);

/* ---- page.c ---- */
void dialog_render(void);
uint8_t page_attr(int style);
uint8_t dialog_attr(int kind, int style);
void page_close_step(const uint16_t *detail);

/* ---- list.c: the status box, shared with the passphrase dialog ---- */
int status_same(const tui_status *a, const tui_status *b);

/* The next line of s from `at` that fits w columns, broken at a space when
 * one is in reach: its length in *len, and where the line after it starts
 * (the end of s when there is none). */
size_t wrap_next(const uint16_t *s, size_t at, size_t w, size_t *len);

#endif /* FALCON_TUI_INTERNAL_H */
