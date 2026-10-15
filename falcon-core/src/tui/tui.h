/* falcon-boot: the screens, shared by every loader (DESIGN Section 7.10).
 *
 * Core owns what a screen is: its layers, its layout and its sentences. A
 * loader owns only a display (tui_display): a way to put a run of cells on
 * the screen, read a key, and tell the time. The UEFI loader's is Simple
 * Text Output and Input (falcon-efi/src/console/display.c), falcon-bios's
 * is VGA text memory and INT 16h (falcon-bios/src/console/display.c), and
 * the host tests' is a fake that records cells (falcon-core/tests).
 *
 * The screen is retained: every call draws into a cell buffer, rows it
 * changed are marked, and tui_flush sends only those rows, as runs of one
 * attribute. Nothing outside this module writes to the display while a
 * screen is up, so the buffer is the screen, and a full repaint after a
 * payload had the display is a matter of marking every row.
 *
 * Layers, bottom to top: the backdrop (light shade in dark grey), the
 * chrome (the title bar on the first row, the key hints on the last), a
 * panel (a bordered, titled box: a list, a page of steps), and a dialog (a
 * double-bordered box with a drop shadow: a failure, a question, the
 * passphrase prompt). docs/tui-mockup.html is the look.
 *
 * Text is UCS-2 in uint16_t, the unit EFI's CHAR16 and the survey's labels
 * already are. Glyphs outside ASCII are the ones UEFI requires every console
 * to draw and code page 437 holds: box drawing, light shade, full block and
 * the two triangles.
 */

#ifndef FALCON_TUI_H
#define FALCON_TUI_H

#include <stddef.h>
#include <stdint.h>

/* Seconds a page waits for a key before the machine is powered off, on
 * every loader (DESIGN Section 7.9); the station's STATION.INI may change
 * it (`poweroff`). */
#define TUI_IDLE_SECONDS    300u

/* Wrong passphrases a boot takes, counted across every prompt, before the
 * machine is powered off (DESIGN Section 5.2); after each of the others the
 * next attempt waits tui_unlock_delay seconds. */
#define TUI_UNLOCK_TRIES    5u

/* Seconds before the next attempt after `failures` wrong passphrases this
 * boot: none after the first, then 4, 8, 16. */
unsigned tui_unlock_delay(unsigned failures);

/* The largest screen the buffer is sized for. A display reporting more is
 * drawn in its top left corner at this size. */
#define TUI_COLS_MAX 240u
#define TUI_ROWS_MAX 80u

/* ---- colours ----
 *
 * A text attribute byte: foreground in the low nibble, background in the
 * high one. That is the VGA attribute byte and EFI's EFI_TEXT_ATTR, with
 * the same sixteen colours in the same order. UEFI takes only the first
 * eight as a background, so no attribute here has a background above 7. */
#define TUI_BLACK        0x0u
#define TUI_BLUE         0x1u
#define TUI_GREEN        0x2u
#define TUI_CYAN         0x3u
#define TUI_RED          0x4u
#define TUI_MAGENTA      0x5u
#define TUI_BROWN        0x6u
#define TUI_LIGHTGRAY    0x7u
#define TUI_DARKGRAY     0x8u
#define TUI_LIGHTBLUE    0x9u
#define TUI_LIGHTGREEN   0xAu
#define TUI_LIGHTCYAN    0xBu
#define TUI_LIGHTRED     0xCu
#define TUI_LIGHTMAGENTA 0xDu
#define TUI_YELLOW       0xEu
#define TUI_WHITE        0xFu

#define TUI_ATTR(fg, bg) ((uint8_t)((fg) | ((bg) << 4)))

/* Ordinary text on black: what a display is left in, and what a payload
 * finds. */
#define TUI_ATTR_TEXT  TUI_ATTR(TUI_LIGHTGRAY, TUI_BLACK)

/* The colours a caller hands in: the title bar's state segment. */
#define TUI_ATTR_STATE TUI_ATTR(TUI_LIGHTGRAY, TUI_BLACK)
#define TUI_ATTR_ALERT TUI_ATTR(TUI_YELLOW, TUI_RED)

/* ---- glyphs ---- */

#define TUI_HLINE   0x2500u   /* box drawing, light horizontal */
#define TUI_VLINE   0x2502u
#define TUI_SHADE   0x2591u   /* light shade: the backdrop */
#define TUI_BLOCK   0x2588u   /* full block: the scroll bar's thumb */
#define TUI_UP      0x25B2u   /* black up-pointing triangle */
#define TUI_DOWN    0x25BCu   /* black down-pointing triangle */

