/* falcon-bios: argon2id's lanes on the other processors. See mp.h. */

#include <mp/mp.h>
#include <mp/mpcore.h>

#include <cpuid.h>

#include <crypto/crypto.h>
#include <crypto/lanes.h>
#include <crypto/xstate.h>
#include <hw/modrt.h>
#include <lib/string.h>
#include <power/acpicore.h>
#include <start/bios.h>

#define MSR_APIC_BASE   0x1Bu
#define APIC_BASE_X2    (1u << 10)
#define APIC_BASE_EN    (1u << 11)
#define LAPIC_SVR       0xF0u
#define LAPIC_ICR_LO    0x300u
#define LAPIC_ICR_HI    0x310u
#define X2_MSR_SVR      0x80Fu
#define X2_MSR_ICR      0x830u
#define SVR_ENABLE      (1u << 8)
#define ICR_INIT        0x00004500u   /* INIT, level assert */
#define ICR_SIPI        0x00004600u   /* startup, level assert, | vector */
#define ICR_BUSY        (1u << 12)    /* xAPIC: delivery pending */
#define MADT_MAX        4096u

/* aptramp.S's page, exactly APT_BYTES: its last .org says so. */
extern const uint8_t aptramp_start[APT_BYTES];

static uint8_t g_stacks[MP_WORKERS_MAX][MP_STACK_BYTES]
    __attribute__((aligned(16)));
static uint8_t g_madt[MADT_MAX];
static mpb_cpu g_cpus[MPB_CPUS_MAX];

static lanes_pool    g_pool;
static argon2_runner g_runner;
static uint32_t      g_ids[MP_WORKERS_MAX];
static uint32_t      g_nids;
static uint32_t      g_out;       /* workers back from mp_ap_main */
static uint32_t      g_lapic;
static uint32_t      g_svr;       /* the SVR as found */
static int           g_x2;
static int           g_started;

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;

    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)v),
                      "d"((uint32_t)(v >> 32)) : "memory");
}

static uint32_t svr_read(void)
{
    return g_x2 ? (uint32_t)rdmsr(X2_MSR_SVR) : peek32(g_lapic + LAPIC_SVR);
}

static void svr_write(uint32_t v)
{
    if (g_x2)
        wrmsr(X2_MSR_SVR, v);
    else
        poke32(g_lapic + LAPIC_SVR, v);
}

/* One IPI to one processor. The xAPIC takes the destination first, and
 * the write of the low half sends; a send waits for the last to be
 * delivered. The x2APIC takes both in one write and needs no wait. */
static void ipi(uint32_t id, uint32_t icr)
{
    uint32_t i;

    if (g_x2) {
        wrmsr(X2_MSR_ICR, ((uint64_t)id << 32) | icr);
        return;
    }
    for (i = 0; i < 1000u && (peek32(g_lapic + LAPIC_ICR_LO) & ICR_BUSY);
         i++)
        io_delay_us(1u);
    poke32(g_lapic + LAPIC_ICR_HI, id << 24);
    poke32(g_lapic + LAPIC_ICR_LO, icr);
}

/* Paging is off and memory is identity-mapped: an address is a pointer. */
static int phys_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    if (addr == 0 || addr > 0xFFFFFFFFu - len)
        return -1;
    memcpy(buf, (const void *)(uintptr_t)addr, len);
    return 0;
}

void mp_ap_main(uint32_t ticket)
{
    xstate_saved xs;

    (void)ticket;
    xstate_borrow(&xs);
    (void)lanes_serve(&g_pool, crypto_simd_level() >= g_pool.level);
    xstate_return(&xs);
    (void)__atomic_add_fetch(&g_out, 1u, __ATOMIC_RELEASE);
}

/* The workers' IDs: every enabled processor of the MADT but the boot one,
 * one a core, at most want. 0 when there are none or the machine cannot
 * be asked. */
static uint32_t find_workers(uint32_t want)
{
    unsigned int eax, ebx, ecx, edx, max;
    uint32_t madt = 0, len = 0, lapic = 0, bsp, shift = 0;
    uint64_t base;
    size_t n = 0;

    /* CPUID leaf 1 EDX bit 9: a local APIC. */
    if (__get_cpuid(0u, &max, &ebx, &ecx, &edx) == 0 ||
        __get_cpuid(1u, &eax, &ebx, &ecx, &edx) == 0 ||
        (edx & (1u << 9)) == 0u)
        return 0;
    base = rdmsr(MSR_APIC_BASE);
    if ((base & APIC_BASE_EN) == 0u || (base >> 32) != 0u)
        return 0;
    g_x2 = (base & APIC_BASE_X2) != 0u;
    g_lapic = (uint32_t)base & 0xFFFFF000u;
    bsp = ebx >> 24;
    if (max >= 0xBu) {
        uint32_t a, b, c, d;

        __cpuid_count(0xBu, 0u, a, b, c, d);
        shift = mpb_smt_shift(max, a, c);
        if (g_x2)
            bsp = d;
    }

    if (acpi_find_table(phys_read, (void *)0, (uint32_t)peek16(0x40Eu) << 4,
                        "APIC", &madt, &len) != ACPI_OK)
        return 0;
    if (len > MADT_MAX)
        len = MADT_MAX;
    if (phys_read((void *)0, madt, g_madt, len) != 0 ||
        mpb_parse_madt(g_madt, len, &lapic, g_cpus, MPB_CPUS_MAX, &n) != 0)
        return 0;
    if (!g_x2 && lapic != 0)
        g_lapic = lapic;
    if (want > MP_WORKERS_MAX)
        want = MP_WORKERS_MAX;
    g_nids = mpb_pick(g_cpus, n, bsp, shift, want, g_ids);
    /* An xAPIC addresses 8-bit IDs only. */
    if (!g_x2) {
        uint32_t i, k = 0;

        for (i = 0; i < g_nids; i++)
            if (g_ids[i] <= 0xFFu)
                g_ids[k++] = g_ids[i];
        g_nids = k;
    }
    return g_nids;
}

