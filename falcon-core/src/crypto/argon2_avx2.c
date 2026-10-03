/* falcon-boot: Argon2id's block function on AVX2. See argon2_simd.h.
 *
 * BUILD REQUIREMENT: this translation unit alone is built with -mavx2, and
 * argon2.c reaches it only when crypto_simd_level says the processor has it
 * and its state is enabled. On mingw it is also assembled with
 * -muse-unaligned-vector-move: GCC for Windows targets cannot realign the
 * stack to 32 bytes (GCC PR 54412), so a spilled ymm register would be an
 * aligned move to an address that is not, and a #GP in the firmware.
 *
 * Layout. A block is 32 vectors of four words; row r of the 8 x 16 matrix
 * is vectors 4r..4r+3, which are exactly G's a, b, c and d words for that
 * row, so the row round takes two rows at a time, one in each of A0..D0 and
 * A1..D1, and the diagonal step is a rotation of b, c and d inside each
 * register (vpermq).
 *
 * The column round takes vector i of rows 0..7 (four words: word pairs 2i
 * and 2i+1 of each row) as A0 A1 B0 B1 C0 C1 D0 D1, which puts G's sixteen
 * words for column pair 2i in the low halves and those for 2i+1 in the high
 * halves, laid out as argon2_sse.h lays out one pair. Its diagonal step is
 * that file's pair-swap done per half: a blend picks the word from the other
 * register and a swap of the two words in each half puts it in place.
 */

#include <crypto/argon2_simd.h>

/* See aesni.c: keeps mm_malloc.h, and the hosted <stdlib.h> behind it, out
 * of a freestanding build. */
#define _MM_MALLOC_H_INCLUDED   /* NOLINT(bugprone-reserved-identifier) */
#define __MM_MALLOC_H           /* NOLINT(bugprone-reserved-identifier) */

#include <immintrin.h>

#define LD(p)     _mm256_loadu_si256((const __m256i *)(const void *)(p))
#define ST(p, v)  _mm256_storeu_si256((__m256i *)(void *)(p), (v))

#define ROR32(x)  _mm256_shuffle_epi32((x), _MM_SHUFFLE(2, 3, 0, 1))
#define ROR24(x)  _mm256_shuffle_epi8((x), _mm256_setr_epi8(                  \
                      3, 4, 5, 6, 7, 0, 1, 2, 11, 12, 13, 14, 15, 8, 9, 10,   \
                      3, 4, 5, 6, 7, 0, 1, 2, 11, 12, 13, 14, 15, 8, 9, 10))
#define ROR16(x)  _mm256_shuffle_epi8((x), _mm256_setr_epi8(                  \
                      2, 3, 4, 5, 6, 7, 0, 1, 10, 11, 12, 13, 14, 15, 8, 9,   \
                      2, 3, 4, 5, 6, 7, 0, 1, 10, 11, 12, 13, 14, 15, 8, 9))
#define ROR63(x)  _mm256_xor_si256(_mm256_srli_epi64((x), 63),               \
                                   _mm256_add_epi64((x), (x)))

#define FBLA(x, y)                                                          \
    _mm256_add_epi64(_mm256_add_epi64((x), (y)),                            \
                     _mm256_add_epi64(_mm256_mul_epu32((x), (y)),           \
                                      _mm256_mul_epu32((x), (y))))

#define G(a, b, c, d)                                                       \
    do {                                                                    \
        (a) = FBLA((a), (b));                                               \
        (d) = ROR32(_mm256_xor_si256((d), (a)));                            \
        (c) = FBLA((c), (d));                                               \
        (b) = ROR24(_mm256_xor_si256((b), (c)));                            \
        (a) = FBLA((a), (b));                                               \
        (d) = ROR16(_mm256_xor_si256((d), (a)));                            \
        (c) = FBLA((c), (d));                                               \
        (b) = ROR63(_mm256_xor_si256((b), (c)));                            \
    } while (0)

/* Lane j takes lane j+1 (ROT1), j+2 (ROT2) or j+3 (ROT3) of the same
 * register. */
#define ROT1(x)   _mm256_permute4x64_epi64((x), _MM_SHUFFLE(0, 3, 2, 1))
#define ROT2(x)   _mm256_permute4x64_epi64((x), _MM_SHUFFLE(1, 0, 3, 2))
#define ROT3(x)   _mm256_permute4x64_epi64((x), _MM_SHUFFLE(2, 1, 0, 3))

