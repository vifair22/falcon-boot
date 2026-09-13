/* The masked passphrase line editor. See menu.h for the contract and for why
 * it produces bytes rather than CHAR16.
 *
 * Separate translation unit from menu.c on purpose: this one handles a secret
 * and menu.c does not, so "which code has ever held the passphrase" stays a
 * question with a one-file answer.
 */

#include <menu/menu.h>
#include <crypto/crypto.h>

/* Control characters the console delivers in UnicodeChar with ScanCode 0
 * (UEFI spec 12.3, Table 12.3). CR is the only commit key: LF never arrives
 * on its own from a real console, and treating it as a commit would make a
 * stray keystroke submit a half-typed passphrase. */
#define CHAR_BACKSPACE 0x0008
#define CHAR_TAB       0x0009
#define CHAR_CR        0x000D

static void out(EFI_SYSTEM_TABLE *st, const CHAR16 *s)
{
    /* OutputString's parameter is mutable because the spec declares it so,
     * not because the firmware writes to it; the round trip through
     * uintptr_t is what keeps -Wcast-qual quiet about the spec's wart. */
    st->ConOut->OutputString(st->ConOut, (CHAR16 *)(uintptr_t)s);
}

/* Blanks n columns from the cursor and returns the cursor to where it was:
 * how both the hint and (via one column) an asterisk are unprinted. */
static void erase_forward(EFI_SYSTEM_TABLE *st, UINTN n)
{
    UINTN i;

    for (i = 0; i < n; i++)
        out(st, L" ");
    for (i = 0; i < n; i++)
        out(st, L"\b");
}

INTN menu_prompt_secret(EFI_SYSTEM_TABLE *st, const CHAR16 *prompt,
                        char *buf, UINTN cap)
{
    UINTN len = 0;
    /* systemd ask-password convention: Tab erases the asterisks already on
     * screen and silences the rest of the entry, so an onlooker cannot even
     * count the passphrase's length. Display state only - the buffer, the
     * keys accepted, and every other behavior are identical either way. */
    BOOLEAN silent = FALSE;
    /* The hint is shown dim after the prompt and erased on the first
     * keystroke, exactly as systemd-ask-password does, so it never sits
     * next to the asterisks pretending to be part of the passphrase. */
    static const CHAR16 hint[] = L"(press TAB for no echo)";
    BOOLEAN hint_shown = FALSE;

    if (st == (void *)0 || buf == (void *)0 || cap == 0)
        return -1;

    buf[0] = '\0';
    st->ConOut->EnableCursor(st->ConOut, TRUE);
    if (prompt != (void *)0)
        out(st, prompt);
    {
        INT32 attr = st->ConOut->Mode->Attribute;
        UINTN i;

        st->ConOut->SetAttribute(st->ConOut,
                                 EFI_TEXT_ATTR(EFI_DARKGRAY, EFI_BLACK));
        out(st, hint);
        st->ConOut->SetAttribute(st->ConOut, (UINTN)attr);
        /* Park the cursor back at the hint's first column so the first
         * asterisk lands where the hint starts rather than after it. */
        for (i = 0; hint[i] != L'\0'; i++)
            out(st, L"\b");
        hint_shown = TRUE;
    }

    for (;;) {
        EFI_INPUT_KEY key;
        UINTN index;

        if (st->BootServices->WaitForEvent(1, &st->ConIn->WaitForKey,
                                           &index) != EFI_SUCCESS)
            break;
        if (st->ConIn->ReadKeyStroke(st->ConIn, &key) != EFI_SUCCESS)
            continue;

        if (key.ScanCode == EFI_SCAN_ESC) {
            /* Cancel wipes here rather than leaving it to the caller: a
             * cancelled prompt is the one exit where the caller has no
             * reason to think it ever held anything. */
            crypto_wipe(buf, (size_t)cap);
            out(st, L"\r\n");
            return -1;
        }
        if (key.ScanCode != EFI_SCAN_NULL)
            continue; /* arrows, function keys: nothing to do in a line */

        if (key.UnicodeChar == CHAR_CR) {
            out(st, L"\r\n");
            buf[len] = '\0';
            return (INTN)len;
        }
        if (key.UnicodeChar == CHAR_TAB) {
            if (hint_shown) {
                erase_forward(st, sizeof(hint) / sizeof(hint[0]) - 1);
                hint_shown = FALSE;
            }
            if (!silent) {
                UINTN i;

                for (i = 0; i < len; i++)
                    out(st, L"\b \b");
                silent = TRUE;
            }
            continue;
        }
        if (key.UnicodeChar == CHAR_BACKSPACE) {
            if (len > 0) {
                len--;
                /* Overwrite the byte as it leaves the buffer, not merely on
                 * the way out: a passphrase retyped after a correction would
                 * otherwise leave its earlier tail sitting past len. */
                buf[len] = '\0';
                if (!silent)
                    out(st, L"\b \b");
            }
            continue;
        }
        /* Printable ASCII only, and only while there is room for the byte
         * plus its terminator. An over-long passphrase stops echoing, which
         * is the honest signal that the keystroke did not land. */
        if (key.UnicodeChar < 0x20 || key.UnicodeChar > 0x7E)
            continue;
        if (len + 1 >= cap)
            continue;
        if (hint_shown) {
            erase_forward(st, sizeof(hint) / sizeof(hint[0]) - 1);
            hint_shown = FALSE;
        }
        buf[len++] = (char)key.UnicodeChar;
        if (!silent)
            out(st, L"*");
    }

    /* WaitForEvent failed, which means the console is gone; treat it as a
     * cancel so no caller is left holding a half-read secret. */
    crypto_wipe(buf, (size_t)cap);
    return -1;
}
