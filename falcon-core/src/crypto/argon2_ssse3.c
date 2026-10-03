/* falcon-boot: Argon2id's block function on SSSE3. See argon2_simd.h.
 *
 * BUILD REQUIREMENT: this translation unit alone is built with -mssse3,
 * and argon2.c reaches it only when crypto_simd_level says the processor
 * has it. The body is argon2_sse.h's, shared with argon2_sse2.c; SSSE3's
 * byte shuffle is what it adds, for the rotations by 16 and 24. */

#define SIMD_NAME(n) n##_ssse3

#include <crypto/argon2_sse.h>
