/* falcon jump's drivers: PCI configuration space through the PC's
 * mechanism #1, ports 0xCF8 and 0xCFC (DESIGN Section 13.4).
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

/* A pci_loc with this fn is before the first function: what pci_next
 * starts from. */
#define PCI_FN_START 0xFFu

/* Advances `at` to the next function present on any bus, in bus, device,
 * function order, from the sentinel {0, 0, PCI_FN_START} or from the
 * function it names. Functions 1-7 of a device are visited only when its
 * function 0 says multi-function (header type bit 7): a single-function
 * device answers every function number with its own registers. 0 with
 * `at` naming a function, -1 past the last. A whole walk is a few thousand
 * port reads, a few milliseconds on the machines Section 13 is for; walk
 * once and keep what is wanted rather than once per thing wanted. */
static inline int pci_next(pci_loc *at)
{
    unsigned bus = at->bus;
    unsigned dev = at->dev;
    unsigned fn = at->fn == PCI_FN_START ? 0u : (unsigned)at->fn + 1u;

    for (; bus < 256u; bus++, dev = 0) {
        for (; dev < 32u; dev++, fn = 0) {
            for (; fn < 8u; fn++) {
                uint32_t id;

                if (fn != 0 && (pci_read32((uint8_t)bus, (uint8_t)dev, 0,
                                           0x0C) & 0x00800000u) == 0)
                    break;
                id = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn,
                                0x00);
                if ((id & 0xFFFFu) == 0xFFFFu) {
                    if (fn == 0)
                        break;
                    continue;
                }
                at->bus = (uint8_t)bus;
                at->dev = (uint8_t)dev;
                at->fn = (uint8_t)fn;
                return 0;
            }
        }
    }
    return -1;
}

/* The `nth` function (from 0) whose class code, masked with `mask`, is
 * `cls` (class << 16 | subclass << 8 | programming interface); 0 when there
 * is one. A whole walk each call (pci_next): call it once per thing
 * wanted, never once per programming interface. (It was, for IDE, and 128
 * passes cost the ThinkPad A21m about two seconds before its menu.) */
static inline int pci_find_class_mask(uint32_t cls, uint32_t mask,
                                      unsigned nth, pci_loc *out)
{
    pci_loc at;

    at.bus = 0;
    at.dev = 0;
    at.fn = PCI_FN_START;
    while (pci_next(&at) == 0) {
        uint32_t cc = pci_read32(at.bus, at.dev, at.fn, 0x08) >> 8;

        if ((cc & mask) == cls && nth-- == 0) {
            *out = at;
            return 0;
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
