/* falcon-boot: Argon2id's block function on 128-bit vectors, the body
 * argon2_sse2.c and argon2_ssse3.c both compile (see argon2_simd.h).
 *
 * Included by those two files only, after they define SIMD_NAME(n) to
 * append their level to a name; the one difference between them is the
 * rotations, which SSSE3 does with a byte shuffle where SSE2 needs two
 * shifts.
 *
 * Layout. A block is 128 words, 64 vectors of two. Row r of the 8 x 16
 * matrix is vectors 8r..8r+7, and in every round the 16 words G works on
 * are eight vectors A0 A1 B0 B1 C0 C1 D0 D1, where A0 A1 hold G's four "a"
 * words (v0..v3), B0 B1 the four "b" (v4..v7), and so on. The row round
 * takes a row's eight vectors in order; the column round takes vector i of
 * each of the eight rows, which is word pair i of each row, exactly the
 * sixteen words argon2.c's column permutation gathers. The column step is
 * then two G's on whole vectors, A0 with B0 C0 D0 and A1 with B1 C1 D1. The
 * diagonal step needs b rotated by one word, c by two and d by three, which
 * across two registers is a pair-swap (shufpd): B0 B1 = (b1 b2), (b3 b0),
 * C0 and C1 exchanged, D0 D1 = (d3 d0), (d1 d2).
 */

#ifndef SIMD_NAME
#error "argon2_sse.h is included by argon2_sse2.c and argon2_ssse3.c only"
#endif

#include <crypto/argon2_simd.h>

/* See aesni.c: keeps mm_malloc.h, and the hosted <stdlib.h> behind it, out
 * of a freestanding build. */
#define _MM_MALLOC_H_INCLUDED   /* NOLINT(bugprone-reserved-identifier) */
#define __MM_MALLOC_H           /* NOLINT(bugprone-reserved-identifier) */

#include <emmintrin.h>
#if defined(__SSSE3__)
#include <tmmintrin.h>
#endif

#define LD(p)     _mm_loadu_si128((const __m128i *)(const void *)(p))
#define ST(p, v)  _mm_storeu_si128((__m128i *)(void *)(p), (v))

/* Right rotations of each 64-bit lane. 32 swaps the halves; 63 is a left
 * rotation by one, x + x being the shift. */
#define ROR32(x)  _mm_shuffle_epi32((x), _MM_SHUFFLE(2, 3, 0, 1))
#define ROR63(x)  _mm_xor_si128(_mm_srli_epi64((x), 63),                   \
                                _mm_add_epi64((x), (x)))
#if defined(__SSSE3__)
#define ROR24(x)  _mm_shuffle_epi8((x), _mm_setr_epi8(3, 4, 5, 6, 7, 0, 1, 2, \
                                       11, 12, 13, 14, 15, 8, 9, 10))
#define ROR16(x)  _mm_shuffle_epi8((x), _mm_setr_epi8(2, 3, 4, 5, 6, 7, 0, 1, \
                                       10, 11, 12, 13, 14, 15, 8, 9))
#else
#define ROR24(x)  _mm_xor_si128(_mm_srli_epi64((x), 24),                   \
                                _mm_slli_epi64((x), 40))
#define ROR16(x)  _mm_xor_si128(_mm_srli_epi64((x), 16),                   \
                                _mm_slli_epi64((x), 48))
#endif

/* x + y + 2 * lo32(x) * lo32(y), per lane: fBlaMka. */
#define FBLA(x, y)                                                          \
    _mm_add_epi64(_mm_add_epi64((x), (y)),                                  \
                  _mm_add_epi64(_mm_mul_epu32((x), (y)),                    \
                                _mm_mul_epu32((x), (y))))

/* G on two lanes at once. */
#define G(a, b, c, d)                                                       \
    do {                                                                    \
        (a) = FBLA((a), (b));                                               \
        (d) = ROR32(_mm_xor_si128((d), (a)));                               \
        (c) = FBLA((c), (d));                                               \
        (b) = ROR24(_mm_xor_si128((b), (c)));                               \
        (a) = FBLA((a), (b));                                               \
        (d) = ROR16(_mm_xor_si128((d), (a)));                               \
        (c) = FBLA((c), (d));                                               \
        (b) = ROR63(_mm_xor_si128((b), (c)));                               \
    } while (0)

/* (x1, y0): the high word of x, then the low word of y. */
#define PAIR(x, y)                                                          \
    _mm_castpd_si128(_mm_shuffle_pd(_mm_castsi128_pd(x),                    \
                                    _mm_castsi128_pd(y), 1))

