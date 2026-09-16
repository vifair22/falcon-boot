/* falcon-boot: text menu over Simple Text Output/Input (DESIGN Section 9).
 * Presentation only: it renders entries and reports what the user picked;
 * acting on the selection is the caller's job.
 */

#ifndef FALCON_MENU_H
#define FALCON_MENU_H

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

/* Picks a console text mode. Called once, before anything is drawn.
 *
 * Separate from menu_run because SetMode clears the screen, and the loader
 * shows several screens per run. Safe to skip: the console keeps whatever the
 * firmware handed it. */
void menu_init(EFI_SYSTEM_TABLE *st);

/* Blocks until the user commits. Returns the selected entry index, or a
 * negative value if the user backed out (ESC). */
INTN menu_run(EFI_SYSTEM_TABLE *st, const menu_banner *banner,
              const menu_entry *entries, UINTN count);

/* Masked line editor, for the p3 passphrase (DESIGN Section 5 step 2).
 *
 * Lives in the menu module because it is console presentation and nothing
 * else: it knows how to echo a '*' and how to take a backspace, and it has no
 * idea what the bytes are for. What they are for, and the wiping discipline
 * that follows from it, is the caller's (src/pos).
 *
 * The result is BYTES, not CHAR16: a LUKS passphrase is a byte string, and
 * widening it to UTF-16 here only to narrow it again at the LUKS call would
 * put a second copy of the secret in a second buffer. Input is restricted to
 * printable ASCII for exactly the same reason it is on the mkstick side:
 * anything else has no single agreed byte encoding, so a passphrase typed at
 * this prompt and one typed at a Linux prompt would silently differ.
 * Non-ASCII keystrokes are ignored rather than rejected, because a refusal
 * message would be a keystroke-by-keystroke oracle for nothing.
 *
 * Tab follows the systemd ask-password convention: it erases the asterisks
 * already on screen and silences echo for the rest of the entry, so the
 * passphrase's length is not on display. Editing keeps working unchanged.
 *
 * Returns the number of bytes typed (0 is a legal answer, and the LUKS layer
 * will reject it like any wrong passphrase), or a negative value if the user
 * pressed ESC. On ESC the buffer is wiped before returning; on success the
 * caller owns the wipe. buf always ends NUL-terminated within cap. */
INTN menu_prompt_secret(EFI_SYSTEM_TABLE *st, const CHAR16 *prompt,
                        char *buf, UINTN cap);

#endif /* FALCON_MENU_H */
