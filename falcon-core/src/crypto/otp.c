/* falcon-boot: HOTP and TOTP over HMAC-SHA1. See otp.h. */

#include <crypto/otp.h>
#include <crypto/crypto.h>
#include <crypto/hmac.h>
#include <crypto/sha1.h>

int hotp_sha1(const uint8_t *key, size_t key_len, uint64_t counter,
              unsigned int digits, uint32_t *code)
{
    uint8_t msg[8];
    uint8_t mac[SHA1_DIGEST_LEN];
    uint32_t bin, mod = 1;
    unsigned int i, off;
    int rc;

    if (key == NULL || code == NULL || digits < 6u || digits > 9u) {
        return CRYPTO_ERR_BAD_ARG;
    }
    for (i = 0; i < 8u; i++) {
        msg[i] = (uint8_t)(counter >> (56u - 8u * i));
    }
    rc = hmac(hash_sha1(), key, key_len, msg, sizeof msg, mac);
    if (rc != CRYPTO_OK) {
        return rc;
    }
    /* RFC 4226 Section 5.3, dynamic truncation: the low nibble of the last
     * byte picks four bytes, whose top bit is dropped. */
    off = mac[SHA1_DIGEST_LEN - 1u] & 0x0fu;
    bin = ((uint32_t)(mac[off] & 0x7fu) << 24) |
          ((uint32_t)mac[off + 1u] << 16) |
          ((uint32_t)mac[off + 2u] << 8) |
          (uint32_t)mac[off + 3u];
    for (i = 0; i < digits; i++) {
        mod *= 10u;
    }
    *code = bin % mod;
    crypto_wipe(mac, sizeof mac);
    return CRYPTO_OK;
}

int totp_sha1(const uint8_t *key, size_t key_len, uint64_t unix_time,
              unsigned int step, unsigned int digits, uint32_t *code)
{
    if (step == 0) {
        return CRYPTO_ERR_BAD_ARG;
    }
    return hotp_sha1(key, key_len, unix_time / step, digits, code);
}
