/* falcon-boot: how a loader screen is laid out, shared by both loaders.
 *
 * Every screen either loader draws sits in one content block centred on the
 * console: a menu (banner, list, key hint) or a page of text started a third
 * of the way down. This module is that geometry and the composition of a
 * row's cells, and nothing else. It never draws: the UEFI loader emits a
 * composed row through Simple Text Output (falcon-efi/src/console/menu.c),
 * falcon-bios writes it into VGA text memory (falcon-bios/src/console/screen.c), and
 * both hand this module the same numbers, so both screens come out the same
 * cell for cell. Before it existed the two loaders drew the same menu twice,
 * by hand, and the BIOS one drifted.
 *
 * Text is UCS-2 in uint16_t, the unit the survey's labels and EFI's CHAR16
 * already are. A composed row is exactly `width` cells plus a terminator, in
 * a caller buffer of TUI_ROW_MAX units (tui_frame_init never makes a block
 * wider than TUI_ROW_MAX - 1).
 */

#ifndef FALCON_TUI_H
#define FALCON_TUI_H

#include <stddef.h>
#include <stdint.h>

/* The frame's rules. MARGIN_X and MARGIN_Y are the floor on how close the
 * block may come to an edge; WIDTH_MAX caps its width because a 160-column
 * row of entries is harder to scan than a 100-column one. CHROME_ROWS is a
 * menu's banner, the blank under it, the blank over the hint, and the hint.
 *
 * On a console of NARROW_COLS or fewer the margin is MARGIN_X_NARROW
 * instead. Six columns a side is a border on a 100-column console and a
 * fifth of an 80-column one, which is every BIOS screen: there it left 64
 * columns of text, and the personal OS's retry prompt, 80 characters with
 * its TAB hint, did not fit a line. */
/* Seconds a page waits for a key before the machine is powered off, on
 * every loader (DESIGN Section 7.9); the station's STATION.INI may change
 * it (`poweroff`). */
#define TUI_IDLE_SECONDS    300u

/* Wrong passphrases a boot takes, counted across every prompt, before the
 * machine is powered off (DESIGN Section 5.2); after each of the others the
 * next attempt waits tui_unlock_delay seconds. Both loaders, both builds. */
#define TUI_UNLOCK_TRIES    5u

#define TUI_MARGIN_X        6u
#define TUI_MARGIN_X_NARROW 1u
#define TUI_NARROW_COLS     80u
#define TUI_MARGIN_Y        1u
#define TUI_WIDTH_MAX   100u
#define TUI_CHROME_ROWS 4u

/* Blank cells at each end of every row. Rows are padded to the block's full
 * width so the selection bar covers them; without this the bar would start
 * on the first glyph. Banner and hint carry it too, so all three share one
 * text column. */
#define TUI_PAD_X 2u

/* How far the list steps in from the block's edge, selection bar and all.
 * Banner and hint are chrome and stay at the edge. */
#define TUI_LIST_INDENT 2u

/* A name this narrow is where aligned tag columns stop being worth it. */
#define TUI_NAME_MIN 12u

/* The widest row composed. A wider console loses its rightmost columns
 * rather than its composition. */
#define TUI_ROW_MAX 512u

/* Longer than any route code, so a copy is never cut short. */
#define TUI_MODE_CODE_MAX 7u

/* Colours, as a text attribute byte: foreground in the low nibble, background
 * in the high one. That is the VGA attribute byte and it is also EFI's
 * EFI_TEXT_ATTR, with the same sixteen colours in the same order, which is
 * why one table serves both loaders. */
#define TUI_BLACK      0x0u
#define TUI_LIGHTGRAY  0x7u
#define TUI_BROWN      0x6u
#define TUI_LIGHTCYAN  0xBu
#define TUI_YELLOW     0xEu
#define TUI_WHITE      0xFu

#define TUI_ATTR(fg, bg) ((uint8_t)((fg) | ((bg) << 4)))

