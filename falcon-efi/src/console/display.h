/* falcon-boot: the UEFI loader's display (DESIGN Section 7.10).
 *
 * falcon-core's screens (tui/tui.h) drawn through Simple Text Output and
 * read from Simple Text Input. This module is only the display: a run of
 * cells is a SetAttribute, a SetCursorPosition and one OutputString; a key
 * is a wait on WaitForKey beside the idle timer (console/idle.h) and a
 * timer of the wait's own; the clock is a timer whose notification counts
 * tenths of a second. Every screen, sentence and layout is core's.
 */

#ifndef FALCON_EFI_DISPLAY_H
#define FALCON_EFI_DISPLAY_H

#include <efi/uefi.h>

/* Picks the console's text mode, the largest that fits the screen buffer
 * (TUI_COLS_MAX by TUI_ROWS_MAX), starts the clock and the screens, and
 * clears the console to the backdrop. Once, before anything is drawn. */
void display_init(EFI_SYSTEM_TABLE *st);

#endif /* FALCON_EFI_DISPLAY_H */