/* A glyph's byte in code page 437, the VGA text font: ASCII as itself, the
 * glyphs above as the font holds them, anything else as fallback. */
uint8_t tui_cp437(uint16_t c, uint8_t fallback);

/* A glyph as plain ASCII, for a serial line read by people and scripts:
 * corners as '+', lines as '-', '=' and '|', the shade as a space, the
 * block as '#', the triangles as '^' and 'v'. */
char tui_ascii(uint16_t c, char fallback);

/* The spinner's glyph for `phase`, counting up: / - \ | with the dash the
 * box-drawing line, which fills its cell as '-' does not. */
uint16_t tui_spin(unsigned phase);

/* ---- keys ----
 *
 * What tui_display.key returns. Printable ASCII is itself; these are the
 * rest, and every other key a console can deliver is TUI_KEY_OTHER. */
#define TUI_KEY_NONE      0       /* nothing in the time given */
#define TUI_KEY_BACKSPACE 0x08
#define TUI_KEY_TAB       0x09
#define TUI_KEY_ENTER     0x0D
#define TUI_KEY_ESC       0x1B
#define TUI_KEY_UP        0x100
#define TUI_KEY_DOWN      0x101
#define TUI_KEY_PGUP      0x102
#define TUI_KEY_PGDN      0x103
#define TUI_KEY_HOME      0x104
#define TUI_KEY_END       0x105
#define TUI_KEY_OTHER     0x1FF
#define TUI_KEY_IDLE      (-2)    /* the idle time ran out (DESIGN 7.9) */
#define TUI_KEY_GONE      (-3)    /* there is no keyboard to wait on */

/* tui_display.key's wait with no end but a key or the idle time. */
#define TUI_FOREVER 0xFFFFFFFFu

/* ---- the display: what a loader provides ---- */

typedef struct {
    void *ctx;

    /* n cells at (row, col), all in attr. The glyphs are UCS-2 as above.
     * Never asked for the bottom right cell, which on a console scrolls
     * the screen when written. */
    void (*put)(void *ctx, size_t row, size_t col, const uint16_t *s,
                size_t n, uint8_t attr);

    /* The hardware cursor at (row, col), shown or hidden. */
    void (*cursor)(void *ctx, int on, size_t row, size_t col);

    /* The next key, waiting at most `tenths` tenths of a second (0 polls,
     * TUI_FOREVER waits for a key or the idle time). TUI_KEY_IDLE when the
     * idle time ran out first: the caller wipes what it holds and calls
     * idle_off. Each key read restarts the idle time. */
    int (*key)(void *ctx, uint32_t tenths);

    /* Tenths of a second from some fixed point, never going back. */
    uint32_t (*clock)(void *ctx);

    /* Restarts the idle time: a page starting to wait, a countdown's
     * second. */
    void (*touch)(void *ctx);

    /* The idle time is up: the loader's hook, then the power-off. Returns
     * when the machine stayed on, with the idle time started again. */
    void (*idle_off)(void *ctx);

    /* Off now: the last wrong passphrase. Does not return. */
    void (*power_off)(void *ctx);

    /* Takes the display back after a payload had it: falcon-bios sets 80x25
     * text again if the mode changed. NULL when there is nothing to do. */
    void (*reclaim)(void *ctx);

    /* A plain line for the serial transcript, without its line break. NULL
     * when there is no serial port to send it to. */
    void (*log)(void *ctx, const uint16_t *line);

    /* A wait on the user starts (1) or ends (0): the UEFI loader counts the
     * time spent waiting out of its boot timing. NULL when unused. */
    void (*wait)(void *ctx, int begin);
} tui_display;

/* One cell of the screen buffer. */
typedef struct {
    uint16_t ch;
    uint8_t  attr;
} tui_cell;

/* Starts the screens on a display of cols x rows, with three buffers of
 * `cap` cells each: the screen as drawn, the screen as the display last
 * showed it (so a flush sends only what changed), and the copy of the last
 * menu a dialog is drawn over. A size under 80x25 is taken as 80x25.
 * Clears the screen to the backdrop. Once, before anything else here. */
