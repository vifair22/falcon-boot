/* falcon-boot: Argon2id's block function on SSE2. See argon2_simd.h.
 *
 * BUILD REQUIREMENT: this translation unit alone is built with -msse2
 * (falcon-bios and the 32-bit mkstick; it is architectural on x86-64), and
 * argon2.c reaches it only when crypto_simd_level says the processor has
 * it. The body is argon2_sse.h's, shared with argon2_ssse3.c. */

#define SIMD_NAME(n) n##_sse2

#include <crypto/argon2_sse.h>
