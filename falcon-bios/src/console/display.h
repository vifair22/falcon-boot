/* falcon-bios: the display (DESIGN Section 7.10).
 *
 * falcon-core's screens (tui/tui.h) drawn into VGA text memory and mirrored
 * to COM1, with the keyboard read through INT 16h. Stage 3 only. This module
 * is only the display; every screen, sentence and layout is core's.
 */

#ifndef FALCON_BIOS_DISPLAY_H
#define FALCON_BIOS_DISPLAY_H

/* 80x25 colour text, the clock started, and the screens started on it,
 * cleared to the backdrop. Once, before anything is drawn. */
void display_init(void);

/* The idle time in seconds (TUI_IDLE_SECONDS until set; 0 never): the
 * station's STATION.INI `poweroff` (DESIGN Section 7.9). */
void display_idle_set(unsigned seconds);

#endif /* FALCON_BIOS_DISPLAY_H */
