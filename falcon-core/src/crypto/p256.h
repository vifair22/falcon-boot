/* falcon-boot: NIST P-256 key agreement (SP 800-56A ECC CDH).
 *
 * Purpose. A TPM 2.0 session salted to an ECC key (TPM 2.0 Part 1 Section
 * C.6.1): falcon makes an ephemeral key pair, sends its public point with
 * the session's start, and derives the salt from the x-coordinate of the
 * shared point. The TPM does the same with its private key. A bus
 * interposer sees two public points and learns nothing it can turn into the
 * session key that encrypts the TOTP seed on its way out.
 *
 * Reasoning. Coordinates live in Montgomery form over bn.c; points are
 * Jacobian so the ladder needs one inversion, at the end. The scalar
 * multiplication is a Montgomery ladder with masked swaps: every bit costs
 * one addition and one doubling whatever its value. The addition's special
 * cases are handled by masked selects rather than branches, and the one the
 * ladder never reaches (adding a point to itself) is argued at its site.
 *
 * The private scalar is never generated here: the caller hands in 32 random
 * bytes (EFI_RNG_PROTOCOL, the TPM's own GetRandom, or getrandom(2)), and a
 * value outside [1, n-1] is refused rather than reduced, so the caller draws
 * again. The chance of that is about 2^-32.
 *
 * Every peer point is validated before use (on the curve, coordinates below
 * p). P-256 has cofactor 1, so a point on the curve that is not the identity
 * has order n and there is no small-subgroup case to check.
 *
 * Interactions. falcon-core/src/tpm/tpm2.c (the salt); host-tested with
 * OpenSSL-derived vectors in falcon-core/tests/tpmcrypto_test.c.
 */

#ifndef FALCON_CRYPTO_P256_H
#define FALCON_CRYPTO_P256_H

#include <stdint.h>
#include <stddef.h>

#define P256_BYTES 32u

/* CRYPTO_OK when (x, y) is a point on the curve with coordinates below p,
 * CRYPTO_ERR_BAD_ARG otherwise. */
int p256_check_point(const uint8_t x[P256_BYTES], const uint8_t y[P256_BYTES]);

/* The public point of private scalar d; CRYPTO_ERR_BAD_ARG when d is 0 or
 * not below the group order. */
int p256_public(const uint8_t d[P256_BYTES],
                uint8_t x[P256_BYTES], uint8_t y[P256_BYTES]);

/* The x-coordinate of d * (px, py), after validating the point. */
int p256_ecdh(const uint8_t d[P256_BYTES],
              const uint8_t px[P256_BYTES], const uint8_t py[P256_BYTES],
              uint8_t zx[P256_BYTES]);

#endif /* FALCON_CRYPTO_P256_H */