/* Ordinary text, the banner's title, and the route code. */
#define TUI_ATTR_TEXT  TUI_ATTR(TUI_LIGHTGRAY, TUI_BLACK)
#define TUI_ATTR_TITLE TUI_ATTR(TUI_LIGHTCYAN, TUI_BLACK)
#define TUI_ATTR_MODE  TUI_ATTR(TUI_WHITE, TUI_BLACK)

/* A list row's colours. Caution is the whole row, not the tag. Selected, the
 * bar itself turns: yellow text on the light-grey bar is unreadable, and
 * black on brown (which firmware draws as dark yellow) is the same warning in
 * a form that still reads. */
uint8_t tui_entry_attr(int caution, int selected);

/* The block, fixed for a run from the console's size.
 *
 * left/width are the block. text_col is where a line's first glyph goes,
 * which is the block plus TUI_PAD_X; text_width is how many glyphs a line
 * holds before it wraps. rows is the console height, kept so a menu can
 * centre itself in it. */
typedef struct {
    size_t left;
    size_t width;
    size_t text_col;
    size_t text_width;
    size_t rows;
} tui_frame;

/* Fixes the frame for a console of cols x rows. A size under 40x12 is taken
 * to be nonsense and replaced with 80x25, the one mode every console has;
 * columns past TUI_ROW_MAX are ignored. */
void tui_frame_init(tui_frame *f, size_t cols, size_t rows);

/* The row a page of text starts on: a third of the way down, which puts a
 * short screen at about the height a menu block sits at, while still leaving
 * two thirds below for a long one to run into. */
size_t tui_page_row(const tui_frame *f);

/* Where a route code of `len` cells goes: the block's top right, one row
 * above a page's first line so a long label on that line cannot run into it.
 * 0 when it does not fit, and the caller draws nothing. */
int tui_mode_at(const tui_frame *f, size_t len, size_t *row, size_t *col);

/* A menu resolved into the block: where the logo, the banner, the list
 * window and the hint go for `count` entries. The block's height is how many
 * entries there are; a longer list than the screen holds fills it and
 * scrolls, and has no logo. */
/* The logo drawn over every menu's banner: the name in figlet's slant
 * lettering, TUI_LOGO_ROWS lines, none wider than TUI_LOGO_WIDTH, drawn in
 * the title's colour at the banner's text column with a blank row under it.
 * A menu shows it only when its whole list still fits beside it; a list the
 * screen cannot hold with it gets the rows instead, since the list is what
 * the screen is for. */
#define TUI_LOGO_ROWS  5u
#define TUI_LOGO_WIDTH 31u
extern const uint16_t *const tui_logo[TUI_LOGO_ROWS];

typedef struct {
    size_t left;        /* the block */
    size_t width;
    size_t list_left;   /* the block, stepped in by TUI_LIST_INDENT */
    size_t list_width;
    size_t logo_row;    /* the logo's first row */
    size_t logo_rows;   /* TUI_LOGO_ROWS, or 0 when it is left out */
    size_t banner_row;
    size_t status_row;  /* first status line, under the banner */
    size_t status_rows; /* how many; 0 for every menu but the station's */
    size_t list_top;    /* first list row */
    size_t win;         /* list rows */
    size_t hint_row;
} tui_menu;

void tui_menu_layout(const tui_frame *f, size_t count, tui_menu *m);

/* The same with `status` lines directly under the banner, before the blank
 * that precedes the list: the UEFI station's TOTP code (DESIGN Section
 * 13.15). At most TUI_STATUS_ROWS; they come out of the list's rows when
 * the screen is full. tui_menu_layout is this with none. */
#define TUI_STATUS_ROWS 2u
void tui_menu_layout_status(const tui_frame *f, size_t count,
                            size_t status, tui_menu *m);

