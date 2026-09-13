#include <menu/menu.h>

/* Layout: banner on row 0, list window from LIST_TOP, hint line at the
 * bottom. Redraws are cursor-repositioned overwrites, not ClearScreen, to
 * avoid flicker on slow console paths (serial mirrors). */
#define LIST_TOP 2u

static void out(EFI_SYSTEM_TABLE *st, CHAR16 *s)
{
    st->ConOut->OutputString(st->ConOut, s);
}

/* Overwrite one screen row with s, padding to width to erase leftovers. */
static void draw_row(EFI_SYSTEM_TABLE *st, UINTN row, UINTN cols,
                     const CHAR16 *s)
{
    CHAR16 pad[2] = { L' ', L'\0' };
    UINTN written = 0;

    st->ConOut->SetCursorPosition(st->ConOut, 0, row);
    if (s != (void *)0) {
        /* Bounded output: clip to the row so long paths cannot wrap and
         * shift every row below by one. */
        CHAR16 chunk[2] = { L'\0', L'\0' };
        while (s[written] != L'\0' && written < cols - 1) {
            chunk[0] = s[written];
            out(st, chunk);
            written++;
        }
    }
    while (written < cols - 1) {
        out(st, pad);
        written++;
    }
}

static void screen_dims(EFI_SYSTEM_TABLE *st, UINTN *cols, UINTN *rows)
{
    if (st->ConOut->QueryMode(st->ConOut,
                              (UINTN)st->ConOut->Mode->Mode,
                              cols, rows) != EFI_SUCCESS ||
        *cols < 10 || *rows < 6) {
        /* Spec-guaranteed minimum mode. */
        *cols = 80;
        *rows = 25;
    }
}

INTN menu_run(EFI_SYSTEM_TABLE *st, const CHAR16 *banner,
              const menu_entry *entries, UINTN count)
{
    UINTN cols, rows, win, top = 0, sel = 0;

    screen_dims(st, &cols, &rows);
    win = rows - LIST_TOP - 2; /* list rows between banner and hint */

    st->ConOut->EnableCursor(st->ConOut, FALSE);
    st->ConOut->ClearScreen(st->ConOut);

    st->ConOut->SetAttribute(st->ConOut,
                             EFI_TEXT_ATTR(EFI_LIGHTCYAN, EFI_BLACK));
    draw_row(st, 0, cols, banner);
    st->ConOut->SetAttribute(st->ConOut,
                             EFI_TEXT_ATTR(EFI_DARKGRAY, EFI_BLACK));
    draw_row(st, rows - 1, cols,
             count ? L"up/down select | enter boot | esc exit"
                   : L"no entries | esc exit");

    for (;;) {
        UINTN i;
        EFI_INPUT_KEY key;
        UINTN idx;

        /* Keep the selection inside the window before painting. */
        if (sel < top)
            top = sel;
        if (sel >= top + win)
            top = sel - win + 1;

        for (i = 0; i < win; i++) {
            idx = top + i;
            if (idx < count) {
                st->ConOut->SetAttribute(
                    st->ConOut,
                    idx == sel
                        ? EFI_TEXT_ATTR(EFI_BLACK, EFI_LIGHTGRAY)
                        : EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
                draw_row(st, LIST_TOP + i, cols, entries[idx].label);
            } else {
                st->ConOut->SetAttribute(
                    st->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
                draw_row(st, LIST_TOP + i, cols,
                         (idx == 0) ? L"(no ISOs found on the exFAT "
                                      L"partition)"
                                    : (void *)0);
            }
        }

        st->BootServices->WaitForEvent(1, &st->ConIn->WaitForKey, &i);
        if (st->ConIn->ReadKeyStroke(st->ConIn, &key) != EFI_SUCCESS)
            continue;

        if (key.ScanCode == EFI_SCAN_ESC) {
            st->ConOut->SetAttribute(
                st->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
            st->ConOut->EnableCursor(st->ConOut, TRUE);
            return -1;
        }
        if (count == 0)
            continue;
        if (key.ScanCode == EFI_SCAN_UP && sel > 0)
            sel--;
        else if (key.ScanCode == EFI_SCAN_DOWN && sel + 1 < count)
            sel++;
        else if (key.UnicodeChar == L'\r') {
            st->ConOut->SetAttribute(
                st->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
            st->ConOut->EnableCursor(st->ConOut, TRUE);
            return (INTN)sel;
        }
    }
}