/* Sixteen words, eight vectors at st + 2 * (base + k * stride). */
static inline __attribute__((always_inline)) void
SIMD_NAME(round)(uint64_t *st, size_t base, size_t stride)
{
    uint64_t *p[8];
    __m128i a0, a1, b0, b1, c0, c1, d0, d1, t0, t1;
    size_t k;

    for (k = 0; k < 8u; k++) {
        p[k] = st + 2u * (base + k * stride);
    }
    a0 = LD(p[0]); a1 = LD(p[1]);
    b0 = LD(p[2]); b1 = LD(p[3]);
    c0 = LD(p[4]); c1 = LD(p[5]);
    d0 = LD(p[6]); d1 = LD(p[7]);

    G(a0, b0, c0, d0);
    G(a1, b1, c1, d1);

    /* Diagonalise: b by one word, c by two, d by three. */
    t0 = PAIR(b0, b1);
    t1 = PAIR(b1, b0);
    b0 = t0;
    b1 = t1;
    t0 = c0;
    c0 = c1;
    c1 = t0;
    t0 = PAIR(d1, d0);
    t1 = PAIR(d0, d1);
    d0 = t0;
    d1 = t1;

    G(a0, b0, c0, d0);
    G(a1, b1, c1, d1);

    /* And back. */
    t0 = PAIR(b1, b0);
    t1 = PAIR(b0, b1);
    b0 = t0;
    b1 = t1;
    t0 = c0;
    c0 = c1;
    c1 = t0;
    t0 = PAIR(d0, d1);
    t1 = PAIR(d1, d0);
    d0 = t0;
    d1 = t1;

    ST(p[0], a0); ST(p[1], a1);
    ST(p[2], b0); ST(p[3], b1);
    ST(p[4], c0); ST(p[5], c1);
    ST(p[6], d0); ST(p[7], d1);
}

void SIMD_NAME(argon2_fill)(uint64_t *scratch, const uint64_t *prev,
                            const uint64_t *ref, uint64_t *next, int with_xor)
{
    uint64_t *st = scratch;
    uint64_t *xy = scratch + 128;
    size_t i;

    /* R = prev ^ ref is the state; what the permutation's output is XORed
     * with is R, and from the second pass on R ^ next as well. */
    for (i = 0; i < 64u; i++) {
        __m128i r = _mm_xor_si128(LD(prev + 2u * i), LD(ref + 2u * i));

        ST(st + 2u * i, r);
        ST(xy + 2u * i, with_xor ? _mm_xor_si128(r, LD(next + 2u * i)) : r);
    }
    for (i = 0; i < 8u; i++) {
        SIMD_NAME(round)(st, 8u * i, 1u);     /* row i */
    }
    for (i = 0; i < 8u; i++) {
        SIMD_NAME(round)(st, i, 8u);          /* word pair i of every row */
    }
    for (i = 0; i < 64u; i++) {
        ST(next + 2u * i, _mm_xor_si128(LD(st + 2u * i), LD(xy + 2u * i)));
    }
}

void SIMD_NAME(argon2_wipe)(void)
{
    __asm__ volatile ("pxor %%xmm0, %%xmm0\n\tpxor %%xmm1, %%xmm1\n\t"
                      "pxor %%xmm2, %%xmm2\n\tpxor %%xmm3, %%xmm3\n\t"
                      "pxor %%xmm4, %%xmm4\n\tpxor %%xmm5, %%xmm5\n\t"
                      "pxor %%xmm6, %%xmm6\n\tpxor %%xmm7, %%xmm7"
                      ::: "xmm0", "xmm1", "xmm2", "xmm3",
                          "xmm4", "xmm5", "xmm6", "xmm7");
#if defined(__x86_64__)
    __asm__ volatile ("pxor %%xmm8, %%xmm8\n\tpxor %%xmm9, %%xmm9\n\t"
                      "pxor %%xmm10, %%xmm10\n\tpxor %%xmm11, %%xmm11\n\t"
                      "pxor %%xmm12, %%xmm12\n\tpxor %%xmm13, %%xmm13\n\t"
                      "pxor %%xmm14, %%xmm14\n\tpxor %%xmm15, %%xmm15"
                      ::: "xmm8", "xmm9", "xmm10", "xmm11",
                          "xmm12", "xmm13", "xmm14", "xmm15");
#endif
}
