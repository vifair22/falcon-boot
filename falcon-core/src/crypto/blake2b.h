/* falcon-boot: BLAKE2b (RFC 7693).
 *
 * Purpose. The hash Argon2 is built on (RFC 9106 Section 3.2): H0, the
 * variable-length H' that fills a lane's first two blocks and produces the
 * tag, all of it BLAKE2b at a chosen output length. Nothing else in the
 * repository uses it, and nothing registers it in hash.c: a LUKS2 header
 * never names blake2b for a keyslot, an AF splitter or a digest, so the
 * descriptor table stays the list of hashes a header can ask for.
 *
 * Reasoning. Keyed hashing and every output length from 1 to 64 are here
 * although Argon2 needs only unkeyed output lengths up to 64: they are the
 * same few lines, and RFC 7693's own vectors exercise them, which is what
 * pins the parameter block rather than trusting it.
 *
 * Interactions. Freestanding like the rest of crypto/: no allocation, the
 * context is the caller's. Called by argon2.c.
 */

#ifndef FALCON_CRYPTO_BLAKE2B_H
#define FALCON_CRYPTO_BLAKE2B_H

#include <stdint.h>
#include <stddef.h>

#define BLAKE2B_OUT_MAX  64u
#define BLAKE2B_KEY_MAX  64u
#define BLAKE2B_BLOCK    128u

typedef struct {
    uint64_t h[8];
    uint64_t t[2];                /* bytes compressed, 128-bit count */
    uint32_t buflen;              /* bytes held in buf, 0..128 */
    uint32_t outlen;
    uint8_t  buf[BLAKE2B_BLOCK];
} blake2b_ctx;

/* outlen 1..64, keylen 0..64 (key may be NULL when keylen is 0).
 * CRYPTO_OK or CRYPTO_ERR_BAD_ARG. */
int  blake2b_init(blake2b_ctx *c, size_t outlen, const void *key,
                  size_t keylen);
void blake2b_update(blake2b_ctx *c, const void *data, size_t len);
/* Writes the outlen bytes init was given and leaves c wiped. */
void blake2b_final(blake2b_ctx *c, uint8_t *out);

/* One call: init, update, final. */
int  blake2b(uint8_t *out, size_t outlen, const void *key, size_t keylen,
             const void *data, size_t len);

#endif /* FALCON_CRYPTO_BLAKE2B_H */
