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

void bios_putc(char c)
{
    while ((inb((uint16_t)(COM1 + 5u)) & LSR_THRE) == 0)
        ;
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
