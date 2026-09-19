/* falcon-boot: SHA-1 (FIPS 180-4 Section 6.1).
 *
 * Not for anything security-bearing. A WIM names every stream by its SHA-1
 * (falcon-core/src/wim/), so patching one means computing the name of the
 * stream it adds; that is the only use. */

#ifndef FALCON_CRYPTO_SHA1_H
#define FALCON_CRYPTO_SHA1_H

#include <stdint.h>
#include <stddef.h>

#define SHA1_DIGEST_LEN 20u
#define SHA1_BLOCK_LEN  64u

typedef struct {
    uint32_t h[5];
    uint64_t total;
    uint32_t buflen;
    uint8_t  buf[SHA1_BLOCK_LEN];
} sha1_ctx;

void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const void *data, size_t len);
void sha1_final(sha1_ctx *c, uint8_t *out);
void sha1(const void *data, size_t len, uint8_t *out);

#endif /* FALCON_CRYPTO_SHA1_H */
