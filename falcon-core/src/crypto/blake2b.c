/* falcon-boot: BLAKE2b (RFC 7693). See blake2b.h.
 *
 * A transcription of RFC 7693 Sections 2 and 3 with the RFC's Appendix C
 * structure: the last block is held back in buf until final, because it is
 * compressed with the final flag set and update cannot know which block is
 * last. A message that is an exact multiple of 128 bytes therefore ends with
 * a full block in buf, not an empty one.
 */

#include <crypto/blake2b.h>
#include <crypto/crypto.h>
#include <lib/string.h>

static const uint64_t IV[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull,
    0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
    0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull
};

/* Rounds 10 and 11 reuse rows 0 and 1 (RFC 7693 Section 3.3). */
static const uint8_t SIGMA[12][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 }
};

static uint64_t ror64(uint64_t x, unsigned int n)
{
    return (x >> n) | (x << (64u - n));
}

#define B2B_G(a, b, c, d, x, y)              \
    do {                                     \
        v[a] = v[a] + v[b] + (x);            \
        v[d] = ror64(v[d] ^ v[a], 32u);      \
        v[c] = v[c] + v[d];                  \
        v[b] = ror64(v[b] ^ v[c], 24u);      \
        v[a] = v[a] + v[b] + (y);            \
        v[d] = ror64(v[d] ^ v[a], 16u);      \
        v[c] = v[c] + v[d];                  \
        v[b] = ror64(v[b] ^ v[c], 63u);      \
    } while (0)

static void compress(blake2b_ctx *c, const uint8_t *block, int last)
{
    uint64_t v[16];
    uint64_t m[16];
    unsigned int i;

    for (i = 0; i < 16u; i++) {
        m[i] = get_le64(block + (size_t)8u * i);
    }
    memcpy(v, c->h, sizeof c->h);
    memcpy(v + 8, IV, sizeof IV);
    v[12] ^= c->t[0];
    v[13] ^= c->t[1];
    if (last) {
        v[14] = ~v[14];
    }
    for (i = 0; i < 12u; i++) {
        const uint8_t *s = SIGMA[i];

        B2B_G(0, 4,  8, 12, m[s[ 0]], m[s[ 1]]);
        B2B_G(1, 5,  9, 13, m[s[ 2]], m[s[ 3]]);
        B2B_G(2, 6, 10, 14, m[s[ 4]], m[s[ 5]]);
        B2B_G(3, 7, 11, 15, m[s[ 6]], m[s[ 7]]);
        B2B_G(0, 5, 10, 15, m[s[ 8]], m[s[ 9]]);
        B2B_G(1, 6, 11, 12, m[s[10]], m[s[11]]);
        B2B_G(2, 7,  8, 13, m[s[12]], m[s[13]]);
        B2B_G(3, 4,  9, 14, m[s[14]], m[s[15]]);
    }
    for (i = 0; i < 8u; i++) {
        c->h[i] ^= v[i] ^ v[i + 8u];
    }
    crypto_wipe(v, sizeof v);
    crypto_wipe(m, sizeof m);
}

static void count(blake2b_ctx *c, uint32_t n)
{
    c->t[0] += n;
    if (c->t[0] < n) {
        c->t[1]++;
    }
}

int blake2b_init(blake2b_ctx *c, size_t outlen, const void *key,
                 size_t keylen)
{
    unsigned int i;

    if (c == NULL || outlen == 0u || outlen > BLAKE2B_OUT_MAX ||
        keylen > BLAKE2B_KEY_MAX || (key == NULL && keylen != 0u)) {
        return CRYPTO_ERR_BAD_ARG;
    }
    for (i = 0; i < 8u; i++) {
        c->h[i] = IV[i];
    }
    /* The parameter block's first word: digest length, key length, fanout 1
     * and depth 1 (sequential mode). Every other parameter word is zero. */
    c->h[0] ^= 0x01010000ull ^ ((uint64_t)keylen << 8) ^ (uint64_t)outlen;
    c->t[0] = 0;
    c->t[1] = 0;
    c->outlen = (uint32_t)outlen;
    c->buflen = 0;
    memset(c->buf, 0, sizeof c->buf);
    if (keylen != 0u) {
        /* The key is its own zero-padded first block. */
        memcpy(c->buf, key, keylen);
        c->buflen = BLAKE2B_BLOCK;
    }
    return CRYPTO_OK;
}

void blake2b_update(blake2b_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;

    while (len > 0u) {
        uint32_t take;

        if (c->buflen == BLAKE2B_BLOCK) {
            count(c, BLAKE2B_BLOCK);
            compress(c, c->buf, 0);
            c->buflen = 0;
        }
        take = BLAKE2B_BLOCK - c->buflen;
        if ((size_t)take > len) {
            take = (uint32_t)len;
        }
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        len -= take;
    }
}

void blake2b_final(blake2b_ctx *c, uint8_t *out)
{
    uint32_t i;

    count(c, c->buflen);
    memset(c->buf + c->buflen, 0, BLAKE2B_BLOCK - c->buflen);
    compress(c, c->buf, 1);
    for (i = 0; i < c->outlen; i++) {
        out[i] = (uint8_t)(c->h[i / 8u] >> (8u * (i % 8u)));
    }
    crypto_wipe(c, sizeof *c);
}

int blake2b(uint8_t *out, size_t outlen, const void *key, size_t keylen,
            const void *data, size_t len)
{
    blake2b_ctx c;
    int rc = blake2b_init(&c, outlen, key, keylen);

    if (rc != CRYPTO_OK) {
        return rc;
    }
    blake2b_update(&c, data, len);
    blake2b_final(&c, out);
    return CRYPTO_OK;
}
