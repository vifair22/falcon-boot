/* falcon-bios: the masked passphrase prompt (docs/DESIGN.md Section 5,
 * docs/notes/bios-route.md Section 4.4).
 *
 * Its own translation unit for the same reason the loader's is
 * (falcon-efi/src/console/prompt.c): "what has held the secret" should have
 * a one-file answer. The buffer is the caller's and this file keeps nothing.
 *
 * It behaves as that prompt does, key for key: the TAB hint after the
 * prompt, erased on the first keystroke; '*' per character; TAB erasing what
 * is shown and silencing the rest of the entry, the systemd ask-password
 * convention; printable ASCII only, for the reason the UEFI prompt gives.
 */

#include <start/bios.h>
#include <console/prompt.h>
#include <console/screen.h>

#include <crypto/crypto.h>

#define KEY_BACKSPACE 0x08
#define KEY_TAB       0x09

static const char k_hint[] = "(press TAB for no echo)";

/* Erases the n characters left of the cursor. */
static void erase_back(size_t n)
{
    while (n-- > 0)
        screen_raw("\b \b");
}

int prompt_passphrase(char *out, size_t cap, size_t *len)
{
    size_t n = 0;
    int silent = 0;
    int hint_shown = 0;

    if (out == (char *)0 || cap == 0)
        return -1;

    out[0] = '\0';
    screen_cursor(1);
    screen_out(k_hint);
    hint_shown = 1;
    /* Wiped before an idle power-off (console/screen.h). */
    screen_idle_secret(out, cap);

    for (;;) {
        int key = screen_key();

        if (key == BIOS_KEY_IDLE) {
            /* The power-off did not take; what was typed is gone. */
            if (!silent)
                erase_back(n);
            n = 0;
            continue;
        }

        if (key == BIOS_KEY_ESC) {
            /* Cancel wipes here rather than leaving it to the caller: a
             * cancelled prompt is the one exit where the caller has no
             * reason to think it ever held anything. */
            crypto_wipe(out, cap);
            screen_idle_secret((void *)0, 0);
            screen_out("\n");
            return -1;
        }
        if (key == BIOS_KEY_ENTER) {
            screen_idle_secret((void *)0, 0);
            screen_out("\n");
            out[n] = '\0';
            if (len != (size_t *)0)
                *len = n;
            return 0;
        }
        if (key == KEY_TAB) {
            if (hint_shown) {
                erase_back(sizeof k_hint - 1u);
                hint_shown = 0;
            }
            if (!silent) {
                erase_back(n);
                silent = 1;
            }
            continue;
        }
        if (key == KEY_BACKSPACE) {
            if (n > 0) {
                n--;
                /* Overwrite the byte as it leaves the buffer, not merely on
                 * the way out: a passphrase retyped after a correction would
                 * otherwise leave its earlier tail sitting past n. */
                out[n] = '\0';
                if (!silent)
                    screen_raw("\b \b");
            }
            continue;
        }
        /* Printable ASCII only, and only while there is room for the byte
         * plus its terminator. An over-long passphrase stops echoing, which
         * is the honest signal that the keystroke did not land. */
        if (key < 0x20 || key > 0x7E)
            continue;
        if (n + 1u >= cap)
            continue;
        if (hint_shown) {
            erase_back(sizeof k_hint - 1u);
            hint_shown = 0;
        }
        out[n++] = (char)key;
        if (!silent)
            screen_raw("*");
    }
}
