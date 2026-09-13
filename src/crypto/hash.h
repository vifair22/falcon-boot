/* falcon-boot: hash-by-name dispatch for LUKS2 metadata.
 *
 * Purpose. A LUKS2 header names its hashes as strings ("sha256"), in four
 * independent places: the binary header's checksum algorithm, the keyslot's
 * kdf.hash, the keyslot's af.hash, and the digest object's hash. Nothing in
 * the format says those four agree, so the unlock path has to carry a hash
 * as a value rather than compile one in.
 *
 * Reasoning. A three-function descriptor plus a tagged-union context keeps
 * that indirection free of any allocator: hmac_ctx embeds a hash_ctx by
 * value, pbkdf2 embeds an hmac_ctx by value, and the whole PBKDF2 working
 * set is one stack frame of a few hundred bytes. The alternative shapes
 * (void* + size, or a separate context allocation per hash) both need a
 * heap that EFI-land's crypto deliberately does not use.
 *
 * The table is closed on purpose. An unknown hash name returns NULL and the
 * LUKS layer turns that into LUKS_ERR_UNSUPPORTED with the name in the
 * message, which is the honest failure: a loader that quietly substituted a
 * different hash would derive the wrong key and report a bad passphrase.
 *
 * Interactions. Implemented over sha256.h and sha512.h; consumed by hmac.c,
 * pbkdf2.c, src/luks/luks.c and src/luks/af.c.
 */

#ifndef FALCON_CRYPTO_HASH_H
#define FALCON_CRYPTO_HASH_H

#include <stdint.h>
#include <stddef.h>

#include <crypto/sha256.h>
#include <crypto/sha512.h>

#define HASH_MAX_DIGEST 64u   /* SHA-512 */
#define HASH_MAX_BLOCK  128u  /* SHA-512 */

typedef struct hash_desc hash_desc;

typedef struct {
    const hash_desc *d;
    union {
        sha256_ctx s256;
        sha512_ctx s512;
    } u;
} hash_ctx;

struct hash_desc {
    const char *name;
    uint32_t digest_len;
    uint32_t block_len;
    void (*init)(hash_ctx *c);
    void (*update)(hash_ctx *c, const void *data, size_t len);
    void (*final)(hash_ctx *c, uint8_t *out);
};

/* NULL when the name is not one we implement. name need not be terminated;
 * len is its exact length, which is how a JSON string span is passed in
 * without being copied out first. */
const hash_desc *hash_by_name(const char *name, size_t len);

const hash_desc *hash_sha256(void);
const hash_desc *hash_sha512(void);

/* Thin wrappers so callers that already hold a descriptor never have to
 * switch on which one it is. */
void hash_init(hash_ctx *c, const hash_desc *d);
void hash_update(hash_ctx *c, const void *data, size_t len);
void hash_final(hash_ctx *c, uint8_t *out);

#endif /* FALCON_CRYPTO_HASH_H */
