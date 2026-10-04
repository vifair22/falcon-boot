/* The masked passphrase line editor. See menu.h for the contract and for why
 * it produces bytes rather than CHAR16.
 *
 * Separate translation unit from menu.c on purpose: this one handles a secret
 * and menu.c does not, so "which code has ever held the passphrase" stays a
 * question with a one-file answer.
 */

#include <console/menu.h>
#include <console/idle.h>
#include <crypto/crypto.h>
#include <tsc/tsc.h>

/* Control characters the console delivers in UnicodeChar with ScanCode 0
 * (UEFI spec 12.3, Table 12.3). CR is the only commit key: LF never arrives
 * on its own from a real console, and treating it as a commit would make a
 * stray keystroke submit a half-typed passphrase. */
#define CHAR_BACKSPACE 0x0008
#define CHAR_TAB       0x0009
#define CHAR_CR        0x000D

/* The prompt is a screen like any other and sits in the same block. The
 * asterisks go through menu_out too, so a long entry wraps inside the block
 * and menu_erase can follow it back across the wrap. */
static void out(EFI_SYSTEM_TABLE *st, const CHAR16 *s)
{
    menu_out(st, s);
}

static void (*g_tick)(void);

void menu_prompt_set_tick(void (*fn)(void))
{
    g_tick = fn;
}

/* A second, in 100 ns units. */
#define TICK_100NS 10000000u

static EFI_EVENT tick_start(EFI_SYSTEM_TABLE *st)
{
    EFI_EVENT ev = (EFI_EVENT)0;

    if (g_tick == (void (*)(void))0 ||
        st->BootServices->CreateEvent(EVT_TIMER, 0, (EFI_EVENT_NOTIFY)0,
                                      (void *)0, &ev) != EFI_SUCCESS)
        return (EFI_EVENT)0;
    if (st->BootServices->SetTimer(ev, TimerPeriodic, TICK_100NS) !=
        EFI_SUCCESS) {
        st->BootServices->CloseEvent(ev);
        return (EFI_EVENT)0;
    }
    return ev;
}

static void tick_stop(EFI_SYSTEM_TABLE *st, EFI_EVENT ev)
{
    if (ev == (EFI_EVENT)0)
        return;
    st->BootServices->SetTimer(ev, TimerCancel, 0);
    st->BootServices->CloseEvent(ev);
}

static INTN prompt_run(EFI_SYSTEM_TABLE *st, const CHAR16 *prompt,
                       char *buf, UINTN cap, EFI_EVENT tick);

INTN menu_prompt_secret(EFI_SYSTEM_TABLE *st, const CHAR16 *prompt,
                        char *buf, UINTN cap)
{
    EFI_EVENT tick;
    INTN r;

    if (st == (void *)0 || buf == (void *)0 || cap == 0)
        return -1;
    tick = tick_start(st);
    tsc_wait_begin();
    r = prompt_run(st, prompt, buf, cap, tick);
    tsc_wait_end();
    tick_stop(st, tick);
    return r;
}

static INTN prompt_run(EFI_SYSTEM_TABLE *st, const CHAR16 *prompt,
                       char *buf, UINTN cap, EFI_EVENT tick)
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

    buf[0] = '\0';
    st->ConOut->EnableCursor(st->ConOut, TRUE);
    if (prompt != (void *)0)
        out(st, prompt);
    /* systemd renders this hint dim and parks the cursor before it; OVMF
     * renders DARKGRAY as black-on-black and its backspace is destructive,
     * so the hint inherits
     * the prompt's attribute and the cursor rests after it. The first
     * keystroke erases it, exactly as systemd's does. */
    out(st, hint);
    hint_shown = TRUE;
    idle_touch();

    for (;;) {
        EFI_INPUT_KEY key;
        EFI_EVENT events[3];
        UINTN n = 0;
        UINTN at_idle = 3u;
        UINTN at_tick = 3u;
        UINTN index;

        events[n++] = st->ConIn->WaitForKey;
        if (idle_event() != (EFI_EVENT)0) {
            at_idle = n;
            events[n++] = idle_event();
        }
        if (tick != (EFI_EVENT)0) {
            at_tick = n;
            events[n++] = tick;
        }
        if (st->BootServices->WaitForEvent(n, events, &index) != EFI_SUCCESS)
            break;
        if (index == at_tick) {
            g_tick();
            continue;
        }
        if (index == at_idle) {
            /* Nobody finished typing: what was typed goes before the
             * machine does (console/idle.h). */
            crypto_wipe(buf, (size_t)cap);
            if (!silent)
                menu_erase(st, len);
            len = 0;
            idle_expired();
            continue;
        }
        if (st->ConIn->ReadKeyStroke(st->ConIn, &key) != EFI_SUCCESS)
            continue;
        idle_touch();

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
                menu_erase(st, sizeof(hint) / sizeof(hint[0]) - 1);
                hint_shown = FALSE;
            }
            if (!silent) {
                menu_erase(st, len);
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
                    menu_erase(st, 1);
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
            menu_erase(st, sizeof(hint) / sizeof(hint[0]) - 1);
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