void tui_init(const tui_display *d, size_t cols, size_t rows,
              tui_cell *screen, tui_cell *shown, tui_cell *keep, size_t cap);

/* The title bar: `title` at its left, its first word in the title colour
 * and the rest in grey, and `state` at its right in `state_attr` (the
 * Secure Boot state, DESIGN Section 7.4). When both do not fit, the title
 * gives way. Kept for every screen after it. */
void tui_title(const uint16_t *title, const uint16_t *state,
               uint8_t state_attr);

/* The route a boot is taking, as a short code (docs/Manual.md, "Route
 * codes"), drawn in the title bar after the title from now on. NULL stops
 * drawing it. A literal: the pointer is kept. */
void tui_code(const uint16_t *code);

/* The same from an ASCII code, copied: falcon-bios's route codes. */
void tui_code_ascii(const char *code);

/* A key and what it does, for the hint bar. */
typedef struct {
    const uint16_t *key;
    const uint16_t *word;
} tui_hint;

/* Sends the rows changed since the last flush. Every call below that ends
 * in a wait flushes; a caller drawing a page between waits flushes when it
 * wants the page seen. */
void tui_flush(void);

/* Marks every row changed and asks the display to take itself back
 * (tui_display.reclaim): after a payload drew on the screen. */
void tui_repaint(void);

/* ---- startup ----
 *
 * What a loader is doing between its first screen and its menu: a small
 * box, one line per step, each finished step marked OK as the next one
 * starts. The strings are the steps every loader names (DESIGN Section
 * 7.8, "Startup steps"). */
void tui_busy(const uint16_t *step);

extern const uint16_t tui_busy_disks[];
extern const uint16_t tui_busy_keys[];
extern const uint16_t tui_busy_shelf[];
extern const uint16_t tui_busy_library[];
extern const uint16_t tui_busy_tpm[];
extern const uint16_t tui_busy_entries[];
extern const uint16_t tui_busy_floppy[];
extern const uint16_t tui_busy_image[];

/* ---- text ----
 *
 * Text goes to whatever surface is up: a page's body, or the dialog being
 * built. A line break starts a new line ('\r' is ignored, so CR LF strings
 * work), and a line too long for the surface wraps. Every finished line is
 * also sent to the serial transcript (tui_display.log). */
void tui_out(const uint16_t *s);
void tui_out_ascii(const char *s);
void tui_out_dec(uint64_t v);
void tui_out_hex(uint64_t v);          /* 0x-prefixed, lower case */

/* How the text after it is drawn, until the next call: plain, dim (a
 * detail a reader may skip), or emphasised. Each surface starts plain. */
#define TUI_PLAIN 0
#define TUI_DIM   1
#define TUI_EMPH  2
void tui_style(int style);

/* A size as a person reads it: "512 B", "14.2 MB", "3.9 GB" (decimal
 * units, one decimal place under 100), into dst of cap units. */
void tui_size(uint16_t *dst, size_t cap, uint64_t bytes);

/* ---- a page: what a route says while it runs ----
 *
 * A panel the full height of the screen, titled, with `code` in the title
 * bar (tui_code; NULL keeps the code already there). Its body is steps and
 * lines of text. */
void tui_page(const uint16_t *title, const uint16_t *code);

/* "booting: <label>", the page every route starts with. */
void tui_boot_page(const uint16_t *label, const uint16_t *code);

/* A step starts: its line is drawn in white with the spinner in its status
 * column, and the step before it, if still open, is marked OK. The stage a
 * failure names is the step that was open (tui_stage). */
void tui_step(const uint16_t *what);
void tui_step_ascii(const char *what);

/* The open step is finished: `detail` in its status column (a size), or
 * OK when NULL. */
void tui_step_done(const uint16_t *detail);

/* The open step, or NULL: what a failure says it failed while doing. */
const uint16_t *tui_stage(void);

/* The open step stays open and a dim, indented line goes under it: a
 * device name found, a detail of what the step read. */
void tui_note(const uint16_t *s);
void tui_note_ascii(const char *s);

/* A slow step is still moving: turns the spinner when a tenth of a second
 * has passed since it last turned, and flushes it. Cheap enough to call
 * from a read loop. */
void tui_tick(void);

/* The payload takes the display from here (DESIGN Section 7.10, "The
 * hand-off"): the open step is marked OK, the rows under the page are
 * cleared to plain text, and the cursor is left on the first of them,
 * shown, so what the payload writes lands under the page. */
