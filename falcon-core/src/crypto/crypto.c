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
     * On i686 (falcon-bios, docs/notes/bios-route.md Section 6h) SSE2 is
     * not architectural, so EDX bit 26 is asked as well, though every part
     * with AES-NI has it. This probe was x86-64 only until a benchmark of
     * the p3 unlock showed the i686 loader on the portable path on a CPU
     * with AES-NI: 229 ms against 197. */
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
