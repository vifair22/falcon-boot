/* falcon-bios: the screen, drawn the way the UEFI loader draws it.
 *
 * The same content block, menu and page as falcon-efi/src/console/menu.h,
 * from the same layout (falcon-core/src/tui), so a BIOS boot and a UEFI boot
 * of one stick look the same. The calls are that header's, one for one:
 * screen_page is menu_screen, screen_out is menu_out, screen_mode is
 * menu_mode and screen_menu is menu_run_keys.
 *
 * Stage 3 only. Everything goes into VGA text memory directly, cell and
 * colour at once, and is mirrored to COM1 as the ANSI sequences OVMF's
 * terminal driver sends for the same calls, so the rig and the coverage
 * harness read either loader's serial the same way.
 */

#ifndef FALCON_BIOS_SCREEN_H
#define FALCON_BIOS_SCREEN_H

#include <stddef.h>
#include <stdint.h>

/* 80x25 colour text, cleared, and the frame fixed for the run. Once, before
 * anything is drawn. */
void screen_init(void);

/* Start a screen: clear it and park the cursor at the block's first text
 * column, a third of the way down. */
void screen_page(void);

/* Text inside the block, re-indented after every line break and wrapped at
 * its right edge. screen_out16 takes a UCS-2 name; a character this console
 * cannot draw is shown as '?'. */
void screen_out(const char *s);
void screen_out16(const uint16_t *s);
void screen_out_dec(uint64_t v);

/* Glyphs and backspaces at the cursor: the passphrase prompt's echo. Glyphs
 * wrap as screen_out's do, and a backspace at the start of a wrapped row
 * goes back to the end of the row above. */
void screen_raw(const char *s);

/* The row the cursor is on, and that row of the block blanked with the cursor
 * at its first text column: menu_line's pair, for the passphrase prompt and
 * the status under it (DESIGN Section 5.2). */
size_t screen_row(void);
void screen_line(size_t row);

/* The route code (docs/Manual.md, "Route codes"), drawn at the block's top
 * right now and by every screen_page after it. A literal, or NULL to stop. */
void screen_mode(const char *code);

/* "<step>..." on the page's first row, replacing whatever step was there
 * (falcon-core's tui_busy_*): what the loader is doing while no menu is up.
 * Call screen_init and screen_page first. */
void screen_busy(const uint16_t *step);

typedef struct {
    const uint16_t *title;
    const uint16_t *state;     /* NULL to draw the title alone */
    uint8_t         state_attr;
} screen_banner;

/* Blocks until the user commits, and returns the selected row, or -1 for
 * escape. `keys` lists extra commit keys in lower case (either case is
 * accepted) and `hint` replaces the key line; *which receives '\r' for enter
 * or the key. keys, hint and which may be NULL. The rows carry no tag
 * columns (screen_menu_tagged draws them). */
int screen_menu(const screen_banner *banner, const uint16_t *const *labels,
                size_t count, const char *keys, const uint16_t *hint,
                int *which);

/* A row's tag columns (DESIGN Section 7.8), the UEFI menu's menu_entry
 * fields: `sig` and `why` are drawn right of the name in aligned columns,
 * either may be NULL, and `caution` draws the row in the caution colours
 * (tui_entry_attr). `probed` is the menu's: it sets it once the probe has
 * filled the row, and a caller building rows clears it. */
typedef struct {
    const uint16_t *sig;
    const uint16_t *why;
    uint8_t         caution;
    uint8_t         probed;
} screen_tags;

/* The menu's background work (DESIGN Section 7.8), the UEFI menu_probe:
 * the caller's probe, on a task of its own (task/task.h), so it outlives the
 * menu call. step runs a slice of it, until the probe next yields, with
 * `sel` the row highlighted, and returns nonzero while there is more; fill
 * copies what is known of row idx's tags onto it and returns nonzero once
 * they are final. Both NULL when the tags are complete as given. */
typedef struct {
    screen_tags *tags;    /* one per row */
    int  (*step)(void *ctx, size_t sel);
    int  (*fill)(void *ctx, size_t idx, screen_tags *tags);
    void        *ctx;
} screen_rows;

/* screen_menu with tag columns, the UEFI menu_run_probed. The page is
 * painted at once, a row whose tags are not known yet showing
 * tui_tag_pending. Whenever no key is waiting the menu steps the probe,
 * fills every pending row on screen and repaints the ones that became final
 * (the whole window when a tag column widens); a spinner in the banner's
 * last cell turns while step says there is more. A row the caller built
 * with probed set is never pending. The columns are sized to the window's
 * widest tags. rows may be NULL, which is screen_menu. */
int screen_menu_tagged(const screen_banner *banner,
                       const uint16_t *const *labels, size_t count,
                       const screen_rows *rows, const char *keys,
                       const uint16_t *hint, int *which);

/* screen_menu with `empty` as the line an empty list shows, `sel`
 * highlighted first and, when `seconds` is nonzero, a countdown in the key
 * line: when it runs out, `sel` is returned as if Enter had been pressed,
 * and any key stops it for the rest of the menu's life (falcon jump's
 * JUMP.INI, DESIGN Section 13.7). */
int screen_menu_timed(const screen_banner *banner,
                      const uint16_t *const *labels, size_t count,
                      const char *keys, const uint16_t *hint, int *which,
                      const uint16_t *empty, size_t sel, unsigned seconds);

/* Blocks for a key, as bios_getkey, and powers the machine off when none
 * comes for the idle time (DESIGN Section 7.9): the time starts with each
 * call, which is each page and each key. When the power-off does not take
 * the screen is turned off until a key, which is swallowed, and
 * BIOS_KEY_IDLE is returned. */
int screen_key(void);

/* Waits `seconds`, timed as the countdown is (ticks and RTC), then throws
 * away the keys pressed meanwhile: the wait after a wrong passphrase. */
void screen_pause(unsigned seconds);

/* Off now: what screen_idle_secret names wiped, the keyboard ring too,
 * then power/power.c, and a reset when that does not take. Does not
 * return. The passphrase prompt's last wrong attempt (DESIGN Section 5.2). */
__attribute__((noreturn)) void screen_power_off(void);

/* The idle time in seconds (TUI_IDLE_SECONDS until set; 0 never). */
void screen_idle_set(unsigned seconds);

/* A buffer wiped before the idle power-off, for as long as it is set; NULL
 * clears it. The passphrase prompt's. */
void screen_idle_secret(void *buf, size_t len);

/* Shows or hides the hardware cursor. */
void screen_cursor(int on);

#endif /* FALCON_BIOS_SCREEN_H */
