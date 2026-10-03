/* falcon-boot: the time-stamp counter's arithmetic (docs/DESIGN.md
 * Section 9).
 *
 * The EFI-free half of tsc/: what CPUID's leaves 0x15 and 0x16 say the
 * counter's rate is, what a calibration window measured, ticks to
 * microseconds, a span accumulator for the time spent waiting on the
 * keyboard, and a number as UTF-16 decimal. Every input is a value the
 * firmware half (tsc.c) read, so the host suite reaches every case,
 * including counters near 2^64.
 */

#ifndef FALCON_TSCCORE_H
#define FALCON_TSCCORE_H

#include <stddef.h>
#include <stdint.h>

/* The rates believed. Outside them a CPUID leaf or the firmware's Stall
 * lied, and a timestamp made from that would be worse than none. The floor
 * also keeps tsc_usec exact (see there). */
#define TSC_HZ_MIN 100000000ull      /* 100 MHz */
#define TSC_HZ_MAX 20000000000ull    /* 20 GHz */

int tsc_hz_sane(uint64_t hz);

/* What CPUID said, as tsc.c read it. */
typedef struct {
    uint32_t max_leaf;   /* leaf 0 EAX */
    int      intel;      /* leaf 0 names GenuineIntel */
    uint32_t den;        /* leaf 0x15 EAX: TSC/crystal ratio denominator */
    uint32_t num;        /* leaf 0x15 EBX: numerator */
    uint32_t crystal;    /* leaf 0x15 ECX: crystal clock in Hz, 0 unknown */
    uint32_t base_mhz;   /* leaf 0x16 EAX: base frequency in MHz */
} tsc_cpuid;

/* The rate the leaves give, read as systemd-boot's src/boot/ticks.c reads
 * them: Intel only, leaf 0x15's crystal times its ratio, and where the
 * crystal is not enumerated, leaf 0x16's base frequency taken through the
 * same ratio. 0 when the leaves give no answer, or one outside the sane
 * range; the caller calibrates then. */
uint64_t tsc_hz_cpuid(const tsc_cpuid *c);

/* The rate a calibration window measured: t1 - t0 ticks across a stall of
 * usec microseconds. 0 when the counter did not advance, the window is 0,
 * or the result is not sane. */
uint64_t tsc_hz_window(uint64_t t0, uint64_t t1, uint32_t usec);

/* floor(ticks * 1000000 / hz): the value systemd-boot computes as
 * 1000 * 1000 * ticks / freq, without that product's overflow once the
 * counter passes about 1.8e13 (two hours at 2.6 GHz). 0 when hz is not
 * sane. */
uint64_t tsc_usec(uint64_t ticks, uint64_t hz);

/* Ticks accumulated across spans that may nest: only the outermost span
 * counts, so a wait inside a wait is not counted twice. A close with no
 * open span is ignored, and so is a span the counter ran backwards across. */
typedef struct {
    uint64_t total;
    uint64_t start;
    unsigned depth;
} tsc_span;

void tsc_span_open(tsc_span *s, uint64_t now);
void tsc_span_close(tsc_span *s, uint64_t now);

/* v in decimal, at least minw digits (zero-padded, minw capped at 20), into
 * out as UTF-16 with its terminator. Returns the units written before the
 * terminator, or 0 with out untouched when cap cannot hold them all. 21
 * units always suffice. */
size_t tsc_dec16(uint64_t v, unsigned minw, uint16_t *out, size_t cap);

#endif /* FALCON_TSCCORE_H */
