/* falcon-boot: RSAES-OAEP-SHA1 encryption and MGF1. See rsa.h. */

#include <crypto/rsa.h>
#include <crypto/bn.h>
#include <crypto/crypto.h>
#include <crypto/sha1.h>
#include <lib/string.h>

void mgf1_sha1_xor(const uint8_t *seed, size_t seed_len,
                   uint8_t *buf, size_t len)
{
    uint8_t block[SHA1_DIGEST_LEN];
    uint8_t ctr[4];
    uint32_t i = 0;
    size_t done = 0;

    while (done < len) {
        sha1_ctx c;
        size_t n = len - done;
        size_t k;

        ctr[0] = (uint8_t)(i >> 24);
        ctr[1] = (uint8_t)(i >> 16);
        ctr[2] = (uint8_t)(i >> 8);
        ctr[3] = (uint8_t)i;
        i++;
        sha1_init(&c);
        sha1_update(&c, seed, seed_len);
        sha1_update(&c, ctr, 4);
        sha1_final(&c, block);
        crypto_wipe(&c, sizeof c);
        if (n > sizeof block) {
            n = sizeof block;
        }
        for (k = 0; k < n; k++) {
            buf[done + k] = (uint8_t)(buf[done + k] ^ block[k]);
        }
        done += n;
    }
    crypto_wipe(block, sizeof block);
}

int rsa_oaep_sha1_encrypt(const uint8_t *mod, size_t mod_len,
                          const uint8_t *exp, size_t exp_len,
                          const uint8_t *label, size_t label_len,
                          const uint8_t *msg, size_t msg_len,
                          const uint8_t seed[20], uint8_t *out)
{
    uint8_t em[RSA_MAX_BYTES];
    bn_limb m[BN_MAX_LIMBS], x[BN_MAX_LIMBS], y[BN_MAX_LIMBS];
    bn_mont mc;
    size_t n, db_len;
    const size_t hl = SHA1_DIGEST_LEN;

    if (mod == NULL || exp == NULL || exp_len == 0 || seed == NULL ||
        out == NULL || (label == NULL && label_len != 0) ||
        (msg == NULL && msg_len != 0)) {
        return CRYPTO_ERR_BAD_ARG;
    }
    /* A modulus given with leading zero bytes is the same modulus. */
    while (mod_len > 0 && mod[0] == 0u) {
        mod++;
        mod_len--;
    }
    if (mod_len > RSA_MAX_BYTES || mod_len < 2u * hl + 2u ||
        msg_len > mod_len - 2u * hl - 2u) {
        return CRYPTO_ERR_BAD_ARG;
    }
    n = (mod_len + 3u) / 4u;
    if (bn_from_be(m, n, mod, mod_len) != CRYPTO_OK ||
        bn_mont_init(&mc, m, n) != CRYPTO_OK) {
        return CRYPTO_ERR_BAD_ARG;
    }

    /* EM = 0x00 || maskedSeed || maskedDB,
     * DB = lHash || PS (zeros) || 0x01 || M. */
    db_len = mod_len - hl - 1u;
    memset(em, 0, mod_len);
    sha1(label, label_len, em + 1u + hl);
    em[1u + hl + db_len - msg_len - 1u] = 0x01u;
    if (msg_len != 0) {
        memcpy(em + 1u + hl + db_len - msg_len, msg, msg_len);
    }
    memcpy(em + 1u, seed, hl);
    mgf1_sha1_xor(em + 1u, hl, em + 1u + hl, db_len);
    mgf1_sha1_xor(em + 1u + hl, db_len, em + 1u, hl);

    (void)bn_from_be(x, n, em, mod_len);
    bn_modexp(y, x, exp, exp_len, &mc);
    bn_to_be(out, mod_len, y, n);

    crypto_wipe(em, sizeof em);
    crypto_wipe(x, sizeof x);
    return CRYPTO_OK;
}
