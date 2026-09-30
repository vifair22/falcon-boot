/* falcon-bios: serial output (docs/notes/bios-route.md Section 6).
 *
 * COM1 directly rather than INT 10h, and the reason is the same one stage 1
 * brings the port up before it does anything else: a machine with no usable
 * video is exactly the machine this route exists to reach, and the rig reads
 * serial (falcon-efi/tools/run-qemu.sh --bios --headless).
 *
 * The screen is falcon-bios/src/console/screen.c's, in stage 3. What is here is
 * what everything before the menu says what it is doing on, and the one
 * screen stage 2 can draw: bios_fatal's.
 */

#include <start/bios.h>

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

/* Nonzero until the port has timed out once. Initialised to something other
 * than zero on purpose: this file is linked into stage 2 as well, which
 * nothing loads and so nothing clears, and a zero initialiser would put the
 * flag in a .bss the build refuses (falcon-bios/build.mk). */
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

/* ---- teletype and the keyboard, through the BIOS ----
 *
 * INT 10h AH=0Eh is teletype output: it scrolls, wraps and tracks the cursor
 * itself, which is all bios_fatal needs from stage 2, where there is no
 * layout to draw with. Stage 3 draws through screen.c.
 * INT 16h AH=00h blocks for a key and returns its scan code in AH, which is
 * the only way to tell an arrow key from anything else.
 */

static void screen_putc(char c)
{
    thunk_regs.eax = 0x0E00u | (uint8_t)c;   /* AH=0Eh, teletype */
    thunk_regs.ebx = 0x0007u;                /* page 0, grey on black */
    thunk_vector = 0x10;
    thunk_bios();
}

void bios_screen_puts(const char *s)
{
    if (s == (const char *)0)
        return;
    while (*s != '\0') {
        if (*s == '\n') {
            screen_putc('\r');
            bios_putc('\r');
        }
        screen_putc(*s);
        bios_putc(*s);
        s++;
    }
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

/* The ring's bounds are in the BDA at 0x480 and 0x482, as offsets from
 * 0x400, on every machine since the AT; a pair that does not describe a
 * buffer inside the data area gets the PC's fixed 0x41E-0x43E instead. The
 * head and tail are left alone: the ring is empty when they are equal, and
 * it was, or the passphrase prompt would still be reading. */
void bios_key_buffer_wipe(void)
{
    uint16_t start = peek16(0x480u);
    uint16_t end = peek16(0x482u);
    uint32_t a;

    if (start < 0x1Eu || end <= start || end > 0x100u) {
        start = 0x1Eu;
        end = 0x3Eu;
    }
    for (a = 0x400u + start; a < 0x400u + end; a += 2u)
        poke16(a, 0);
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

/* Stage 2 has no screen module and no idle power-off; a stage 3 with one
 * gives the strong definition (console/screen.c). */
__attribute__((weak)) int bios_wait_key(void)
{
    return bios_getkey();
}

__attribute__((noreturn)) void bios_fatal(const char *why)
{
    bios_screen_puts("\nfalcon-bios: ");
    bios_screen_puts(why);
    bios_screen_puts("\npress any key to reboot.\n");
    (void)bios_wait_key();
    bios_reboot();
}
