/* falcon jump: what a module's code may lean on, which is only itself
 * (docs/DESIGN.md Section 13.4, jump/jmod.h).
 *
 * Port I/O, low memory and a delay, inline, so a module's resident half needs nothing
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

static inline void outl(uint16_t port, uint32_t v)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(port));
}

/* A pointer to absolute low memory (the BIOS data area), laundered through
 * asm as start/bios.h's peek and poke are: a compiler takes a dereference
 * of a constant near zero for a null-pointer bug and warns that it is one. */
static inline uint8_t *low_mem(uint32_t addr)
{
    uint8_t *p;

    __asm__("" : "=r"(p) : "0"((uintptr_t)addr));
    return p;
}

/* The core's tick (jmod_services.tick) while a module probes: a module
 * sets it at init and drops it where it drops the services (serve, or a
 * resident's first call), so a resident's waits never call into a core
 * that is gone. NULL in the core's own copy. Defined in hw/modrt.c. */
extern void (*modrt_tick)(void);

/* About `us` microseconds, at least; the core's spinner turns at each call
 * and every millisecond of a long one, so a probe's polling and its long
 * settles are seen to move. */
static inline void io_delay_us(uint32_t us)
{
    uint32_t n = 0;

    if (modrt_tick != (void (*)(void))0)
        modrt_tick();
    while (us-- > 0) {
        (void)inb(0x80);
        if (++n == 1000u) {
            n = 0;
            if (modrt_tick != (void (*)(void))0)
                modrt_tick();
        }
    }
}

#endif /* FALCON_BIOS_MODRT_H */
