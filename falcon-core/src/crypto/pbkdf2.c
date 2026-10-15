/* falcon-boot: PBKDF2 (RFC 8018 Section 5.2). See pbkdf2.h.
 *
 * DK = T_1 || T_2 || ... , T_i = U_1 ^ U_2 ^ ... ^ U_c, where
 * U_1 = PRF(P, S || INT_BE32(i)) and U_j = PRF(P, U_{j-1}).
 *
 * The block counter i is one-based and encoded big-endian; getting that
 * wrong yields a derivation that is self-consistent and incompatible with
 * every other implementation, which is only caught by a cross-check against
 * real vectors. The harness runs the RFC 6070 SHA-1-shaped cases restated
 * for SHA-256 plus a cryptsetup-produced header.
 */

#include <crypto/pbkdf2.h>
#include <crypto/hmac.h>
#include <crypto/crypto.h>
#include <lib/string.h>

int pbkdf2(const hash_desc *d, const void *pass, size_t pass_len,
           const void *salt, size_t salt_len, uint32_t iterations,
           uint8_t *out, size_t out_len)
{
    return pbkdf2_tick(d, pass, pass_len, salt, salt_len, iterations, out,
                       out_len, NULL);
}

int pbkdf2_tick(const hash_desc *d, const void *pass, size_t pass_len,
                const void *salt, size_t salt_len, uint32_t iterations,
                uint8_t *out, size_t out_len, void (*tick)(void))
{
    hmac_key key;
    uint8_t u[HASH_MAX_DIGEST];
    uint8_t t[HASH_MAX_DIGEST];
    uint8_t ctr[4];
    uint32_t block = 1;
    size_t done = 0;
    int rc;

    if (d == NULL || out == NULL || out_len == 0 || iterations == 0) {
        return CRYPTO_ERR_BAD_ARG;
    }
    if ((pass == NULL && pass_len != 0) || (salt == NULL && salt_len != 0)) {
        return CRYPTO_ERR_BAD_ARG;
    }

    rc = hmac_key_init(&key, d, pass, pass_len);
    if (rc != CRYPTO_OK) {
        return rc;
    }

    while (done < out_len) {
        size_t take = out_len - done;
        uint32_t j;
        size_t b;
        hmac_ctx c;

        if (take > d->digest_len) {
            take = d->digest_len;
        }

        ctr[0] = (uint8_t)(block >> 24);
        ctr[1] = (uint8_t)(block >> 16);
        ctr[2] = (uint8_t)(block >> 8);
        ctr[3] = (uint8_t)block;

        hmac_begin(&c, &key);
        hmac_update(&c, salt, salt_len);
        hmac_update(&c, ctr, sizeof ctr);
        hmac_final(&c, u);
        memcpy(t, u, d->digest_len);

        for (j = 1; j < iterations; j++) {
            hmac_begin(&c, &key);
            hmac_update(&c, u, d->digest_len);
            hmac_final(&c, u);
            for (b = 0; b < d->digest_len; b++) {
                t[b] = (uint8_t)(t[b] ^ u[b]);
            }
            if (tick != NULL && j % PBKDF2_TICK == 0u) {
                tick();
            }
        }

        memcpy(out + done, t, take);
        done += take;
        block++;
    }

    crypto_wipe(&key, sizeof key);
    crypto_wipe(u, sizeof u);
    crypto_wipe(t, sizeof t);
    return CRYPTO_OK;
}
