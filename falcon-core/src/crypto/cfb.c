/* falcon-boot: AES-CFB128. See cfb.h. */

#include <crypto/cfb.h>
#include <crypto/crypto.h>
#include <lib/string.h>

/* One routine for both directions: `decrypt` says whether the feedback
 * register takes the input or the output, and in both directions that is the
 * ciphertext. The input byte is read before the output byte is written, so
 * in == out works. */
static void cfb_run(const aes_key *k, const uint8_t iv[AES_BLOCK],
                    const uint8_t *in, uint8_t *out, size_t len, int decrypt)
{
    uint8_t reg[AES_BLOCK];
    uint8_t ks[AES_BLOCK];
    size_t off = 0;

    memcpy(reg, iv, AES_BLOCK);
    while (off < len) {
        size_t n = len - off;
        size_t i;

        if (n > AES_BLOCK) {
            n = AES_BLOCK;
        }
        aes_encrypt_block(k, reg, ks);
        for (i = 0; i < n; i++) {
            uint8_t c_in = in[off + i];
            uint8_t c_out = (uint8_t)(c_in ^ ks[i]);

            out[off + i] = c_out;
            reg[i] = decrypt ? c_in : c_out;
        }
        off += n;
    }
    crypto_wipe(ks, sizeof ks);
    crypto_wipe(reg, sizeof reg);
}

void aes_cfb_encrypt(const aes_key *k, const uint8_t iv[AES_BLOCK],
                     const uint8_t *in, uint8_t *out, size_t len)
{
    cfb_run(k, iv, in, out, len, 0);
}

void aes_cfb_decrypt(const aes_key *k, const uint8_t iv[AES_BLOCK],
                     const uint8_t *in, uint8_t *out, size_t len)
{
    cfb_run(k, iv, in, out, len, 1);
}