void tui_handoff(void);

/* ---- a dialog: what a failure, a question or a notice says ---- */

#define TUI_INFO 0     /* grey: a notice, a question */
#define TUI_FAIL 1     /* red: a route failed, the platform refused */

/* Starts a dialog of `kind` titled `title`, drawn over the last menu (or
 * over the screen as it is when no menu has been up), with `label`, when
 * not NULL, as its first line. Text follows with tui_out. */
void tui_report(int kind, const uint16_t *title, const uint16_t *label);

/* The sentences every route's report is made of, said one way:
 *
 *   tui_report_failed: "<what> failed while <stage>: <status>", the
 *     status left out when !with_status (falcon-bios has none).
 *   tui_report_exited: "<what> exited, status <status>".
 *   tui_report_advice: a blank line, then the advice, dim.
 *   tui_report_end:    a blank line, then `keys` emphasised, or "press any
 *     key to return to the menu." when NULL; the dialog is flushed.
 *     tui_wait then waits. */
void tui_report_failed(const uint16_t *what, const uint16_t *stage,
                       uint64_t status, int with_status);
/* The same with falcon-bios's ASCII strings and no status. */
void tui_report_failed_a(const char *what, const char *stage);
void tui_report_exited(const uint16_t *what, uint64_t status);
void tui_report_advice(const uint16_t *advice);
void tui_report_end(const uint16_t *keys);

extern const uint16_t tui_press_any[];

/* A question: a dialog of `text` and the `keys` line under it, and the key
 * pressed, in lower case. TUI_KEY_GONE when there is no keyboard, which a
 * caller takes as "no". */
int tui_ask(const uint16_t *title, const uint16_t *text,
            const uint16_t *keys);

/* Waits for any key, as a "press any key" page does: the idle time runs,
 * and the wait counts as one on the user. The key, or TUI_KEY_GONE. */
int tui_wait(void);

/* Waits `seconds`, then throws away the keys pressed meanwhile: the pause
 * after a wrong passphrase. */
void tui_pause(unsigned seconds);

/* Off now, with the secret tui_secret is holding wiped first: the
 * passphrase prompt's last wrong attempt (DESIGN Section 5.2). */
__attribute__((noreturn)) void tui_power_off(void);

/* ---- what every loader says (say.c) ----
 *
 * The screens both loaders show with the same words, so the words are
 * written once. The _a forms take falcon-bios's ASCII strings. Each ends in
 * a wait for a key unless it says otherwise. */

/* A route came back failed: a red dialog titled "<route> failed", the
 * label, "<route> failed while <stage>[: <status>]" and the advice. Does
 * not wait: the caller's menu loop does. */
void tui_route_failed(const uint16_t *label, const uint16_t *route,
                      const uint16_t *stage, uint64_t status,
                      int with_status, const uint16_t *advice);
void tui_route_failed_a(const uint16_t *label, const char *route,
                        const char *stage, const char *advice);

/* A route's image came back without failing: a grey dialog, the label,
 * "<what> exited, status <status>" and the advice. Does not wait. */
void tui_route_exited(const uint16_t *label, const uint16_t *what,
                      uint64_t status, const uint16_t *advice);

/* "Reboot into this machine's firmware setup?": nonzero for y. And the
 * dialog when the firmware refused the request. */
int tui_fw_setup_ask(void);
void tui_fw_setup_refused(void);

/* A notice before the menu, and "press any key to continue." */
void tui_notice(const uint16_t *title, const uint16_t *text);
void tui_notice_a(const uint16_t *title, const char *text);

/* STATION.INI could not be taken (DESIGN Section 12.7): unread or too big,
 * a line it stopped at, or a default naming no entry here; each followed by
 * "The menu will wait with no default." tui_ini_line names its file, which
 * is falcon jump's JUMP.INI too (Section 13.7). */
void tui_ini_unread(int too_big);
void tui_ini_line(const char *file, unsigned line, const char *why);
void tui_ini_default(const char *name);

/* The next-boot marker could not be acted on (DESIGN Section 12.8): a name
 * no entry has, or the state the marker was in (`note`); `cleared` adds
 * that nothing was booted from it. */
void tui_next_unknown(const char *name);
void tui_next_note(const char *note, int cleared);

/* Escape on the last menu: the loader hands the machine back. `nothing`
 * adds that no personal OS and no other entry was found. Starts the page
 * and returns; the caller adds its notes, then tui_handoff, then leaves. */
