/* falcon-boot: SHA-256 (FIPS 180-4 Section 6.2).
 *
 * Purpose. The workhorse hash of the LUKS2 unlock: it is the header
 * checksum algorithm, the PBKDF2 PRF for both the p3 keyslot and the digest
 * object, and the AF-splitter diffusion hash. Every real LUKS2 header
 * inspected on this machine (cryptsetup 2.8.7 defaults, and the machine's
 * own root volumes) uses sha256 in all three roles.
 *
 * Reasoning. Incremental init/update/final rather than one-shot only,
 * because HMAC needs to save and restore a mid-message state (see hmac.h)
 * and because the header checksum runs over 16 KB that is read in pieces.
 * The one-shot wrapper exists for the AF diffusion, which hashes a 32-byte
 * block at a time and would otherwise be three calls each.
 *
 * Interactions. Registered in hash.c's descriptor table; reached by hmac.c,
 * pbkdf2.c, falcon-core/src/luks/luks.c (header checksum) and
 * falcon-core/src/luks/af.c.
 */

#ifndef FALCON_CRYPTO_SHA256_H
#define FALCON_CRYPTO_SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_LEN 32u
#define SHA256_BLOCK_LEN  64u

typedef struct {
    uint32_t h[8];
    uint64_t total;               /* message bytes absorbed so far */
    uint32_t buflen;              /* bytes held in buf, always < 64 */
    uint8_t  buf[SHA256_BLOCK_LEN];
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
/* Writes SHA256_DIGEST_LEN bytes and leaves c wiped. */
void sha256_final(sha256_ctx *c, uint8_t *out);
void sha256(const void *data, size_t len, uint8_t *out);

#endif /* FALCON_CRYPTO_SHA256_H */
