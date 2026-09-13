/* airies-boot: shared crypto plumbing. See crypto.h. */

#include <crypto/crypto.h>
#include <lib/string.h>

#if defined(__x86_64__) || defined(_M_X64)
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
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;

    /* CPUID leaf 1, ECX bit 25 (AES) and bit 19 (SSE4.1). aesni.c is built
     * with -maes -msse2 -mssse3 and uses only SSE2/SSSE3 shuffles alongside
     * the AES instructions, so bit 25 is the one that gates correctness;
     * SSE2 is architectural on x86-64 and SSSE3 is implied by every part
     * that has AES-NI, so neither needs its own probe. */
    if (__get_cpuid(1u, &eax, &ebx, &ecx, &edx) == 0) {
        return 0;
    }
    return (ecx & (1u << 25)) != 0u;
#else
    return 0;
#endif
}
