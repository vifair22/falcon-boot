/* falcon-boot: SHA-512 (FIPS 180-4 Section 6.4).
 *
 * Purpose. Header-hash generality for the LUKS2 unlock, not the primary
 * path. Evidence from this machine (cryptsetup 2.8.7): every default header
 * and every header created the way `mkstick` will create p3 selects sha256
 * for the keyslot KDF, the AF splitter and the digest. But `cryptsetup
 * luksFormat --hash sha512` is a single flag away and it switches all three
 * at once, and a header created that way is completely ordinary LUKS2.
 *
 * Reasoning. DESIGN Section 9 scopes EFI crypto to "deliberately the
 * minimum", and SHA-512 is a genuine addition to that list. It earns its
 * place on failure cost: the loader that cannot unlock p3 cannot boot the
 * personal OS at all, and the difference between "we support the hash the
 * header names" and "we support the hash we expected the header to name" is
 * about 150 lines and ~1 KB of image. mkstick still pins sha256; this is the
 * belt to that braces. If the scope call goes the other way, deleting this
 * file and its one row in hash.c's table is the whole revert.
 *
 * Interactions. Identical shape to sha256.h; registered in hash.c so hmac.c,
 * pbkdf2.c and falcon-core/src/luks/af.c reach it through the descriptor
 * without knowing which hash they hold.
 */

#ifndef FALCON_CRYPTO_SHA512_H
#define FALCON_CRYPTO_SHA512_H

#include <stdint.h>
#include <stddef.h>

#define SHA512_DIGEST_LEN 64u
#define SHA512_BLOCK_LEN  128u

typedef struct {
    uint64_t h[8];
    uint64_t total_lo;            /* message bytes absorbed, 128-bit count */
    uint64_t total_hi;
    uint32_t buflen;              /* bytes held in buf, always < 128 */
    uint8_t  buf[SHA512_BLOCK_LEN];
} sha512_ctx;

void sha512_init(sha512_ctx *c);
void sha512_update(sha512_ctx *c, const void *data, size_t len);
/* Writes SHA512_DIGEST_LEN bytes and leaves c wiped. */
void sha512_final(sha512_ctx *c, uint8_t *out);
void sha512(const void *data, size_t len, uint8_t *out);

#endif /* FALCON_CRYPTO_SHA512_H */
