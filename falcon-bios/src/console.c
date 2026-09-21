/* falcon-bios: serial output (docs/notes/bios-route.md Section 6).
 *
 * COM1 directly rather than INT 10h, and the reason is the same one stage 1
 * brings the port up before it does anything else: a machine with no usable
 * video is exactly the machine this route exists to reach, and the rig reads
 * serial (falcon-efi/tools/run-qemu.sh --bios --headless).
 *
 * The menu will want the screen as well, and that is INT 10h through the
 * thunk when it arrives. This is what everything before the menu says what
 * it is doing on.
 */

#include <bios.h>

#define COM1        0x3F8u
#define LSR_THRE    0x20u    /* transmitter holding register empty */

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;

    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* How long to wait for the transmitter, in reads of the line status
 * register: about a microsecond each, so tens of milliseconds, against the
 * 87 microseconds one character takes at 115200. A port that reads back all
 * ones (nothing there) passes at once; one that never empties is a UART
 * that is present and not working, and after the first timeout it is
 * written to without waiting, since waiting on it is all that could hang. */
#define THRE_SPINS 65536u

/* Nonzero until the port has timed out once. Initialised rather than zero
 * for the reason g_mode below is: stage 2 has no .bss. */
static uint8_t g_com_live = 1;

void bios_putc(char c)
{
    uint32_t n;

    for (n = 0; g_com_live && n < THRE_SPINS; n++) {
        if ((inb((uint16_t)(COM1 + 5u)) & LSR_THRE) != 0)
            break;
    }
    if (n == THRE_SPINS)
        g_com_live = 0;
    outb(COM1, (uint8_t)c);
}

void bios_puts(const char *s)
{
    if (s == (const char *)0)
        return;
    while (*s != '\0') {
        if (*s == '\n')
            bios_putc('\r');
        bios_putc(*s);
        s++;
    }
}

void bios_put_hex64(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    int shift;
    int leading = 1;

    bios_puts("0x");
    for (shift = 60; shift >= 0; shift -= 4) {
        unsigned nibble = (unsigned)((v >> shift) & 0xFu);

        if (nibble == 0 && leading && shift != 0)
            continue;
        leading = 0;
        bios_putc(digits[nibble]);
    }
}

void bios_put_dec(uint64_t v)
{
    char buf[21];
    size_t n = 0;

    if (v == 0) {
        bios_putc('0');
        return;
    }
    while (v > 0 && n < sizeof buf) {
        buf[n++] = (char)('0' + (unsigned)(v % 10u));
        v /= 10u;
    }
    while (n > 0)
        bios_putc(buf[--n]);
}

/* ---- the screen and the keyboard, through the BIOS ----
 *
 * INT 10h AH=0Eh is teletype output: it scrolls, wraps and tracks the cursor
 * itself, which is everything a menu needs and nothing it has to implement.
 * INT 16h AH=00h blocks for a key and returns its scan code in AH, which is
 * the only way to tell an arrow key from anything else.
 */

void bios_screen_at(uint8_t row, uint8_t col)
{
    thunk_regs.eax = 0x0200u;          /* AH=02h, set cursor position */
    thunk_regs.ebx = 0;                /* page 0 */
    thunk_regs.edx = ((uint32_t)row << 8) | col;
    thunk_vector = 0x10;
    thunk_bios();
}

/* The route code; empty draws nothing. Initialised to something other than
 * zero on purpose: this file is linked into stage 2 as well, which nothing
 * loads and so nothing clears, and a zero initialiser would put the pointer
 * in a .bss the build refuses (falcon-bios/build.mk). In .data it is on the
 * disk with the code. */
static const char k_mode_none[] = "";
static const char *g_mode = k_mode_none;

/* The code right-aligned on row 0, two columns in from the edge as the menu's
 * text is from the left. AH=09h writes a character without moving the
 * cursor, so the cursor is read first and put back after: the next line of
 * teletype output has to land where it would have. */
