/* falcon-boot: SHA-1 (FIPS 180-4). See sha1.h. Same shape as sha256.c,
 * including the finalisation that spills into a second block for a message
 * of 56..63 bytes mod 64. */

#include <crypto/sha1.h>
#include <lib/string.h>

static uint32_t rol32(uint32_t x, unsigned int n)
{
    return (uint32_t)((x << n) | (x >> (32u - n)));
}

static uint32_t be32_load(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    uint32_t a, b, cc, d, e;
    unsigned int i;

    for (i = 0; i < 16u; i++)
        w[i] = be32_load(p + (size_t)4u * i);
    for (i = 16u; i < 80u; i++)
        w[i] = rol32(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1);

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (i = 0; i < 80u; i++) {
        uint32_t f, k, t;

        if (i < 20u) {
            f = (b & cc) | ((~b) & d);
            k = 0x5a827999u;
        } else if (i < 40u) {
            f = b ^ cc ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60u) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ cc ^ d;
            k = 0xca62c1d6u;
        }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301u;
    c->h[1] = 0xefcdab89u;
    c->h[2] = 0x98badcfeu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xc3d2e1f0u;
    c->total = 0;
    c->buflen = 0;
    memset(c->buf, 0, sizeof c->buf);
}

void sha1_update(sha1_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (p == (const uint8_t *)0 || len == 0)
        return;
    c->total += (uint64_t)len;
    if (c->buflen != 0) {
        size_t want = SHA1_BLOCK_LEN - c->buflen;
        size_t take = (len < want) ? len : want;

        memcpy(c->buf + c->buflen, p, take);
        c->buflen += (uint32_t)take;
        p += take;
        len -= take;
        if (c->buflen < SHA1_BLOCK_LEN)
            return;
        sha1_block(c, c->buf);
        c->buflen = 0;
    }
    while (len >= SHA1_BLOCK_LEN) {
        sha1_block(c, p);
        p += SHA1_BLOCK_LEN;
        len -= SHA1_BLOCK_LEN;
    }
    if (len != 0) {
        memcpy(c->buf, p, len);
        c->buflen = (uint32_t)len;
    }
}

void sha1_final(sha1_ctx *c, uint8_t *out)
{
    uint64_t bits = c->total << 3;
    unsigned int i;

    c->buf[c->buflen++] = 0x80u;
    if (c->buflen > SHA1_BLOCK_LEN - 8u) {
        memset(c->buf + c->buflen, 0, SHA1_BLOCK_LEN - c->buflen);
        sha1_block(c, c->buf);
        c->buflen = 0;
    }
    memset(c->buf + c->buflen, 0, SHA1_BLOCK_LEN - 8u - c->buflen);
    for (i = 0; i < 8u; i++)
        c->buf[SHA1_BLOCK_LEN - 1u - i] = (uint8_t)(bits >> (8u * i));
    sha1_block(c, c->buf);
    for (i = 0; i < 5u; i++) {
        out[(size_t)4u * i] = (uint8_t)(c->h[i] >> 24);
        out[4u * i + 1u] = (uint8_t)(c->h[i] >> 16);
        out[4u * i + 2u] = (uint8_t)(c->h[i] >> 8);
        out[4u * i + 3u] = (uint8_t)c->h[i];
    }
    memset(c, 0, sizeof *c);
}

void sha1(const void *data, size_t len, uint8_t *out)
{
    sha1_ctx c;

    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}
