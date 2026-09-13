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

/* The banner is two segments so the Secure Boot state can be coloured apart
 * from the title (DESIGN Section 7.4 wants a relaxed-enforcement state to be
 * unmissable). The menu stays presentation-only: it renders the attribute it
 * is handed and has no idea what the state means, which keeps the module
 * free of any dependency on the sb module and, more to the point, free of
 * anything that could grow into an enforcement decision. */
typedef struct {
    const CHAR16 *title;
    const CHAR16 *state;      /* NULL to render the title alone */
    UINTN state_attr;         /* EFI_TEXT_ATTR value for the state segment */
} menu_banner;

/* Blocks until the user commits. Returns the selected entry index, or a
 * negative value if the user backed out (ESC). */
INTN menu_run(EFI_SYSTEM_TABLE *st, const menu_banner *banner,
              const menu_entry *entries, UINTN count);

#endif /* AIRIES_MENU_H */