/* The start page: the code, then the data mp_ap_main's start needs, and
 * an IDT whose every gate is the halt. */
static void lay_page(void)
{
    uint8_t *page = (uint8_t *)(uintptr_t)APT_ADDR;
    uint32_t halt = APT_ADDR + APT_OFF_HALT;
    uint32_t i;

    memcpy(page, aptramp_start, APT_BYTES);
    __asm__ volatile ("sgdt %0"
                      : "=m"(*(uint8_t (*)[6])(page + APT_OFF_GDTR)));
    for (i = 0; i < APT_IDT_GATES; i++) {
        uint8_t *g = page + APT_OFF_IDT + 8u * i;

        g[0] = (uint8_t)halt;
        g[1] = (uint8_t)(halt >> 8);
        g[2] = SEL_CODE32;
        g[3] = 0;
        g[4] = 0;
        g[5] = 0x8E;                 /* present, ring 0, 32-bit interrupt */
        g[6] = (uint8_t)(halt >> 16);
        g[7] = (uint8_t)(halt >> 24);
    }
    poke16(APT_ADDR + APT_OFF_IDTR, APT_IDT_GATES * 8u - 1u);
    poke32(APT_ADDR + APT_OFF_IDTR + 2u, APT_ADDR + APT_OFF_IDT);
    poke32(APT_ADDR + APT_OFF_ENTRY, (uint32_t)(uintptr_t)mp_ap_main);
    poke32(APT_ADDR + APT_OFF_STACKS, (uint32_t)(uintptr_t)g_stacks);
    poke32(APT_ADDR + APT_OFF_STACKB, MP_STACK_BYTES);
    poke32(APT_ADDR + APT_OFF_NSTACK, g_nids);
    poke32(APT_ADDR + APT_OFF_TICKET, 0);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

const argon2_runner *mp_start(int level, uint32_t want)
{
    uint32_t i;

    g_started = 0;
    g_nids = 0;
    if (want == 0 || find_workers(want) == 0)
        return (const argon2_runner *)0;

    lanes_init(&g_pool, g_nids, level);
    g_runner.run = lanes_run;
    g_runner.self = &g_pool;
    g_out = 0;
    lay_page();

    /* A local APIC software-disabled by the BIOS sends nothing. */
    g_svr = svr_read();
    if ((g_svr & SVR_ENABLE) == 0u)
        svr_write(g_svr | SVR_ENABLE);

    /* INIT, 10 ms, a startup IPI, 200 us, another: the MP specification's
     * sequence, the second for a processor that missed the first. */
    for (i = 0; i < g_nids; i++)
        ipi(g_ids[i], ICR_INIT);
    io_delay_us(10000u);
    for (i = 0; i < g_nids; i++)
        ipi(g_ids[i], ICR_SIPI | APT_VECTOR);
    io_delay_us(200u);
    for (i = 0; i < g_nids; i++)
        ipi(g_ids[i], ICR_SIPI | APT_VECTOR);
    g_started = 1;
    return &g_runner;
}

void mp_stop(void)
{
    uint32_t i, arrived;

    if (!g_started)
        return;
    lanes_stop(&g_pool);
    /* The workers that arrived, back and halted; a second at most, since a
     * worker that is released returns at once. One that arrives after
     * this is caught by the INIT all the same. */
    for (i = 0; i < 1000000u; i++) {
        arrived = peek32(APT_ADDR + APT_OFF_TICKET);
        if (arrived > g_nids)
            arrived = g_nids;
        if (__atomic_load_n(&g_out, __ATOMIC_ACQUIRE) >= arrived)
            break;
        io_delay_us(1u);
    }
    for (i = 0; i < g_nids; i++)
        ipi(g_ids[i], ICR_INIT);
    if ((g_svr & SVR_ENABLE) == 0u)
        svr_write(g_svr);
    g_started = 0;
}

uint32_t mp_count(void)
{
    return 1u + (g_started ? lanes_joined(&g_pool) : 0u);
}