/* The two words of each 128-bit half exchanged. */
#define SWAP(x)   _mm256_shuffle_epi32((x), _MM_SHUFFLE(1, 0, 3, 2))
/* Words 0 and 2 from y, 1 and 3 from x (EVEN); or 1 and 3 from y (ODD). */
#define EVEN(x, y) _mm256_blend_epi32((x), (y), 0x33)
#define ODD(x, y)  _mm256_blend_epi32((x), (y), 0xCC)

/* Rows 2i and 2i+1: vectors 8i..8i+3 and 8i+4..8i+7. */
static inline __attribute__((always_inline)) void
row_round_avx2(uint64_t *st, size_t i)
{
    uint64_t *p = st + 32u * i;
    __m256i a0 = LD(p),      b0 = LD(p + 4),  c0 = LD(p + 8),  d0 = LD(p + 12);
    __m256i a1 = LD(p + 16), b1 = LD(p + 20), c1 = LD(p + 24), d1 = LD(p + 28);

    G(a0, b0, c0, d0);
    G(a1, b1, c1, d1);
    b0 = ROT1(b0); c0 = ROT2(c0); d0 = ROT3(d0);
    b1 = ROT1(b1); c1 = ROT2(c1); d1 = ROT3(d1);
    G(a0, b0, c0, d0);
    G(a1, b1, c1, d1);
    b0 = ROT3(b0); c0 = ROT2(c0); d0 = ROT1(d0);
    b1 = ROT3(b1); c1 = ROT2(c1); d1 = ROT1(d1);

    ST(p, a0);      ST(p + 4, b0);  ST(p + 8, c0);  ST(p + 12, d0);
    ST(p + 16, a1); ST(p + 20, b1); ST(p + 24, c1); ST(p + 28, d1);
}

/* Vector i of every row: word pairs 2i and 2i+1 of rows 0..7. */
static inline __attribute__((always_inline)) void
col_round_avx2(uint64_t *st, size_t i)
{
    uint64_t *p = st + 4u * i;
    __m256i a0 = LD(p),       a1 = LD(p + 16);
    __m256i b0 = LD(p + 32),  b1 = LD(p + 48);
    __m256i c0 = LD(p + 64),  c1 = LD(p + 80);
    __m256i d0 = LD(p + 96),  d1 = LD(p + 112);
    __m256i t0, t1;

    G(a0, b0, c0, d0);
    G(a1, b1, c1, d1);

    /* Diagonalise, per half: B0 B1 = (b1 b2), (b3 b0); C0 C1 exchanged;
     * D0 D1 = (d3 d0), (d1 d2). */
    t0 = SWAP(EVEN(b0, b1));
    t1 = SWAP(ODD(b0, b1));
    b0 = t0;
    b1 = t1;
    t0 = c0;
    c0 = c1;
    c1 = t0;
    t0 = SWAP(ODD(d0, d1));
    t1 = SWAP(EVEN(d0, d1));
    d0 = t0;
    d1 = t1;

    G(a0, b0, c0, d0);
    G(a1, b1, c1, d1);

    /* And back. */
    t0 = ODD(SWAP(b1), SWAP(b0));
    t1 = ODD(SWAP(b0), SWAP(b1));
    b0 = t0;
    b1 = t1;
    t0 = c0;
    c0 = c1;
    c1 = t0;
    t0 = ODD(SWAP(d0), SWAP(d1));
    t1 = ODD(SWAP(d1), SWAP(d0));
    d0 = t0;
    d1 = t1;

    ST(p, a0);      ST(p + 16, a1);
    ST(p + 32, b0); ST(p + 48, b1);
    ST(p + 64, c0); ST(p + 80, c1);
    ST(p + 96, d0); ST(p + 112, d1);
}

void argon2_fill_avx2(uint64_t *scratch, const uint64_t *prev,
                      const uint64_t *ref, uint64_t *next, int with_xor)
{
    uint64_t *st = scratch;
    uint64_t *xy = scratch + 128;
    size_t i;

    for (i = 0; i < 32u; i++) {
        __m256i r = _mm256_xor_si256(LD(prev + 4u * i), LD(ref + 4u * i));

        ST(st + 4u * i, r);
        ST(xy + 4u * i,
           with_xor ? _mm256_xor_si256(r, LD(next + 4u * i)) : r);
    }
    for (i = 0; i < 4u; i++) {
        row_round_avx2(st, i);
    }
    for (i = 0; i < 4u; i++) {
        col_round_avx2(st, i);
    }
    for (i = 0; i < 32u; i++) {
        ST(next + 4u * i,
           _mm256_xor_si256(LD(st + 4u * i), LD(xy + 4u * i)));
    }
}

/* Every ymm register to zero, which also leaves the upper halves clean for
 * whatever SSE code runs next. */
void argon2_wipe_avx2(void)
{
    _mm256_zeroall();
}
