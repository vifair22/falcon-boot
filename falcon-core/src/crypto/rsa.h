/* falcon-boot: RSA public-key encryption, RSAES-OAEP with SHA-1 and MGF1
 * (RFC 8017 Section 7.1), and MGF1 on its own.
 *
 * Purpose. A TPM 1.2 transport session (TPM Main Part 3 Section 24.1)
 * starts with a secret only the TPM can read: falcon encrypts it to a TPM
 * storage key, and TPM 1.2 fixes the scheme as TPM_ES_RSAESOAEP_SHA1_MGF1
 * with the encoding parameter "TCPA". The transport's own encryption is
 * TPM_ALG_MGF1, an XOR with MGF1-SHA1 output, which is why MGF1 is public.
 *
 * Reasoning. Public-key operation only: the exponent is public, so
 * bn_modexp's cost is a few multiplications and nothing here holds a private
 * key. The OAEP seed is the caller's random bytes, never generated here
 * (see p256.h for why), which also makes the encryption reproducible under
 * test.
 *
 * Interactions. falcon-core/src/tpm/tpm12.c; vectors in
 * falcon-core/tests/tpmcrypto_test.c, decrypted by OpenSSL when they were
 * made.
 */

#ifndef FALCON_CRYPTO_RSA_H
#define FALCON_CRYPTO_RSA_H

#include <stdint.h>
#include <stddef.h>

#define RSA_MAX_BYTES 256u      /* 2048-bit moduli */

/* XORs MGF1-SHA1(seed) into buf, len bytes. */
void mgf1_sha1_xor(const uint8_t *seed, size_t seed_len,
                   uint8_t *buf, size_t len);

/* Encrypts msg to (mod, exp), both big-endian, writing mod_len bytes to out.
 * seed is 20 random bytes. CRYPTO_ERR_BAD_ARG when the modulus is even, too
 * large, or too small for the message. */
int rsa_oaep_sha1_encrypt(const uint8_t *mod, size_t mod_len,
                          const uint8_t *exp, size_t exp_len,
                          const uint8_t *label, size_t label_len,
                          const uint8_t *msg, size_t msg_len,
                          const uint8_t seed[20], uint8_t *out);

#endif /* FALCON_CRYPTO_RSA_H */