/* Keeps the selection inside the list window. Returns nonzero when *top
 * moved, which is the one case that repaints every row of the window rather
 * than the two the selection left and reached. */
int tui_menu_scroll(size_t *top, size_t sel, size_t win);

/* Composes a row of plain text: inset by TUI_PAD_X, clipped rather than
 * wrapped so a long path cannot shift every row under it, and padded to w
 * cells. s may be NULL for a blank row. */
void tui_row_text(uint16_t *row, size_t w, const uint16_t *s);

/* Composes one list row: the name, then the tag columns (DESIGN Section
 * 7.8). sig_w and why_w are the widest sig and why on this screen, so every
 * row's tags start at the same column and read as columns. The NAME is what
 * gets cut, never a tag: a clipped name is still recognisable, a clipped tag
 * misstates what was found. When aligning would squeeze the name below
 * TUI_NAME_MIN each row places its own tags at the right edge, and only on a
 * console too narrow for that does the name win and the tags go. */
void tui_row_entry(uint16_t *row, size_t w, const uint16_t *label,
                   const uint16_t *sig, const uint16_t *why,
                   size_t sig_w, size_t why_w);

/* Composes the banner: the title, then two spaces and the state. The state
 * is drawn in its own colour, so the row is returned in three runs: the
 * title is [0, *state_at), the state [*state_at, *state_end) and the padding
 * [*state_end, w). With no state both are where the title ends. When both do
 * not fit the title gives way: the state is the part a user needs (DESIGN
 * Section 7.4 wants it unmissable), and a clipped version string loses
 * nothing. */
void tui_row_banner(uint16_t *row, size_t w, const uint16_t *title,
                    const uint16_t *state, size_t *state_at,
                    size_t *state_end);

/* The strings a menu draws itself, so both loaders say them the same way.
 *
 * The hint's two key words stay general because what they do is the
 * caller's: enter opens a folder or boots an image on the main menu and
 * picks an image in the personal-OS chooser, and esc leaves a folder, the
 * chooser or the loader depending on where the caller is.
 *
 * tui_hint_plain is the default less the legend for the trust tags, for a
 * list whose rows never carry one: the personal-OS chooser on both loaders,
 * and every menu of the stationary build, which links no predictions. */
/* Seconds before the next attempt after `failures` wrong passphrases this
 * boot: none after the first, then 4, 8, 16. */
unsigned tui_unlock_delay(unsigned failures);

extern const uint16_t tui_hint_default[];
extern const uint16_t tui_hint_plain[];
extern const uint16_t tui_hint_empty[];
extern const uint16_t tui_list_empty[];

/* A row whose tags are still being read shows tui_tag_pending in its first
 * tag column, and the key line says how far the menu has got through the
 * rows on screen: "checking k of n | esc back", written into dst (cap
 * units, terminated, clipped). Both loaders fill a page this way, a row at a
 * time with keys read between rows, so the page is drawn before any row is
 * looked inside. */
extern const uint16_t tui_tag_pending[];
void tui_hint_checking(uint16_t *dst, size_t cap, size_t k, size_t n);

/* What a loader is doing while nothing else is on screen: startup, before
 * its menu, and a row being looked inside after Enter. Drawn as one line at
 * tui_page_row, "<step>...", each step overwriting the last (menu_busy,
 * screen_busy), so a slow machine shows where the time goes. tui_busy_text
 * composes the line into dst (cap units, terminated, clipped). */
extern const uint16_t tui_busy_disks[];
extern const uint16_t tui_busy_keys[];
extern const uint16_t tui_busy_shelf[];
extern const uint16_t tui_busy_library[];
extern const uint16_t tui_busy_tpm[];
extern const uint16_t tui_busy_entries[];
extern const uint16_t tui_busy_floppy[];
extern const uint16_t tui_busy_image[];
void tui_busy_text(uint16_t *dst, size_t cap, const uint16_t *step);

#endif /* FALCON_TUI_H */
