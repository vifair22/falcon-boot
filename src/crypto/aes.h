/* falcon-boot: AES-128/256 block cipher, portable C (FIPS 197).
 *
 * Purpose. The block cipher under AES-XTS, which is how every byte of p3 is
 * encrypted: both the keyslot area (where the AF-split master key lives) and
 * the data segment (where the UKI lives). LUKS2 with --key-size 512 gives
 * XTS two 256-bit halves, so AES-256 is the size that matters; AES-128 falls
 * out of the same key schedule for free and covers a --key-size 256 volume.
 *
 * Reasoning: no tables. The usual fast AES is four 1 KB T-tables per
 * direction, 8 KB of rodata whose access pattern is key-dependent. This is
 * the *fallback* path (xts.c prefers AES-NI whenever CPUID offers it), so
 * its job is to be small, obviously correct against FIPS 197, and free of a
 * cache-timing side channel rather than to be fast. It uses only the 256-byte
 * S-box and its inverse, with MixColumns computed from xtime. Measured cost
 * is roughly an order of magnitude off the AES-NI path, which is the right
 * trade for code that only runs on pre-2010 hardware and in the host test
 * suite.
 *
 * One schedule serves both directions: aes_decrypt_block walks the forward
 * expanded key backwards and applies the straight inverse cipher (FIPS 197
 * Section 5.3), so there is no second "equivalent inverse cipher" schedule
 * to keep in step. aesni.c does need that other form and derives it itself.
 *
 * Interactions. Used only by xts.c. Nothing outside the crypto module calls
 * a raw block operation.
 */

#ifndef FALCON_CRYPTO_AES_H
#define FALCON_CRYPTO_AES_H

#include <stdint.h>
#include <stddef.h>

#define AES_BLOCK      16u
#define AES_MAX_ROUNDS 14u

typedef struct {
    uint32_t rk[4u * (AES_MAX_ROUNDS + 1u)];  /* forward expanded key */
    uint32_t nr;                              /* 10 or 14 */
} aes_key;

/* key_len is 16 or 32 bytes; anything else is CRYPTO_ERR_UNSUPPORTED. */
int  aes_setkey(aes_key *k, const uint8_t *key, size_t key_len);
void aes_encrypt_block(const aes_key *k, const uint8_t *in, uint8_t *out);
void aes_decrypt_block(const aes_key *k, const uint8_t *in, uint8_t *out);

#endif /* FALCON_CRYPTO_AES_H */
