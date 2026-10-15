/* falcon-boot: the UEFI loader's display. See display.h.
 *
 * Why the largest mode. The console font is a fixed 8x19 cell in every
 * mode, so a smaller mode does not enlarge the text; it makes the
 * firmware's console driver centre a smaller grid and pad the rest itself.
 * Only on the largest grid is the whole screen the backdrop's.
 */

#include <console/display.h>
#include <console/idle.h>
#include <console/serial.h>
#include <tsc/tsc.h>

#include <tui/tui.h>

static EFI_SYSTEM_TABLE *g_st;

/* The screen, what the console was last sent, and the copy a dialog is
 * drawn over. */
static tui_cell g_screen[TUI_COLS_MAX * TUI_ROWS_MAX];
static tui_cell g_shown[TUI_COLS_MAX * TUI_ROWS_MAX];
static tui_cell g_keep[TUI_COLS_MAX * TUI_ROWS_MAX];

/* The mode the screens were laid out for, put back when a payload that
 * changed it returns. */
static INT32 g_mode = -1;

/* A run, terminated for OutputString. */
static CHAR16 g_run[TUI_COLS_MAX + 1u];

/* The clock: tenths of a second, counted by a periodic timer's
 * notification. Counted rather than read from the TSC because a timer is
 * there on every firmware and needs no calibration. */
static volatile UINT32 g_tenths;
static EFI_EVENT g_clock_ev;

/* The timer a key wait with an end waits on. */
static EFI_EVENT g_wait_ev;

static void EFIAPI clock_tick(EFI_EVENT ev, void *ctx)
{
    (void)ev;
    (void)ctx;
    g_tenths++;
}

static void d_put(void *ctx, size_t row, size_t col, const uint16_t *s,
                  size_t n, uint8_t attr)
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *o = g_st->ConOut;
    size_t i;

    (void)ctx;
    for (i = 0; i < n && i < TUI_COLS_MAX; i++)
        g_run[i] = (CHAR16)s[i];
    g_run[i] = 0;
    if ((UINTN)o->Mode->Attribute != (UINTN)attr)
        o->SetAttribute(o, (UINTN)attr);
    o->SetCursorPosition(o, (UINTN)col, (UINTN)row);
    o->OutputString(o, g_run);
}

static void d_cursor(void *ctx, int on, size_t row, size_t col)
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *o = g_st->ConOut;

    (void)ctx;
    if (on) {
        o->SetAttribute(o, TUI_ATTR_TEXT);
        o->SetCursorPosition(o, (UINTN)col, (UINTN)row);
    }
    o->EnableCursor(o, on ? TRUE : FALSE);
}

static int map_key(const EFI_INPUT_KEY *k)
{
    switch (k->ScanCode) {
    case EFI_SCAN_NULL:
        break;
    case EFI_SCAN_UP:   return TUI_KEY_UP;
    case EFI_SCAN_DOWN: return TUI_KEY_DOWN;
    case EFI_SCAN_PGUP: return TUI_KEY_PGUP;
    case EFI_SCAN_PGDN: return TUI_KEY_PGDN;
    case EFI_SCAN_HOME: return TUI_KEY_HOME;
    case EFI_SCAN_END:  return TUI_KEY_END;
    case EFI_SCAN_ESC:  return TUI_KEY_ESC;
    default:            return TUI_KEY_OTHER;
    }
    if (k->UnicodeChar == 0x0D || k->UnicodeChar == 0x08 ||
        k->UnicodeChar == 0x09)
        return (int)k->UnicodeChar;
    if (k->UnicodeChar >= 0x20 && k->UnicodeChar <= 0x7E)
        return (int)k->UnicodeChar;
    return TUI_KEY_OTHER;
}

/* A key already in the queue, or TUI_KEY_NONE. */
static int read_key(void)
{
    EFI_INPUT_KEY key;

    if (g_st->ConIn->ReadKeyStroke(g_st->ConIn, &key) != EFI_SUCCESS)
        return TUI_KEY_NONE;
    idle_touch();
    return map_key(&key);
}

static int d_key(void *ctx, uint32_t tenths)
{
    EFI_BOOT_SERVICES *bs = g_st->BootServices;
    EFI_EVENT events[3];
    UINTN n = 0;
    UINTN at_idle = 3u;
    UINTN at_wait = 3u;
    UINTN index;
    int k;

    (void)ctx;
    /* A poll: the idle time first, so a menu stepping its probe still
     * powers an unwatched machine off. */
    if (idle_event() != (EFI_EVENT)0 &&
        bs->CheckEvent(idle_event()) == EFI_SUCCESS)
        return TUI_KEY_IDLE;
    k = read_key();
    if (k != TUI_KEY_NONE || tenths == 0)
        return k;

    events[n++] = g_st->ConIn->WaitForKey;
    if (idle_event() != (EFI_EVENT)0) {
        at_idle = n;
        events[n++] = idle_event();
    }
    if (tenths != TUI_FOREVER && g_wait_ev != (EFI_EVENT)0 &&
        bs->SetTimer(g_wait_ev, TimerRelative,
                     (UINT64)tenths * 1000000u) == EFI_SUCCESS) {
        at_wait = n;
        events[n++] = g_wait_ev;
    }
    for (;;) {
        /* A failed wait means there is no console input to wait on; the
         * read would spin on EFI_NOT_READY forever. */
        if (bs->WaitForEvent(n, events, &index) != EFI_SUCCESS)
            return TUI_KEY_GONE;
        if (index == at_idle)
            return TUI_KEY_IDLE;
        if (index == at_wait)
            return TUI_KEY_NONE;
        k = read_key();
        if (k != TUI_KEY_NONE) {
            if (at_wait != 3u)
                bs->SetTimer(g_wait_ev, TimerCancel, 0);
            return k;
        }
    }
}

