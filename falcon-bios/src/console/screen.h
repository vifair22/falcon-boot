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

/* Fills row idx's tags. The menu calls it once per unprobed row, the first
 * time the row is on screen, so a menu of many rows pays only for the ones
 * it shows (DESIGN Section 7.8, lazy probing). */
typedef void (*screen_probe_fn)(void *ctx, size_t idx, screen_tags *tags);

typedef struct {
    screen_tags    *tags;    /* one per row */
    screen_probe_fn probe;   /* NULL when the tags are complete as given */
    void           *ctx;
} screen_rows;

/* screen_menu with tag columns, the UEFI menu_run_probed. A window is
 * painted at once, its unprobed rows showing tui_tag_pending, and then
 * probed a row at a time top to bottom, each row repainted as it is filled
 * (the whole window when a tag column widens) and the keys read between
 * rows; the key line says "checking k of n" until the window is done. A
 * row the caller built with probed set is never probed and never pending.
 * The columns are sized to the window's widest tags. rows may be NULL,
 * which is screen_menu. */
int screen_menu_tagged(const screen_banner *banner,
                       const uint16_t *const *labels, size_t count,
                       const screen_rows *rows, const char *keys,
                       const uint16_t *hint, int *which);

/* screen_menu with `empty` as the line an empty list shows, `sel`
 * highlighted first and, when `seconds` is nonzero, a countdown in the key
 * line: when it runs out, `sel` is returned as if Enter had been pressed,
 * and any key stops it for the rest of the menu's life (falcon jump's
 * JUMP.INI, DESIGN Section 14.7). */
int screen_menu_timed(const screen_banner *banner,
                      const uint16_t *const *labels, size_t count,
                      const char *keys, const uint16_t *hint, int *which,
                      const uint16_t *empty, size_t sel, unsigned seconds);

/* Blocks for a key, as bios_getkey. */
int screen_key(void);

/* Shows or hides the hardware cursor. */
void screen_cursor(int on);

#endif /* FALCON_BIOS_SCREEN_H */
