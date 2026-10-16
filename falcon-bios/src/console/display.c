/* falcon-bios: the display. See display.h.
 *
 * Why VGA memory and not INT 10h. Teletype output (AH=0Eh) cannot colour a
 * cell, and AH=09h can, one character per call, a round trip through the
 * thunk down to real mode and back each. Text memory at 0xB8000 is an
 * ordinary address in flat protected mode, a cell is a character and an
 * attribute byte, and the attribute byte is EFI_TEXT_ATTR's encoding
 * already. So a run goes in as a loop of stores, and the BIOS is asked only
 * for what it alone knows: the mode, the cursor and the keyboard.
 *
 * The serial mirror. Every cell that changes on screen goes to COM1 as the
 * ANSI sequences OVMF's terminal sends for the same Simple Text Output calls
 * (an attribute, a cursor move, the text), so the coverage harness reads
 * either loader's serial the same way, finding the selected row by its
 * colours. The glyphs go as ASCII (tui_ascii): a box is +, - and |, the
 * backdrop blank. falcon-core sends only the cells that changed, so a
 * moved selection is two rows on the line, not a screen.
 *
 * The spinner. Its frames (tui_spin_vga) are loaded into the font over
 * characters 0x01-0x04, which nothing here prints, with the BIOS's own call
 * (INT 10h AX=1100h), and the ROM's glyphs for them go back the same way
 * before a payload has the screen. Only when the BIOS says where its ROM
 * font is (AX=1130h): without that they could not go back, and the frames
 * are the font's | / - \ instead. A frame lasts five ticks of the BIOS
 * clock, 275 ms, the nearest the 18.2 Hz tick comes to 250. Its frames
 * stay off the serial mirror: a tick lands inside a read, where it would
 * split a diagnostic line half written with bios_puts.
 */

#include <start/bios.h>
#include <console/display.h>
#include <console/cdclock.h>
#include <hw/modrt.h>
#include <power/power.h>

#include <tui/tui.h>

#define VGA_TEXT  0xB8000u
#define COLS      ((size_t)80u)
#define ROWS      ((size_t)25u)
#define BDA_TICKS 0x46Cu
#define BDA_MODE  0x449u
#define SPIN_CHAR 0x01u
#define SPIN_TICKS 5u

static tui_cell g_screen[COLS * ROWS];
static tui_cell g_shown[COLS * ROWS];
static tui_cell g_keep[COLS * ROWS];
static int g_ready;

/* The clock since display_init, and the idle time since the last key or
 * touch (DESIGN Section 7.9). */
static cdclock g_clock;
static cdclock g_idle_clk;
static unsigned g_idle = TUI_IDLE_SECONDS;

/* The serial line's state, so a sequence is sent only when it changes
 * something: the attribute last sent (0x100 when none has been), and where
 * the terminal's cursor is (SIZE_MAX when not known). */
static unsigned g_ser_attr = 0x100u;

/* The ROM's 8x16 font as AX=1130h gave it (0:0 when it did not), and
 * whether the spinner's frames are in the font now. */
static uint16_t g_rom_seg;
static uint16_t g_rom_off;
static int g_spin_font;
static size_t g_ser_row = (size_t)-1;
static size_t g_ser_col = (size_t)-1;

static volatile uint16_t *vga(void)
{
    return (volatile uint16_t *)(uintptr_t)VGA_TEXT;
}

/* ---- the serial mirror ---- */

static void ser_dec2(size_t v)
{
    bios_putc((char)('0' + (v / 10u) % 10u));
    bios_putc((char)('0' + v % 10u));
}

/* One-based, two digits each, as EDK2's TerminalDxe writes it. */
static void ser_goto(size_t row, size_t col)
{
    if (row == g_ser_row && col == g_ser_col)
        return;
    bios_puts("\x1b[");
    ser_dec2(row + 1u);
    bios_putc(';');
    ser_dec2(col + 1u);
    bios_putc('H');
    g_ser_row = row;
    g_ser_col = col;
}

/* Brightness, then foreground, then background. The palette index is EFI's
 * and VGA's (blue is 1, red is 4); ANSI's has red and blue the other way
 * round, which is what the table is for. */
static void ser_attr(uint8_t attr)
{
    static const char ansi[8] = { '0', '4', '2', '6', '1', '5', '3', '7' };

    if (g_ser_attr == attr)
        return;
    g_ser_attr = attr;
    bios_puts("\x1b[");
    bios_putc((attr & 0x08u) ? '1' : '0');
    bios_puts("m\x1b[3");
    bios_putc(ansi[attr & 0x07u]);
    bios_puts("m\x1b[4");
    bios_putc(ansi[(attr >> 4) & 0x07u]);
    bios_putc('m');
}

