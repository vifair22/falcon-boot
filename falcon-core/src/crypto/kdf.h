/* falcon-boot: the two TPM 2.0 key derivation functions (TPM 2.0 Part 1
 * Section 11.4.10), both over SHA-256.
 *
 * KDFa is SP 800-108's counter-mode KDF with HMAC as the PRF:
 *
 *   K(i) = HMAC(key, [i]32 || label || 0x00 || contextU || contextV || [bits]32)
 *
 * It makes a session's key from its salt and nonces ("ATH") and the key and
 * IV of each encrypted parameter ("CFB").
 *
 * KDFe is SP 800-56A's concatenation KDF:
 *
 *   K(i) = SHA-256([i]32 || Z || use || 0x00 || partyUInfo || partyVInfo)
 *
 * It turns the ECDH shared x-coordinate into a salted session's salt
 * ("SECRET", with the two public x-coordinates as the party infos).
 *
 * Both labels are C strings and both functions append the terminating zero
 * octet, as the specification counts it. out_bits must be a whole number of
 * bytes, which every TPM use here is.
 *
 * Interactions. falcon-core/src/tpm/tpm2.c; vectors in
 * falcon-core/tests/tpmcrypto_test.c.
 */

#ifndef FALCON_CRYPTO_KDF_H
#define FALCON_CRYPTO_KDF_H

#include <stdint.h>
#include <stddef.h>

int kdfa_sha256(const uint8_t *key, size_t key_len, const char *label,
                const uint8_t *ctx_u, size_t u_len,
                const uint8_t *ctx_v, size_t v_len,
                uint8_t *out, size_t out_bits);

int kdfe_sha256(const uint8_t *z, size_t z_len, const char *use,
                const uint8_t *party_u, size_t u_len,
                const uint8_t *party_v, size_t v_len,
                uint8_t *out, size_t out_bits);

#endif /* FALCON_CRYPTO_KDF_H */
