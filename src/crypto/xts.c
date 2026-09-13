/* falcon-boot: AES-XTS sector decryption, portable path and dispatch.
 * See xts.h.
 */

#include <crypto/xts.h>
#include <crypto/crypto.h>
#include <lib/string.h>

/* t <- t * x in GF(2^128) over the little-endian XTS representation:
 * a one-bit left shift of the 128-bit value, with a carry out of the top
 * reduced by x^128 + x^7 + x^2 + x + 1 (IEEE 1619 Section 5.2). Written
 * byte-wise so it is the same operation the SSE2 version in aesni.c
 * performs, and so the vectors check both against each other. */
static void gf_double(uint8_t *t)
{
    unsigned int i;
    uint8_t carry = 0;

    for (i = 0; i < AES_BLOCK; i++) {
        uint8_t next = (uint8_t)(t[i] >> 7);

        t[i] = (uint8_t)(((t[i] << 1) & 0xffu) | carry);
        carry = next;
    }
    if (carry != 0u) {
        t[0] = (uint8_t)(t[0] ^ 0x87u);
    }
}

static void tweak0(const aes_key *tweak_key, uint64_t sector, uint8_t *t)
{
    uint8_t iv[AES_BLOCK];
    unsigned int i;

    /* plain64: sector number little-endian in the low 8 bytes, zeroes above. */
    for (i = 0; i < 8u; i++) {
        iv[i] = (uint8_t)(sector >> (8u * i));
    }
    memset(iv + 8, 0, 8);
    aes_encrypt_block(tweak_key, iv, t);
    crypto_wipe(iv, sizeof iv);
}

static int setkey(xts_ctx *c, const uint8_t *key, size_t key_len, int use_ni)
{
    size_t half;
    int rc;

    if (c == NULL || key == NULL) {
        return CRYPTO_ERR_BAD_ARG;
    }
    if (key_len != 32u && key_len != 64u) {
        return CRYPTO_ERR_UNSUPPORTED;
    }
    half = key_len / 2u;

    memset(c, 0, sizeof *c);
    if (use_ni) {
        c->impl = XTS_IMPL_AESNI;
        rc = aesni_setkey_dec(&c->k.n.data_dec, key, half);
        if (rc == CRYPTO_OK) {
            rc = aesni_setkey_enc(&c->k.n.data_enc, key, half);
        }
        if (rc == CRYPTO_OK) {
            rc = aesni_setkey_enc(&c->k.n.tweak, key + half, half);
        }
    } else {
        c->impl = XTS_IMPL_PORTABLE;
        rc = aes_setkey(&c->k.p.data, key, half);
        if (rc == CRYPTO_OK) {
            rc = aes_setkey(&c->k.p.tweak, key + half, half);
        }
    }
    if (rc != CRYPTO_OK) {
        crypto_wipe(c, sizeof *c);
    }
    return rc;
}

int xts_setkey(xts_ctx *c, const uint8_t *key, size_t key_len)
{
    return setkey(c, key, key_len, crypto_has_aesni());
}

int xts_setkey_portable(xts_ctx *c, const uint8_t *key, size_t key_len)
{
    return setkey(c, key, key_len, 0);
}

int xts_impl(const xts_ctx *c)
{
    return (int)c->impl;
}

void xts_wipe(xts_ctx *c)
{
    crypto_wipe(c, sizeof *c);
}

static void xts_portable(const aes_key *data_key, const aes_key *tweak_key,
                         uint64_t sector, const uint8_t *in, uint8_t *out,
                         size_t len, int encrypt)
{
    uint8_t t[AES_BLOCK];
    uint8_t blk[AES_BLOCK];
    size_t i;
    unsigned int j;

    tweak0(tweak_key, sector, t);
    for (i = 0; i + AES_BLOCK <= len; i += AES_BLOCK) {
        for (j = 0; j < AES_BLOCK; j++) {
            blk[j] = (uint8_t)(in[i + j] ^ t[j]);
        }
        if (encrypt) {
            aes_encrypt_block(data_key, blk, blk);
        } else {
            aes_decrypt_block(data_key, blk, blk);
        }
        for (j = 0; j < AES_BLOCK; j++) {
            out[i + j] = (uint8_t)(blk[j] ^ t[j]);
        }
        gf_double(t);
    }
    crypto_wipe(t, sizeof t);
    crypto_wipe(blk, sizeof blk);
}

void xts_decrypt_sector(const xts_ctx *c, uint64_t sector,
                        const uint8_t *in, uint8_t *out, size_t len)
{
    if (c->impl == XTS_IMPL_AESNI) {
        aesni_xts_decrypt(&c->k.n.data_dec, &c->k.n.tweak, sector,
                          in, out, len);
    } else {
        xts_portable(&c->k.p.data, &c->k.p.tweak, sector, in, out, len, 0);
    }
}

void xts_encrypt_sector(const xts_ctx *c, uint64_t sector,
                        const uint8_t *in, uint8_t *out, size_t len)
{
    if (c->impl == XTS_IMPL_AESNI) {
        aesni_xts_encrypt(&c->k.n.data_enc, &c->k.n.tweak, sector,
                          in, out, len);
    } else {
        xts_portable(&c->k.p.data, &c->k.p.tweak, sector, in, out, len, 1);
    }
}
