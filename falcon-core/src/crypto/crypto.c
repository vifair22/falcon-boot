/* falcon-boot: shared crypto plumbing. See crypto.h. */

#include <crypto/crypto.h>
#include <lib/string.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <cpuid.h>
#endif

/* memset reached through a volatile pointer: the compiler must assume the
 * target can change between the load and the call, so it cannot fold the
 * call away as a dead store even when the buffer dies immediately after.
 * This is the portable barrier; memset_s and explicit_bzero are not
 * available in a freestanding build. */
static void *(*volatile crypto_memset)(void *, int, size_t) = memset;

void crypto_wipe(void *p, size_t n)
{
    if (p != NULL && n != 0) {
        (void)crypto_memset(p, 0, n);
    }
}

int crypto_ct_eq(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    uint8_t diff = 0;
    size_t i;

    if (pa == NULL || pb == NULL) {
        return 0;
    }
    /* Accumulate rather than branch: the loop runs the full length whatever
     * the data is, so the time taken carries no information about where the
     * first mismatch was. */
    for (i = 0; i < n; i++) {
        diff = (uint8_t)(diff | (pa[i] ^ pb[i]));
    }
    return diff == 0;
}

int crypto_has_aesni(void)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;

    /* CPUID leaf 1, ECX bit 25 (AES). aesni.c uses SSE2 shuffles alongside
     * the AES instructions, so bit 25 is the one that gates correctness on
     * x86-64, where SSE2 is architectural.
     *
     * On i686 (falcon-bios, DESIGN Section 13.8) SSE2 is not
     * architectural, so EDX bit 26 is asked as well, though every part
     * with AES-NI has it. */
    if (__get_cpuid(1u, &eax, &ebx, &ecx, &edx) == 0) {
        return 0;
    }
#if defined(__i386__)
    if ((edx & (1u << 26)) == 0u) {
        return 0;
    }
#endif
    return (ecx & (1u << 25)) != 0u;
#else
    return 0;
#endif
}

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
/* XCR0, through XGETBV. Only reached after CPUID has reported OSXSAVE,
 * since the instruction is #UD while CR4.OSXSAVE is clear. */
static uint64_t xcr0_read(void)
{
    uint32_t lo, hi;

    __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0u));
    return ((uint64_t)hi << 32) | lo;
}
#endif

int crypto_simd_level(void)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    int level;

    /* Leaf 1: EDX bit 26 SSE2, ECX bit 9 SSSE3, bit 27 OSXSAVE, bit 28 AVX.
     * On i586 (falcon-bios) SSE2 also means pmode.S has set CR4.OSFXSR,
     * which it does whenever the processor reports FXSR and SSE, and every
     * SSE2 processor reports both. */
    if (__get_cpuid(1u, &eax, &ebx, &ecx, &edx) == 0 ||
        (edx & (1u << 26)) == 0u) {
        return CRYPTO_SIMD_NONE;
    }
    level = CRYPTO_SIMD_SSE2;
    if ((ecx & (1u << 9)) == 0u) {
        return level;
    }
    level = CRYPTO_SIMD_SSSE3;
    /* AVX2: the AVX state enabled (XCR0 bits 1 and 2, SSE and AVX), and
     * leaf 7 EBX bit 5. */
    if ((ecx & (1u << 27)) == 0u || (ecx & (1u << 28)) == 0u ||
        (xcr0_read() & 0x6u) != 0x6u) {
        return level;
    }
    if (__get_cpuid_count(7u, 0u, &eax, &ebx, &ecx, &edx) == 0 ||
        (ebx & (1u << 5)) == 0u) {
        return level;
    }
    return CRYPTO_SIMD_AVX2;
#else
    return CRYPTO_SIMD_NONE;
#endif
}

const char *crypto_simd_name(int level)
{
    switch (level) {
    case CRYPTO_SIMD_NONE:  return "portable";
    case CRYPTO_SIMD_SSE2:  return "sse2";
    case CRYPTO_SIMD_SSSE3: return "ssse3";
    case CRYPTO_SIMD_AVX2:  return "avx2";
    default:                return "?";
    }
}
