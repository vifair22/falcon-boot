/* airies-boot: text menu over Simple Text Output/Input (DESIGN Section 9).
 * Presentation only: it renders entries and reports what the user picked;
 * acting on the selection is the caller's job.
 */

#ifndef AIRIES_MENU_H
#define AIRIES_MENU_H

#include <efi/uefi.h>

#define MENU_PATH_MAX 512u

typedef struct {
    CHAR16 label[MENU_PATH_MAX];
} menu_entry;

/* Blocks until the user commits. Returns the selected entry index, or a
 * negative value if the user backed out (ESC). */
INTN menu_run(EFI_SYSTEM_TABLE *st, const CHAR16 *banner,
              const menu_entry *entries, UINTN count);

#endif /* AIRIES_MENU_H */
