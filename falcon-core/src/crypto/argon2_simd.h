/* falcon-boot: Argon2id's block function in vector instructions.
 *
 * Purpose. The compression G (RFC 9106 Section 3.5) is where an unlock
 * spends its time, and it is BLAKE2b's round on 64-bit words: two, or four,
 * of them fit one vector register and go through the round side by side.
 * argon2.c picks one of these per derivation (crypto_simd_level, or the
 * level its caller names) and keeps its portable fill_block for the rest.
 *
 * Reasoning. One object per instruction set, each compiled with that set's
 * flags and nothing above it, as aesni.c is: the flag lets the compiler use
 * the instructions anywhere in the object, so only code that is never
 * reached on a processor without them may carry it. The probe that decides
 * is crypto.c's, built with the baseline flags.
 *
 * Every function takes the same arguments as the portable path's block
 * function and gives the same block, bit for bit (argon2_test.c runs every
 * level the build host has against the reference's vectors). The working
 * state lives in scratch, 256 words of the caller's, never in a local
 * array: the caller wipes it once a segment, and no alignment is asked of
 * it, since every load and store here is the unaligned form. That is also
 * what keeps the BIOS build safe when nothing promises the stack's
 * alignment, and the mingw build safe from its unaligned 32-byte spills.
 *
 * The wipe functions clear the vector registers the level uses, which hold
 * passphrase-derived state when a segment ends. Their names end in the
 * level, as every function here does, which is what tools/check-floor.sh
 * --userland reads as CPUID-dispatched.
 *
 * Interactions. Called from falcon-core/src/crypto/argon2.c only.
 */

#ifndef FALCON_CRYPTO_ARGON2_SIMD_H
#define FALCON_CRYPTO_ARGON2_SIMD_H

#include <stddef.h>
#include <stdint.h>

/* Words of scratch every level's fill takes: the state and the block the
 * permutation's output is XORed with, 1 KiB each. */
#define ARGON2_SIMD_SCRATCH_WORDS 256u

/* next = G(prev, ref), or next ^= G(prev, ref) when with_xor; 128 words
 * each. prev, ref and next are distinct blocks of the memory. */
typedef void (*argon2_fill_fn)(uint64_t *scratch, const uint64_t *prev,
                               const uint64_t *ref, uint64_t *next,
                               int with_xor);

void argon2_fill_sse2(uint64_t *scratch, const uint64_t *prev,
                      const uint64_t *ref, uint64_t *next, int with_xor);
void argon2_wipe_sse2(void);

void argon2_fill_ssse3(uint64_t *scratch, const uint64_t *prev,
                       const uint64_t *ref, uint64_t *next, int with_xor);
void argon2_wipe_ssse3(void);

void argon2_fill_avx2(uint64_t *scratch, const uint64_t *prev,
                      const uint64_t *ref, uint64_t *next, int with_xor);
void argon2_wipe_avx2(void);

#endif /* FALCON_CRYPTO_ARGON2_SIMD_H */
