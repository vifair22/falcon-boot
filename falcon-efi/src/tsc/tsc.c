/* falcon-boot: the time-stamp counter, or AArch64's generic timer. See
 * tsc.h. */

#include <tsc/tsc.h>
#include <tsc/tsccore.h>

/* The calibration window. systemd-boot counts across 1 ms; this counts
 * across 10. The error is the window's absolute slop over its length: the
 * Stall call's overhead and granularity (a microsecond or so, more on a
 * firmware that times Stall off the 3.58 MHz ACPI timer) and anything that
 * lands inside it, an SMI or a timer tick, tens of microseconds. Over 1 ms
 * one tick is a few percent of rate, which over a 30-second boot misplaces
 * the timestamps by about a second; over 10 ms it is a tenth of that, for
 * 10 ms spent once. */
#define TSC_CAL_USEC 10000u

static UINT64 g_hz;
static tsc_hz_how g_how;
static BOOLEAN g_decided;
static tsc_span g_wait;

#if defined(__x86_64__)

UINT64 tsc_read(void)
{
    UINT32 lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((UINT64)hi << 32) | lo;
}

static void cpuid(UINT32 leaf, UINT32 r[4])
{
    __asm__ volatile("cpuid"
                     : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                     : "a"(leaf), "c"(0u));
}

#define MSR_MPERF 0xE7u
#define MSR_APERF 0xE8u

static UINT64 rdmsr(UINT32 msr)
{
    UINT32 lo, hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((UINT64)hi << 32) | lo;
}

int tsc_perf_read(UINT64 *aperf, UINT64 *mperf)
{
    UINT32 r[4];

    cpuid(0, r);
    if (r[0] < 6u)
        return 0;
    cpuid(1u, r);
    if ((r[2] & 0x80000000u) != 0)
        return 0;
    cpuid(6u, r);
    if ((r[2] & 1u) == 0)
        return 0;
    *mperf = rdmsr(MSR_MPERF);
    *aperf = rdmsr(MSR_APERF);
    return 1;
}

static UINT64 hz_from_arch(void)
{
    tsc_cpuid c;
    UINT32 r[4];

    cpuid(0, r);
    c.max_leaf = r[0];
    /* "GenuineIntel", in EBX EDX ECX order. Leaf 0x15 is Intel's; another
     * vendor's leaf of that number means something else. */
    c.intel = r[1] == 0x756e6547u && r[3] == 0x49656e69u &&
              r[2] == 0x6c65746eu;
    c.den = c.num = c.crystal = c.base_mhz = 0;
    if (c.intel && c.max_leaf >= 0x15u) {
        cpuid(0x15u, r);
        c.den = r[0];
        c.num = r[1];
        c.crystal = r[2];
    }
    if (c.intel && c.max_leaf >= 0x16u) {
        cpuid(0x16u, r);
        c.base_mhz = r[0];
    }
    return tsc_hz_cpuid(&c);
}

#define TSC_HZ_ARCH TSC_HZ_CPUID

#elif defined(__aarch64__)

/* The generic timer's virtual count. The isb keeps the read from being
 * taken ahead of the code before it. */
UINT64 tsc_read(void)
{
    UINT64 v;

    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
}

/* The activity monitors (FEAT_AMU) are optional and EL0/EL1 access to them
 * is the firmware's to grant: no effective clock here. */
int tsc_perf_read(UINT64 *aperf, UINT64 *mperf)
{
    (void)aperf;
    (void)mperf;
    return 0;
}

/* CNTFRQ_EL0, which the firmware sets to the counter's rate. It is far
 * below a TSC's (24 MHz on Apple silicon, 62.5 MHz under QEMU), so it is
 * held to its own sanity bound rather than tsccore's. */
static UINT64 hz_from_arch(void)
{
    UINT64 f;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f >= 1000000u && f <= 10000000000ull ? f : 0;
}

#define TSC_HZ_ARCH TSC_HZ_CNTFRQ

#else
#error "tsc.c: no counter for this architecture"
#endif

/* systemd-boot reports no timestamps at all under a hypervisor, on the
 * ground that a guest's counter need not count from reset. This does not
 * skip them: under KVM the guest's counter starts at the guest's own reset,
 * which is what the variables mean, and the rig's proof runs there. */
UINT64 tsc_hz(EFI_BOOT_SERVICES *bs, tsc_hz_how *how)
{
    if (!g_decided) {
        g_decided = TRUE;
        g_hz = hz_from_arch();
        g_how = g_hz != 0 ? TSC_HZ_ARCH : TSC_HZ_NONE;
        if (g_hz == 0 && bs != (EFI_BOOT_SERVICES *)0 &&
            bs->Stall != (void *)0) {
            UINT64 t0 = tsc_read();
            UINT64 t1;

            (void)bs->Stall(TSC_CAL_USEC);
            t1 = tsc_read();
            g_hz = tsc_hz_window(t0, t1, TSC_CAL_USEC);
            g_how = g_hz != 0 ? TSC_HZ_STALL : TSC_HZ_NONE;
        }
    }
    if (how != (tsc_hz_how *)0)
        *how = g_how;
    return g_hz;
}

/* Wall time blocked on input, from the page being up to the key that ends
 * it. Work the loader does meanwhile (the menu's background probe, the
 * TOTP code's refresh) is inside it and is not taken out. */
void tsc_wait_begin(void)
{
    tsc_span_open(&g_wait, tsc_read());
}

void tsc_wait_end(void)
{
    tsc_span_close(&g_wait, tsc_read());
}

UINT64 tsc_wait_ticks(void)
{
    UINT64 now = tsc_read();

    if (g_wait.depth != 0 && now > g_wait.start)
        return g_wait.total + (now - g_wait.start);
    return g_wait.total;
}
