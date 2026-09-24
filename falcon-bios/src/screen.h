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

typedef struct {
    const uint16_t *title;
    const uint16_t *state;     /* NULL to draw the title alone */
    uint8_t         state_attr;
} screen_banner;

/* Blocks until the user commits, and returns the selected row, or -1 for
 * escape. `keys` lists extra commit keys in lower case (either case is
 * accepted) and `hint` replaces the key line; *which receives '\r' for enter
 * or the key. keys, hint and which may be NULL. The rows carry no tag
 * columns: a BIOS machine has no signature for them to predict. */
int screen_menu(const screen_banner *banner, const uint16_t *const *labels,
                size_t count, const char *keys, const uint16_t *hint,
                int *which);

/* Blocks for a key, as bios_getkey. */
int screen_key(void);

/* Shows or hides the hardware cursor. */
void screen_cursor(int on);

#endif /* FALCON_BIOS_SCREEN_H */