static void mode_draw(void)
{
    uint32_t saved;
    size_t len = 0;
    size_t i;

    while (g_mode[len] != '\0')
        len++;
    if (len == 0 || len > 70u)
        return;

    thunk_regs.eax = 0x0300u;          /* AH=03h, read cursor position */
    thunk_regs.ebx = 0;                /* page 0 */
    thunk_vector = 0x10;
    thunk_bios();
    saved = thunk_regs.edx;            /* DH row, DL column */

    for (i = 0; i < len; i++) {
        bios_screen_at(0, (uint8_t)(78u - len + i));
        thunk_regs.eax = 0x0900u | (uint8_t)g_mode[i]; /* AH=09h, write */
        thunk_regs.ebx = 0x000Fu;      /* page 0, white on black */
        thunk_regs.ecx = 1;            /* once */
        thunk_vector = 0x10;
        thunk_bios();
    }
    bios_screen_at((uint8_t)((saved >> 8) & 0xFFu), (uint8_t)(saved & 0xFFu));
}

void bios_screen_mode(const char *code)
{
    if (code == (const char *)0) {
        g_mode = k_mode_none;
        return;
    }
    g_mode = code;
    bios_puts("  route ");
    bios_puts(code);
    bios_puts("\n");
    mode_draw();
}

void bios_screen_clear(void)
{
    thunk_regs.eax = 0x0600u;          /* AH=06h, scroll up; AL=0 clears */
    thunk_regs.ebx = 0x0700u;          /* attribute: grey on black */
    thunk_regs.ecx = 0;                /* top left */
    thunk_regs.edx = 0x184Fu;          /* bottom right of an 80x25 screen */
    thunk_vector = 0x10;
    thunk_bios();
    bios_screen_at(0, 0);
    mode_draw();
}

static void screen_putc(char c)
{
    thunk_regs.eax = 0x0E00u | (uint8_t)c;   /* AH=0Eh, teletype */
    thunk_regs.ebx = 0x0007u;                /* page 0, grey on black */
    thunk_vector = 0x10;
    thunk_bios();
}

void bios_screen_puts(const char *s)
{
    int line = 0;

    if (s == (const char *)0)
        return;
    while (*s != '\0') {
        if (*s == '\n') {
            screen_putc('\r');
            bios_putc('\r');
            line = 1;
        }
        screen_putc(*s);
        bios_putc(*s);
        s++;
    }
    /* A line break may have scrolled the screen and the code with it. */
    if (line)
        mode_draw();
}

int bios_getkey(void)
{
    uint8_t scan;
    uint8_t ascii;

    thunk_regs.eax = 0x0000u;          /* AH=00h, wait for a key */
    thunk_vector = 0x16;
    thunk_bios();

    scan = (uint8_t)((thunk_regs.eax >> 8) & 0xFFu);
    ascii = (uint8_t)(thunk_regs.eax & 0xFFu);

    /* The arrows have no ASCII, so the scan code is the only evidence. */
    if (ascii == 0) {
        if (scan == 0x48u)
            return BIOS_KEY_UP;
        if (scan == 0x50u)
            return BIOS_KEY_DOWN;
        return -1;
    }
    return (int)ascii;
}

/* ---- reset ----
 *
 * Three ways, each tried only if the one before it did not take. The reset
 * control register at 0xCF9 is every chipset since the ICH, and the 8042's
 * pulse line is what came before it and what most boards still wire up.
 * Neither is certain on a given board, and the last way is: an IDT with no
 * entries turns the breakpoint into a triple fault, which the processor
 * answers with a reset of its own.
 */

/* Port 0x80 is the POST code port. A write to it takes about a microsecond
 * on every board and does nothing else, which makes it a delay that does
 * not depend on how fast the processor is. */
static void io_delay(uint32_t n)
{
    while (n-- > 0)
        outb(0x80u, 0);
}

__attribute__((noreturn)) void bios_reboot(void)
{
    static const struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } no_idt = { 0, 0 };
    uint32_t i;

    outb(0xCF9u, 0x02u);
    outb(0xCF9u, 0x06u);        /* full reset */
    io_delay(500000u);

    for (i = 0; i < 100000u && (inb(0x64u) & 0x02u) != 0; i++)
        ;                       /* the 8042's input buffer, until it is empty */
    outb(0x64u, 0xFEu);         /* pulse the reset line */
    io_delay(500000u);

    __asm__ __volatile__("lidt %0\n\tint3" : : "m"(no_idt));
    for (;;)
        __asm__ __volatile__("hlt");
}
