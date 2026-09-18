/* falcon-boot: HMAC (RFC 2104). See hmac.h. */

#include <crypto/hmac.h>
#include <crypto/crypto.h>
#include <lib/string.h>

int hmac_key_init(hmac_key *k, const hash_desc *d,
                  const void *key, size_t key_len)
{
    uint8_t pad[HASH_MAX_BLOCK];
    uint8_t shortened[HASH_MAX_DIGEST];
    const uint8_t *kp = (const uint8_t *)key;
    size_t klen = key_len;
    size_t i;

    if (k == NULL || d == NULL || (key == NULL && key_len != 0)) {
        return CRYPTO_ERR_BAD_ARG;
    }
    if (d->block_len > HASH_MAX_BLOCK || d->digest_len > HASH_MAX_DIGEST) {
        return CRYPTO_ERR_UNSUPPORTED;
    }

    /* RFC 2104: a key longer than the block is replaced by its own hash, a
     * shorter one is zero-extended. */
    if (klen > d->block_len) {
        hash_ctx h;

        hash_init(&h, d);
        hash_update(&h, kp, klen);
        hash_final(&h, shortened);
        kp = shortened;
        klen = d->digest_len;
    }

    k->d = d;

    memset(pad, 0x36, d->block_len);
    for (i = 0; i < klen; i++) {
        pad[i] = (uint8_t)(pad[i] ^ kp[i]);
    }
    hash_init(&k->istate, d);
    hash_update(&k->istate, pad, d->block_len);

    memset(pad, 0x5c, d->block_len);
    for (i = 0; i < klen; i++) {
        pad[i] = (uint8_t)(pad[i] ^ kp[i]);
    }
    hash_init(&k->ostate, d);
    hash_update(&k->ostate, pad, d->block_len);

    crypto_wipe(pad, sizeof pad);
    crypto_wipe(shortened, sizeof shortened);
    return CRYPTO_OK;
}

void hmac_begin(hmac_ctx *c, const hmac_key *k)
{
    c->d = k->d;
    c->inner = k->istate;
    c->outer = k->ostate;
}

void hmac_update(hmac_ctx *c, const void *data, size_t len)
{
    hash_update(&c->inner, data, len);
}

void hmac_final(hmac_ctx *c, uint8_t *out)
{
    uint8_t idig[HASH_MAX_DIGEST];

    hash_final(&c->inner, idig);
    hash_update(&c->outer, idig, c->d->digest_len);
    hash_final(&c->outer, out);
    crypto_wipe(idig, sizeof idig);
    c->d = NULL;
}

int hmac(const hash_desc *d, const void *key, size_t key_len,
         const void *data, size_t len, uint8_t *out)
{
    hmac_key k;
    hmac_ctx c;
    int rc;

    rc = hmac_key_init(&k, d, key, key_len);
    if (rc != CRYPTO_OK) {
        return rc;
    }
    hmac_begin(&c, &k);
    hmac_update(&c, data, len);
    hmac_final(&c, out);
    crypto_wipe(&k, sizeof k);
    return CRYPTO_OK;
}
