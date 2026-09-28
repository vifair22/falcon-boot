/* falcon-boot: TPM 2.0 KDFa and KDFe. See kdf.h. */

#include <crypto/kdf.h>
#include <crypto/crypto.h>
#include <crypto/hmac.h>
#include <crypto/sha256.h>
#include <lib/string.h>

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static size_t cstrlen(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int kdf_args_ok(const void *secret, size_t secret_len, const char *label,
                       const uint8_t *u, size_t u_len,
                       const uint8_t *v, size_t v_len,
                       const uint8_t *out, size_t out_bits)
{
    return (secret != NULL || secret_len == 0) && label != NULL &&
           (u != NULL || u_len == 0) && (v != NULL || v_len == 0) &&
           out != NULL && out_bits != 0 && (out_bits % 8u) == 0 &&
           out_bits / 8u <= 0x0fffffffu;
}

int kdfa_sha256(const uint8_t *key, size_t key_len, const char *label,
                const uint8_t *ctx_u, size_t u_len,
                const uint8_t *ctx_v, size_t v_len,
                uint8_t *out, size_t out_bits)
{
    hmac_key hk;
    uint8_t block[SHA256_DIGEST_LEN];
    uint8_t ctr[4], bits[4];
    size_t out_len = out_bits / 8u;
    size_t done = 0;
    uint32_t i = 1;
    int rc;

    if (!kdf_args_ok(key, key_len, label, ctx_u, u_len, ctx_v, v_len,
                     out, out_bits)) {
        return CRYPTO_ERR_BAD_ARG;
    }
    rc = hmac_key_init(&hk, hash_sha256(), key, key_len);
    if (rc != CRYPTO_OK) {
        return rc;
    }
    be32(bits, (uint32_t)out_bits);
    while (done < out_len) {
        hmac_ctx c;
        size_t n = out_len - done;

        be32(ctr, i++);
        hmac_begin(&c, &hk);
        hmac_update(&c, ctr, 4);
        /* The label with its terminating zero octet. */
        hmac_update(&c, label, cstrlen(label) + 1u);
        hmac_update(&c, ctx_u, u_len);
        hmac_update(&c, ctx_v, v_len);
        hmac_update(&c, bits, 4);
        hmac_final(&c, block);
        if (n > sizeof block) {
            n = sizeof block;
        }
        memcpy(out + done, block, n);
        done += n;
    }
    crypto_wipe(block, sizeof block);
    crypto_wipe(&hk, sizeof hk);
    return CRYPTO_OK;
}

int kdfe_sha256(const uint8_t *z, size_t z_len, const char *use,
                const uint8_t *party_u, size_t u_len,
                const uint8_t *party_v, size_t v_len,
                uint8_t *out, size_t out_bits)
{
    uint8_t block[SHA256_DIGEST_LEN];
    uint8_t ctr[4];
    size_t out_len = out_bits / 8u;
    size_t done = 0;
    uint32_t i = 1;

    if (!kdf_args_ok(z, z_len, use, party_u, u_len, party_v, v_len,
                     out, out_bits)) {
        return CRYPTO_ERR_BAD_ARG;
    }
    while (done < out_len) {
        sha256_ctx c;
        size_t n = out_len - done;

        be32(ctr, i++);
        sha256_init(&c);
        sha256_update(&c, ctr, 4);
        sha256_update(&c, z, z_len);
        sha256_update(&c, use, cstrlen(use) + 1u);
        sha256_update(&c, party_u, u_len);
        sha256_update(&c, party_v, v_len);
        sha256_final(&c, block);
        if (n > sizeof block) {
            n = sizeof block;
        }
        memcpy(out + done, block, n);
        done += n;
    }
    crypto_wipe(block, sizeof block);
    return CRYPTO_OK;
}
