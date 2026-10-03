/* falcon-boot: the time-stamp counter's arithmetic. See tsccore.h. */

#include <tsc/tsccore.h>

int tsc_hz_sane(uint64_t hz)
{
    return hz >= TSC_HZ_MIN && hz <= TSC_HZ_MAX;
}

/* floor(a * b / c) for b and c that fit 32 bits: the remainder's product
 * stays under 2^64. A result that would not fit is UINT64_MAX, which no
 * caller takes for a sane rate, rather than a wrapped value that might be. */
static uint64_t muldiv32(uint64_t a, uint32_t b, uint32_t c)
{
    uint64_t q = a / c;

    if (b != 0 && q > (UINT64_MAX - b) / b)
        return UINT64_MAX;
    return q * b + (a % c) * b / c;
}

uint64_t tsc_hz_cpuid(const tsc_cpuid *c)
{
    uint64_t crystal;
    uint64_t hz;

    if (c == (const tsc_cpuid *)0 || !c->intel || c->max_leaf < 0x15u)
        return 0;
    if (c->den == 0 || c->num == 0)
        return 0;
    crystal = c->crystal;
    /* Most client parts enumerate the ratio but not the crystal.
     * systemd-boot then derives the crystal from leaf 0x16's base frequency
     * through the ratio and multiplies back, and this rounds as it does. */
    if (crystal == 0 && c->max_leaf >= 0x16u)
        crystal = muldiv32((uint64_t)(c->base_mhz & 0xFFFFu) * 1000000u,
                           c->den, c->num);
    if (crystal == UINT64_MAX)
        return 0;
    hz = muldiv32(crystal, c->num, c->den);
    return tsc_hz_sane(hz) ? hz : 0;
}

uint64_t tsc_hz_window(uint64_t t0, uint64_t t1, uint32_t usec)
{
    uint64_t hz;

    if (usec == 0 || t1 <= t0)
        return 0;
    hz = muldiv32(t1 - t0, 1000000u, usec);
    return tsc_hz_sane(hz) ? hz : 0;
}

uint64_t tsc_usec(uint64_t ticks, uint64_t hz)
{
    /* Whole seconds, then the rest. q * 10^6 <= ticks because hz > 10^6,
     * and r * 10^6 < hz * 10^6 < 2^64 for any sane hz, so neither product
     * overflows and the sum is the exact floor. */
    if (!tsc_hz_sane(hz))
        return 0;
    return (ticks / hz) * 1000000u + (ticks % hz) * 1000000u / hz;
}

uint32_t tsc_eff_mhz(uint64_t hz, uint64_t daperf, uint64_t dmperf)
{
    uint64_t q, r;

    if (!tsc_hz_sane(hz) || daperf == 0 || dmperf == 0)
        return 0;
    /* hz is under 2^35, so r * hz stays under 2^63 while dmperf is under
     * 2^28. Halving both deltas keeps their ratio. */
    while (dmperf >= TSC_PERF_SPAN) {
        daperf >>= 1;
        dmperf >>= 1;
    }
    q = daperf / dmperf;
    r = daperf % dmperf;
    if (q >= TSC_PERF_RATIO_MAX)
        return 0;
    return (uint32_t)((hz * q + hz * r / dmperf) / 1000000u);
}

void tsc_span_open(tsc_span *s, uint64_t now)
{
    if (s->depth++ == 0)
        s->start = now;
}

void tsc_span_close(tsc_span *s, uint64_t now)
{
    if (s->depth == 0)
        return;
    if (--s->depth == 0 && now > s->start)
        s->total += now - s->start;
}

size_t tsc_dec16(uint64_t v, unsigned minw, uint16_t *out, size_t cap)
{
    uint16_t tmp[20];
    size_t n = 0;
    size_t i;

    if (minw > 20u)
        minw = 20u;
    do {
        tmp[n++] = (uint16_t)(0x30u + (unsigned)(v % 10u));
        v /= 10u;
    } while (v != 0);
    while (n < minw)
        tmp[n++] = 0x30u;
    if (out == (uint16_t *)0 || cap < n + 1u)
        return 0;
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1u - i];
    out[n] = 0;
    return n;
}
