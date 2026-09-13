/* airies-boot: HMAC (RFC 2104) over the hash descriptors.
 *
 * Purpose. The PRF underneath PBKDF2, which is the whole of the p3 keyslot
 * derivation and the LUKS2 digest verification.
 *
 * Reasoning. The API is split into a *key* and a *context* on purpose.
 * HMAC's two block-sized pads depend only on the key, and PBKDF2 evaluates
 * the PRF with the same key between one hundred thousand and two million
 * times. Recomputing the ipad/opad compressions per evaluation doubles the
 * work; hmac_key holds the two hash states after absorbing the pads once,
 * and hmac_begin restores one of them with a struct copy. That single
 * decision is the difference between a p3 unlock that is tolerable in
 * single-threaded freestanding C and one that is not, and it is why this
 * module exists at all rather than being folded into pbkdf2.c.
 *
 * Interactions. Built on hash.h; the only consumer in the loader is
 * pbkdf2.c, but the one-shot hmac() is kept for the vector suite (RFC 4231),
 * which is how the pad handling gets checked directly rather than only
 * through PBKDF2.
 */

#ifndef AIRIES_CRYPTO_HMAC_H
#define AIRIES_CRYPTO_HMAC_H

#include <stdint.h>
#include <stddef.h>

#include <crypto/hash.h>

/* Key schedule: the inner and outer hash states with the pads absorbed.
 * Contains key-derived material; wipe with crypto_wipe when done. */
typedef struct {
    const hash_desc *d;
    hash_ctx istate;
    hash_ctx ostate;
} hmac_key;

typedef struct {
    const hash_desc *d;
    hash_ctx inner;
    hash_ctx outer;
} hmac_ctx;

int  hmac_key_init(hmac_key *k, const hash_desc *d,
                   const void *key, size_t key_len);

/* Starts one MAC under k. Cheap: two struct copies, no compression. */
void hmac_begin(hmac_ctx *c, const hmac_key *k);
void hmac_update(hmac_ctx *c, const void *data, size_t len);
/* Writes d->digest_len bytes and leaves c wiped. */
void hmac_final(hmac_ctx *c, uint8_t *out);

/* One-shot; used by the vector suite. */
int  hmac(const hash_desc *d, const void *key, size_t key_len,
          const void *data, size_t len, uint8_t *out);

#endif /* AIRIES_CRYPTO_HMAC_H */
