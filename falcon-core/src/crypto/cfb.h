/* falcon-boot: AES in CFB mode, full-block feedback (SP 800-38A Section 6.3).
 *
 * Purpose. TPM 2.0 parameter encryption (TPM 2.0 Part 1 Section 21.3) is
 * AES-CFB with a 128-bit segment: the one response parameter that carries the
 * TOTP seed travels this way between the TPM and falcon, so a bus interposer
 * sees ciphertext.
 *
 * Reasoning. CFB-128 over aes.c's block function: the keystream for block i
 * is E(previous ciphertext block), so encryption and decryption both run the
 * cipher forward and differ only in which side of the XOR feeds back. A
 * trailing partial block uses the leading bytes of its keystream block, which
 * is what the TPM does with a parameter whose length is not a multiple of 16.
 * in and out may be the same buffer.
 *
 * Interactions. falcon-core/src/tpm/tpm2.c; nothing in the LUKS path.
 */

#ifndef FALCON_CRYPTO_CFB_H
#define FALCON_CRYPTO_CFB_H

#include <stdint.h>
#include <stddef.h>

#include <crypto/aes.h>

void aes_cfb_encrypt(const aes_key *k, const uint8_t iv[AES_BLOCK],
                     const uint8_t *in, uint8_t *out, size_t len);
void aes_cfb_decrypt(const aes_key *k, const uint8_t iv[AES_BLOCK],
                     const uint8_t *in, uint8_t *out, size_t len);

#endif /* FALCON_CRYPTO_CFB_H */