/* ---- the BIOS's calls ---- */

static void int10(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx)
{
    thunk_regs.eax = eax;
    thunk_regs.ebx = ebx;
    thunk_regs.ecx = ecx;
    thunk_regs.edx = edx;
    thunk_vector = 0x10;
    thunk_bios();
}

/* The RTC's seconds byte, or CDCLOCK_NO_RTC when the call says the clock
 * is not running (cdclock.h has why there are two clocks). */
static int rtc_seconds(void)
{
    thunk_regs.eax = 0x0200u;
    thunk_regs.ecx = 0;
    thunk_regs.edx = 0;
    thunk_vector = 0x1A;
    thunk_bios();
    if ((thunk_regs.flags & BIOS_FLAG_CARRY) != 0)
        return CDCLOCK_NO_RTC;
    return (int)((thunk_regs.edx >> 8) & 0xFFu);
}

/* AX=1100h: n glyphs of 16 bytes from es:bp into font block 0, from
 * character `first`, without setting the mode again. */
static void font_load(uint16_t es, uint16_t bp, unsigned first, unsigned n)
{
    thunk_regs.es = es;
    thunk_regs.bp = bp;
    int10(0x1100u, 0x1000u, n, first);     /* BH=16 bytes, BL=block 0 */
}

/* The ROM font's address, asked once the mode is set: AX=1130h BH=06h
 * answers in es:bp. */
static void font_find(void)
{
    thunk_regs.es = 0;
    thunk_regs.bp = 0;
    int10(0x1130u, 0x0600u, 0, 0);
    g_rom_seg = thunk_regs.es;
    g_rom_off = thunk_regs.bp;
}

static void spin_font_in(void)
{
    uint8_t *at = (uint8_t *)(uintptr_t)FONT_ADDR;
    size_t k;
    size_t r;

    if (g_rom_seg == 0 && g_rom_off == 0)
        return;
    for (k = 0; k < TUI_SPIN_N; k++)
        for (r = 0; r < 16u; r++)
            at[k * 16u + r] = tui_spin_vga[k][r];
    font_load(0, (uint16_t)FONT_ADDR, SPIN_CHAR, TUI_SPIN_N);
    g_spin_font = 1;
}

static void spin_font_out(void)
{
    if (!g_spin_font)
        return;
    font_load(g_rom_seg, (uint16_t)(g_rom_off + SPIN_CHAR * 16u), SPIN_CHAR,
              TUI_SPIN_N);
    g_spin_font = 0;
}

static uint32_t tenths_since(cdclock *c)
{
    return cdclock_tenths(c, peek32(BDA_TICKS), rtc_seconds());
}

/* Every wait for a key comes through here, so a keyboard stage 3 polls
 * itself (bios_key_poll: falcon jump's USB modules) is read wherever the
 * BIOS's is. */
static int key_waiting(void)
{
    (void)bios_key_poll();
    return bios_key_ready();
}

/* ---- the display ---- */

static void d_put(void *ctx, size_t row, size_t col, const uint16_t *s,
                  size_t n, uint8_t attr)
{
    size_t i;

    (void)ctx;
    if (row >= ROWS)
        return;
    for (i = 0; i < n && col + i < COLS; i++) {
        uint8_t ch = tui_cp437(s[i], (uint8_t)'?');

        if (g_spin_font && s[i] >= TUI_SPIN0 &&
            s[i] < TUI_SPIN0 + TUI_SPIN_N)
            ch = (uint8_t)(SPIN_CHAR + (s[i] - TUI_SPIN0));

        vga()[row * COLS + col + i] = (uint16_t)(((unsigned)attr << 8) | ch);
        if (s[i] >= TUI_SPIN0 && s[i] < TUI_SPIN0 + TUI_SPIN_N)
            continue;
        ser_attr(attr);
        ser_goto(row, col + i);
        bios_putc(tui_ascii(s[i], '?'));
        g_ser_col++;
    }
}

static void d_cursor(void *ctx, int on, size_t row, size_t col)
{
    (void)ctx;
    int10(0x0100u, 0, on ? 0x0607u : 0x2000u, 0);     /* AH=01h, shape */
    if (on) {
        int10(0x0200u, 0, 0, ((uint32_t)row << 8) | (uint32_t)col);
        ser_attr(TUI_ATTR_TEXT);
        ser_goto(row, col);
    }
    bios_puts(on ? "\x1b[?25h" : "\x1b[?25l");
}

