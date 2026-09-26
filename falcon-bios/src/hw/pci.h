/* falcon jump's drivers: PCI configuration space through the PC's
 * mechanism #1, ports 0xCF8 and 0xCFC (DESIGN Section 14.4).
 *
 * A module finds its own controller, so it needs no word from the core
 * about where the controller is. Inline, as modrt.h is: nothing here may be
 * a call into the core.
 */

#ifndef FALCON_BIOS_PCI_H
#define FALCON_BIOS_PCI_H

#include <stdint.h>

#include <hw/modrt.h>

static inline uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t fn,
                                uint8_t reg)
{
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)(dev & 0x1Fu) << 11) |
           ((uint32_t)(fn & 7u) << 8) | (uint32_t)(reg & 0xFCu);
}

static inline uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn,
                                  uint8_t reg)
{
    uint32_t v;

    __asm__ __volatile__("outl %0, %1" : : "a"(pci_addr(bus, dev, fn, reg)),
                         "Nd"((uint16_t)0xCF8));
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    return v;
}

static inline void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn,
                               uint8_t reg, uint32_t v)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(pci_addr(bus, dev, fn, reg)),
                         "Nd"((uint16_t)0xCF8));
    __asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"((uint16_t)0xCFC));
}

static inline void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn,
                               uint8_t reg, uint16_t v)
{
    uint32_t old = pci_read32(bus, dev, fn, reg);
    uint32_t sh = (uint32_t)(reg & 2u) * 8u;

    pci_write32(bus, dev, fn, reg,
                (old & ~(0xFFFFu << sh)) | ((uint32_t)v << sh));
}

typedef struct {
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
} pci_loc;

/* The `nth` function (from 0) whose class code, masked with `mask`, is
 * `cls` (class << 16 | subclass << 8 | programming interface); 0 when there
 * is one. Every bus is tried, which costs a few thousand port reads, a few
 * milliseconds on the machines Section 14 is for: call it once per thing
 * wanted, never once per programming interface. (It was, for IDE, and 128
 * passes cost the ThinkPad A21m about two seconds before its menu.) */
static inline int pci_find_class_mask(uint32_t cls, uint32_t mask,
                                      unsigned nth, pci_loc *out)
{
    unsigned bus;
    unsigned dev;
    unsigned fn;

    for (bus = 0; bus < 256u; bus++) {
        for (dev = 0; dev < 32u; dev++) {
            for (fn = 0; fn < 8u; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev,
                                         (uint8_t)fn, 0x00);
                uint32_t cc;

                if ((id & 0xFFFFu) == 0xFFFFu) {
                    if (fn == 0)
                        break;
                    continue;
                }
                cc = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn,
                                0x08) >> 8;
                if ((cc & mask) == cls && nth-- == 0) {
                    out->bus = (uint8_t)bus;
                    out->dev = (uint8_t)dev;
                    out->fn = (uint8_t)fn;
                    return 0;
                }
                /* A single-function device answers every function number
                 * with its own registers. */
                if (fn == 0 && (pci_read32((uint8_t)bus, (uint8_t)dev, 0,
                                           0x0C) & 0x00800000u) == 0)
                    break;
            }
        }
    }
    return -1;
}

/* An exact class code, programming interface included. */
static inline int pci_find_class(uint32_t cls, unsigned nth, pci_loc *out)
{
    return pci_find_class_mask(cls, 0xFFFFFFu, nth, out);
}

#endif /* FALCON_BIOS_PCI_H */
