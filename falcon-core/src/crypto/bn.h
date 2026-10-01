/* falcon-boot: fixed-width modular arithmetic for public-key operations.
 *
 * Purpose. Two consumers, both in the TPM path (DESIGN
 * Section 12.15): the P-256 field under the TPM 2.0 session salt (p256.c), and
 * the RSA public-key operation that carries a TPM 1.2 transport session's
 * secret to the SRK (rsa.c). Neither needs more than modular multiplication,
 * addition, subtraction and exponentiation over a fixed odd modulus.
 *
 * Reasoning. Numbers are little-endian arrays of 32-bit limbs of a length the
 * caller fixes, capped at BN_MAX_LIMBS (2048 bits: a TPM 1.2 storage key is
 * RSA-2048 and nothing here is larger). No allocation: every array is the
 * caller's, and the Montgomery context is a plain struct.
 *
 * Multiplication is Montgomery's (CIOS), because both consumers do long
 * chains of modular products: the P-256 ladder a few thousand, the RSA
 * encryption seventeen, and a Montgomery product avoids a division per step.
 * 32-bit limbs with a 64-bit product keep it portable C that i586 falcon-bios
 * could also compile, should it ever link this.
 *
 * Timing. The multiply, add, subtract and select do the same work whatever
 * the values (the final Montgomery subtraction is masked, not branched), and
 * bn_modexp touches every exponent bit the same way. That is enough for an
 * ECDH private scalar used once per boot; nothing here claims more.
 *
 * Interactions. p256.c and rsa.c; host-tested through them and directly in
 * falcon-core/tests/tpmcrypto_test.c.
 */

#ifndef FALCON_CRYPTO_BN_H
#define FALCON_CRYPTO_BN_H

#include <stdint.h>
#include <stddef.h>

#define BN_MAX_LIMBS 64u        /* 2048 bits */

typedef uint32_t bn_limb;

/* A modulus in Montgomery form. m points at the caller's modulus, which must
 * stay alive and unchanged for as long as the context is used. */
typedef struct {
    const bn_limb *m;
    size_t   n;                 /* limbs in m, and in every operand */
    bn_limb  m0inv;             /* -m^-1 mod 2^32 */
    bn_limb  rr[BN_MAX_LIMBS];  /* R^2 mod m, R = 2^(32n) */
} bn_mont;

/* Big-endian bytes to limbs; CRYPTO_ERR_BAD_ARG when the value needs more
 * than n limbs. Leading zero bytes beyond the width are fine. */
int  bn_from_be(bn_limb *r, size_t n, const uint8_t *in, size_t len);
/* Limbs to exactly len big-endian bytes, zero-padded or truncated at the top
 * (the caller sizes len to the modulus, so nothing significant is cut). */
void bn_to_be(uint8_t *out, size_t len, const bn_limb *a, size_t n);

void bn_zero(bn_limb *r, size_t n);
int  bn_is_zero(const bn_limb *a, size_t n);
/* -1, 0 or 1. Not constant-time; used on public values only. */
int  bn_cmp(const bn_limb *a, const bn_limb *b, size_t n);
/* r = a + b, returns the carry; r = a - b, returns the borrow. */
bn_limb bn_add(bn_limb *r, const bn_limb *a, const bn_limb *b, size_t n);
bn_limb bn_sub(bn_limb *r, const bn_limb *a, const bn_limb *b, size_t n);
/* r = cond ? a : r, and swap a,b when cond, without a branch on cond. */
void bn_select(bn_limb *r, const bn_limb *a, size_t n, bn_limb cond);
void bn_cswap(bn_limb *a, bn_limb *b, size_t n, bn_limb cond);

/* m odd, 0 < n <= BN_MAX_LIMBS, top limb nonzero. */
int  bn_mont_init(bn_mont *c, const bn_limb *m, size_t n);
/* r = a * b / R mod m, operands < m; r may alias either. */
void bn_mont_mul(bn_limb *r, const bn_limb *a, const bn_limb *b,
                 const bn_mont *c);
void bn_to_mont(bn_limb *r, const bn_limb *a, const bn_mont *c);
void bn_from_mont(bn_limb *r, const bn_limb *a, const bn_mont *c);
/* Modular add and subtract of reduced operands (any form, both the same). */
void bn_mod_add(bn_limb *r, const bn_limb *a, const bn_limb *b,
                const bn_mont *c);
void bn_mod_sub(bn_limb *r, const bn_limb *a, const bn_limb *b,
                const bn_mont *c);
/* r = base^exp mod m, all in ordinary (not Montgomery) form, base < m, the
 * exponent big-endian bytes. Square-and-multiply-always. */
void bn_modexp(bn_limb *r, const bn_limb *base, const uint8_t *exp,
               size_t exp_len, const bn_mont *c);

#endif /* FALCON_CRYPTO_BN_H */