static int d_key(void *ctx, uint32_t tenths)
{
    cdclock wait;
    int k;

    (void)ctx;
    cdclock_start(&wait, peek32(BDA_TICKS), rtc_seconds());
    for (;;) {
        if (key_waiting()) {
            k = bios_getkey();
            cdclock_start(&g_idle_clk, peek32(BDA_TICKS), rtc_seconds());
            if (k < 0)
                return TUI_KEY_OTHER;
            return k;
        }
        if (g_idle != 0 && tenths_since(&g_idle_clk) >= g_idle * 10u)
            return TUI_KEY_IDLE;
        if (tenths == 0 ||
            (tenths != TUI_FOREVER && tenths_since(&wait) >= tenths))
            return TUI_KEY_NONE;
    }
}

static uint32_t d_clock(void *ctx)
{
    (void)ctx;
    return tenths_since(&g_clock);
}

static unsigned d_spin(void *ctx)
{
    (void)ctx;
    return (unsigned)(peek32(BDA_TICKS) / SPIN_TICKS);
}

/* The ROM's glyphs back over the spinner's, for the payload. */
static void d_handoff(void *ctx)
{
    (void)ctx;
    spin_font_out();
}

static void d_touch(void *ctx)
{
    (void)ctx;
    cdclock_start(&g_idle_clk, peek32(BDA_TICKS), rtc_seconds());
}

/* The VGA sequencer's clocking mode register, bit 5: the screen off, which
 * blanks the signal (and puts most monitors to sleep) with text memory
 * left as it is, so turning it back on shows the page again. */
static void vga_screen(int on)
{
    uint8_t v;

    outb(0x3C4u, 0x01u);
    v = inb(0x3C5u);
    outb(0x3C5u, on ? (uint8_t)(v & ~0x20u) : (uint8_t)(v | 0x20u));
}

/* No key for the idle time: the keyboard ring is wiped, then the machine
 * goes off; when nothing takes it off the screen is turned off until a
 * key, which is swallowed. */
static void d_idle_off(void *ctx)
{
    (void)ctx;
    bios_key_buffer_wipe();
    bios_puts("falcon-bios: no key for the idle time, powering off\n");
    power_off();
    bios_puts("falcon-bios: screen off until a key\n");
    vga_screen(0);
    while (!key_waiting())
        ;
    (void)bios_getkey();
    vga_screen(1);
    cdclock_start(&g_idle_clk, peek32(BDA_TICKS), rtc_seconds());
}

static void d_power_off(void *ctx)
{
    (void)ctx;
    bios_key_buffer_wipe();
    power_off();
    bios_reboot();
}

/* A payload that set another mode and came back: 80x25 text again, and the
 * terminal cleared, since falcon-core sends the whole screen next. */
static void d_reclaim(void *ctx)
{
    (void)ctx;
    if (peek8(BDA_MODE) != 0x03u)
        int10(0x0003u, 0, 0, 0);
    /* Setting the mode loads the ROM font; either way the frames go in
     * again, over whatever the payload left. */
    spin_font_in();
    int10(0x0100u, 0, 0x2000u, 0);
    g_ser_attr = 0x100u;
    g_ser_row = (size_t)-1;
    bios_puts("\x1b[0m\x1b[2J\x1b[?25l");
}

static void d_log(void *ctx, const uint16_t *line)
{
    (void)ctx;
    /* A line of its own wherever the mirror left the terminal's cursor. */
    bios_puts("\r\n");
    while (*line != 0)
        bios_putc(tui_ascii(*line++, '?'));
    bios_puts("\r\n");
    g_ser_row = (size_t)-1;
}

static const tui_display g_display = {
    (void *)0, d_put, d_cursor, d_key, d_clock, d_touch, d_idle_off,
    d_power_off, d_reclaim, d_log, (void (*)(void *, int))0, d_spin,
    d_handoff,
};

void display_idle_set(unsigned seconds)
{
    g_idle = seconds;
}

void display_init(void)
{
    size_t i;

    int10(0x0003u, 0, 0, 0);                /* AH=00h AL=03h: 80x25 text */
    font_find();
    spin_font_in();
    for (i = 0; i < COLS * ROWS; i++)
        vga()[i] = 0;
    bios_puts("\x1b[0m\x1b[2J");
    cdclock_start(&g_clock, peek32(BDA_TICKS), rtc_seconds());
    g_idle_clk = g_clock;
    /* Every INT 13h call that comes back turns a step's spinner, so a
     * long read is seen to move, on every stage 3 alike. */
    bios_disk_on_call(tui_tick);
    tui_init(&g_display, COLS, ROWS, g_screen, g_shown, g_keep, COLS * ROWS);
    tui_flush();
    g_ready = 1;
}

/* bios_fatal's wait: the screens' when they are up. */
int bios_wait_key(void)
{
    return g_ready ? tui_wait() : bios_getkey();
}
