/* falcon-boot: the AVX register state, borrowed and given back. See
 * xstate.h. Built with the baseline flags: nothing here may be an
 * instruction the processor lacks before CPUID has said it has it. */

#include <crypto/xstate.h>

#include <cpuid.h>

#define CR4_OSXSAVE   ((uintptr_t)1 << 18)
#define XCR0_X87      0x1u
#define XCR0_SSE      0x2u
#define XCR0_AVX      0x4u
#define XCR0_WANT     (XCR0_X87 | XCR0_SSE | XCR0_AVX)
/* What XCR0 holds after reset, which is what a borrow that found OSXSAVE
 * clear leaves it as before clearing OSXSAVE again. */
#define XCR0_RESET    XCR0_X87

static uintptr_t cr4_read(void)
{
    uintptr_t v;

    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

static void cr4_write(uintptr_t v)
{
    __asm__ volatile ("mov %0, %%cr4" : : "r"(v) : "memory");
}

static uint64_t xcr0_read(void)
{
    uint32_t lo, hi;

    __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0u));
    return ((uint64_t)hi << 32) | lo;
}

static void xcr0_write(uint64_t v)
{
    __asm__ volatile ("xsetbv" : : "a"((uint32_t)v),
                      "d"((uint32_t)(v >> 32)), "c"(0u) : "memory");
}

void xstate_borrow(xstate_saved *s)
{
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;

    s->cr4 = 0;
    s->xcr0 = 0;
    s->changed = 0;
    /* Leaf 1 ECX: bit 26 XSAVE (XSETBV and XCR0 exist), bit 28 AVX. Leaf
     * 0Dh sub-leaf 0 EAX: the XCR0 bits this processor will take. */
    if (__get_cpuid(1u, &eax, &ebx, &ecx, &edx) == 0 ||
        (ecx & (1u << 26)) == 0u || (ecx & (1u << 28)) == 0u) {
        return;
    }
    if (__get_cpuid_count(0xDu, 0u, &eax, &ebx, &ecx, &edx) == 0 ||
        (eax & XCR0_WANT) != XCR0_WANT) {
        return;
    }

    s->cr4 = cr4_read();
    if ((s->cr4 & CR4_OSXSAVE) != 0u) {
        s->xcr0 = xcr0_read();
        if ((s->xcr0 & XCR0_WANT) == XCR0_WANT) {
            return;                     /* on already: the firmware's */
        }
    } else {
        cr4_write(s->cr4 | CR4_OSXSAVE);
        s->xcr0 = XCR0_RESET;
    }
    xcr0_write(s->xcr0 | XCR0_WANT);
    s->changed = 1;
}

void xstate_return(const xstate_saved *s)
{
    if (!s->changed) {
        return;
    }
    /* The clobbers matter on x86-64, where the MS ABI has the caller keep
     * xmm6..15 across a call; falcon-bios is built with no SSE registers
     * for the compiler to keep anything in. */
#if defined(__x86_64__)
    __asm__ volatile ("vzeroall" : : : "memory",
                      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
                      "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
                      "xmm12", "xmm13", "xmm14", "xmm15");
#else
    __asm__ volatile ("vzeroall" : : : "memory");
#endif
    xcr0_write(s->xcr0);
    if ((s->cr4 & CR4_OSXSAVE) == 0u) {
        cr4_write(s->cr4);
    }
}