static uint32_t d_clock(void *ctx)
{
    (void)ctx;
    return g_tenths;
}

static void d_touch(void *ctx)
{
    (void)ctx;
    idle_touch();
}

static void d_idle_off(void *ctx)
{
    (void)ctx;
    idle_expired();
}

static void d_power_off(void *ctx)
{
    (void)ctx;
    idle_power_off();
}

/* A payload that set another mode and came back leaves the screens laid out
 * for a console that is not there; the mode goes back first. */
static void d_reclaim(void *ctx)
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *o = g_st->ConOut;

    (void)ctx;
    if (g_mode >= 0 && o->Mode->Mode != g_mode)
        o->SetMode(o, (UINTN)g_mode);
    o->SetAttribute(o, TUI_ATTR_TEXT);
    o->EnableCursor(o, FALSE);
}

static void d_log(void *ctx, const uint16_t *line)
{
    char buf[256];
    size_t n = 0;

    (void)ctx;
    while (line[n] != 0 && n + 1u < sizeof buf) {
        buf[n] = tui_ascii(line[n], '?');
        n++;
    }
    buf[n] = '\0';
    serial_line(g_st, buf);
}

static void d_wait(void *ctx, int begin)
{
    (void)ctx;
    if (begin)
        tsc_wait_begin();
    else
        tsc_wait_end();
}

static const tui_display g_display = {
    (void *)0, d_put, d_cursor, d_key, d_clock, d_touch, d_idle_off,
    d_power_off, d_reclaim, d_log, d_wait,
};

/* Sanity rails: a firmware reporting nonsense cannot hand the screens a
 * grid they would subtract from. */
static int mode_fits(UINTN c, UINTN r)
{
    return c >= 80u && r >= 25u && c <= TUI_COLS_MAX && r <= TUI_ROWS_MAX;
}

void display_init(EFI_SYSTEM_TABLE *st)
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *o = st->ConOut;
    INT32 best = -1;
    INT32 i;
    UINTN best_cols = 0;
    UINTN best_rows = 0;
    UINTN cols = 0;
    UINTN rows = 0;

    g_st = st;

    /* Mode 0 is 80x25 by spec and mode 1 is 80x50 when present; 2 and up
     * are OEM-defined and are where the useful ones live. Largest wins,
     * rows first, because the panels are lists. Every step may fail:
     * QueryMode returns EFI_UNSUPPORTED for modes below MaxMode on firmware
     * that miscounts, and SetMode can refuse a mode it just described. The
     * console then keeps what it had. */
    for (i = 0; i < o->Mode->MaxMode; i++) {
        UINTN c = 0;
        UINTN r = 0;

        if (o->QueryMode(o, (UINTN)i, &c, &r) != EFI_SUCCESS ||
            !mode_fits(c, r))
            continue;
        if (r > best_rows || (r == best_rows && c > best_cols)) {
            best = i;
            best_cols = c;
            best_rows = r;
        }
    }
    if (best >= 0 && best != o->Mode->Mode)
        o->SetMode(o, (UINTN)best);
    g_mode = o->Mode->Mode;
    if (o->QueryMode(o, (UINTN)o->Mode->Mode, &cols, &rows) != EFI_SUCCESS ||
        !mode_fits(cols, rows)) {
        cols = 80u;
        rows = 25u;
    }

    if (st->BootServices->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL,
                                      TPL_CALLBACK, clock_tick, (void *)0,
                                      &g_clock_ev) == EFI_SUCCESS)
        st->BootServices->SetTimer(g_clock_ev, TimerPeriodic, 1000000u);
    if (st->BootServices->CreateEvent(EVT_TIMER, 0, (EFI_EVENT_NOTIFY)0,
                                      (void *)0, &g_wait_ev) != EFI_SUCCESS)
        g_wait_ev = (EFI_EVENT)0;

    o->SetAttribute(o, TUI_ATTR_TEXT);
    o->ClearScreen(o);
    o->EnableCursor(o, FALSE);
    tui_init(&g_display, (size_t)cols, (size_t)rows, g_screen, g_shown,
             g_keep, sizeof g_screen / sizeof g_screen[0]);
    tui_flush();
}