void tui_leaving(int nothing);

/* ---- a list: the menus ---- */

/* A row: its name and two optional tag columns drawn right of it in aligned
 * columns (DESIGN Section 7.8). `sig` says who signed what the entry boots;
 * `why` says why the platform is expected to refuse it, set only with
 * `caution`, which draws the row in the caution colours. `probed` is the
 * list's: it sets it when fill says the row is final, and a caller with a
 * probe clears it on rows it has not looked inside. */
typedef struct {
    const uint16_t *label;
    const uint16_t *sig;
    const uint16_t *why;
    uint8_t         caution;
    uint8_t         probed;
} tui_row;

/* Lines a caller keeps current beside the list, in a box of their own:
 * the UEFI station's TOTP code (DESIGN Section 12.15). Asked for when the
 * list opens and every second after; the box is redrawn when a line
 * changed. A line longer than the box is wide wraps at its spaces. */
#define TUI_STATUS_ROWS 3u
#define TUI_STATUS_MAX  96u

typedef struct {
    uint16_t line[TUI_STATUS_ROWS][TUI_STATUS_MAX];
    uint8_t  attr[TUI_STATUS_ROWS];
    size_t   n;
} tui_status;

typedef struct {
    const uint16_t *title;       /* on the panel's top border */
    tui_row        *rows;
    size_t          count;
    size_t          sel;         /* highlighted first */
    const uint16_t *empty;       /* the line an empty list shows */
    const char     *keys;        /* commit keys beyond enter, lower case */
    const tui_hint *hints;
    size_t          nhints;

    /* A countdown: when it runs out, `sel` is taken as if enter had been
     * pressed; any key stops it for good. 0 for none. */
    unsigned        seconds;

    /* The background probe (DESIGN Section 7.8): step runs a slice of it
     * with `sel` the row highlighted and returns nonzero while there is
     * more; fill brings row idx up to date and returns nonzero once it is
     * final. Both NULL for a list whose rows are complete. */
    int  (*step)(void *ctx, size_t sel);
    int  (*fill)(void *ctx, size_t idx);

    /* What the highlighted row is, on the panel's bottom border; and a line
     * under the panel. Either NULL. */
    void (*detail)(void *ctx, size_t idx, uint16_t *buf, size_t cap);
    void (*note)(void *ctx, uint16_t *buf, size_t cap);

    /* A line under the panel in the alert colours, in place of the note:
     * shim's validation off (tui_validation_off). NULL for none. */
    const uint16_t *alert;

    /* The status box (tui_status), and its title. NULL for none. */
    void (*status)(void *ctx, tui_status *out);
    const uint16_t *status_title;

    void *ctx;
} tui_list;

/* Blocks until the user commits, and returns the index of the row
 * committed, or -1 for escape (and when there is no keyboard). *which, when
 * not NULL, receives TUI_KEY_ENTER or the commit key in lower case. */
int tui_list_run(const tui_list *l, int *which);

/* The hints most lists show. */
extern const uint16_t tui_list_empty[];

/* The alert a menu shows when shim's validation is off (DESIGN Section
 * 7.4). */
extern const uint16_t tui_validation_off[];
extern const uint16_t tui_tag_pending[];

/* ---- the passphrase ----
 *
 * A masked line editor in a dialog (DESIGN Section 5). The result is bytes:
 * a LUKS passphrase is a byte string, and only printable ASCII is taken,
 * which has one encoding everywhere. TAB clears the echo and silences the
 * rest of the entry (the systemd ask-password convention). Returns the
 * length typed, or -1 on escape, with the buffer wiped; on success the
 * caller owns the wipe. buf always ends NUL-terminated within cap.
 *
 * Its own translation unit (secret.c): what has held a passphrase has a
 * one-file answer. */
typedef struct {
    const uint16_t *title;       /* "Unlock the shelf" */
    const uint16_t *prompt;      /* the line above the field */
    const uint16_t *status;      /* the line under it ("wrong passphrase."), or NULL */
    /* Lines shown under the field and refreshed every second: the
     * station's TOTP code. NULL for none. */
    void (*lines)(void *ctx, tui_status *out);
    void *ctx;
} tui_secret_spec;

long tui_secret(const tui_secret_spec *s, char *buf, size_t cap);

#endif /* FALCON_TUI_H */
