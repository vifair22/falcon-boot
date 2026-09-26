/* falcon jump: what a module's code may lean on, which is only itself
 * (docs/DESIGN.md Section 14.4, jump/jmod.h).
 *
 * Port I/O and a delay, inline, so a module's resident half needs nothing
 * the core provides: by the time it answers a bootloader's INT 13h call, the
 * core is gone.
 *
 * THE DELAY IS A COUNT OF PORT READS, not a clock. A read of port 0x80 (the
 * POST code port, which no device answers) takes about a microsecond on the
 * ISA-era buses these machines have, and the timeouts below it are generous
 * by orders of magnitude, so an emulator that answers faster only shortens a
 * wait that was never going to run out. The PIT would be a clock, but the
 * BIOS owns it and its mode is not this code's to rely on.
 */

#ifndef FALCON_BIOS_MODRT_H
#define FALCON_BIOS_MODRT_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;

    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t v)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t v;

    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* About `us` microseconds, at least. */
static inline void io_delay_us(uint32_t us)
{
    while (us-- > 0)
        (void)inb(0x80);
}

#endif /* FALCON_BIOS_MODRT_H */
