/* falcon-bios: the masked passphrase prompt (docs/DESIGN.md Section 5,
 * docs/notes/bios-route.md Section 4.4).
 *
 * Its own translation unit for the same reason the loader's is
 * (falcon-efi/src/console/prompt.c): "what has held the secret" should have
 * a one-file answer. The buffer is the caller's and this file keeps nothing.
 *
 * It reads through INT 16h and echoes to the screen and serial both, which
 * is what every other line this build prints does. A masked echo is the
 * whole point: the character count is visible, the characters are not.
 */

#include <bios.h>
#include <prompt.h>

int prompt_passphrase(const char *label, char *out, size_t cap, size_t *len)
{
    size_t n = 0;

    if (out == (char *)0 || cap == 0)
        return -1;

    bios_screen_puts("\n  ");
    bios_screen_puts(label);
    bios_screen_puts(": ");

    for (;;) {
        int key = bios_getkey();

        if (key == BIOS_KEY_ENTER)
            break;
        if (key == BIOS_KEY_ESC) {
            n = 0;
            break;
        }
        if (key == '\b') {
            if (n > 0) {
                n--;
                /* Backspace, space, backspace: the only way to unprint a
                 * character through a teletype call. */
                bios_screen_puts("\b \b");
            }
            continue;
        }
        if (key < 0x20 || key > 0x7E)
            continue;
        if (n + 1u < cap) {
            out[n++] = (char)key;
            bios_screen_puts("*");
        }
    }
    out[n] = '\0';
    bios_screen_puts("\n");
    if (len != (size_t *)0)
        *len = n;
    return n > 0 ? 0 : -1;
}
