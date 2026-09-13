/* airies-boot: SHA-256 (FIPS 180-4). See sha256.h.
 *
 * A direct transcription of the standard, unrolled only far enough to keep
 * the message schedule in registers. No table beyond the 64 round constants
 * K, which are the first 32 bits of the fractional parts of the cube roots
 * of the first 64 primes (FIPS 180-4 Section 4.2.2); the initial state H is
 * the same construction over the square roots (Section 5.3.3).
 *
 * The shape to be careful about is the finalisation padding: the length is
 * appended as a big-endian bit count, and when fewer than 9 bytes are left
 * in the current block the padding spills into a second block. Getting that
 * wrong produces a hash that is right for most inputs and wrong for inputs
 * of length 56..63 mod 64, which is exactly the class of bug that survives
 * a careless test suite. The vectors in the harness include such a length.
 */

#include <crypto/sha256.h>
#include <crypto/crypto.h>
#include <lib/string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t ror32(uint32_t x, unsigned int n)
{
    return (uint32_t)((x >> n) | (x << (32u - n)));
}

static uint32_t be32_load(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void be32_store(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;
    unsigned int i;

    for (i = 0; i < 16u; i++) {
        w[i] = be32_load(p + 4u * i);
    }
    for (i = 16u; i < 64u; i++) {
        uint32_t s0 = ror32(w[i - 15u], 7) ^ ror32(w[i - 15u], 18) ^
                      (w[i - 15u] >> 3);
        uint32_t s1 = ror32(w[i - 2u], 17) ^ ror32(w[i - 2u], 19) ^
                      (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6];  h = c->h[7];

    for (i = 0; i < 64u; i++) {
        uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g;  c->h[7] += h;

    crypto_wipe(w, sizeof w);
}

void sha256_init(sha256_ctx *c)
{
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->total = 0;
    c->buflen = 0;
    memset(c->buf, 0, sizeof c->buf);
}

void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (p == NULL || len == 0) {
        return;
    }
    c->total += (uint64_t)len;

    if (c->buflen != 0) {
        size_t want = SHA256_BLOCK_LEN - c->buflen;
        size_t take = (len < want) ? len : want;

        memcpy(c->buf + c->buflen, p, take);
        c->buflen += (uint32_t)take;
        p += take;
        len -= take;
        if (c->buflen < SHA256_BLOCK_LEN) {
            return;
        }
        sha256_block(c, c->buf);
        c->buflen = 0;
    }

    while (len >= SHA256_BLOCK_LEN) {
        sha256_block(c, p);
        p += SHA256_BLOCK_LEN;
        len -= SHA256_BLOCK_LEN;
    }
    if (len != 0) {
        memcpy(c->buf, p, len);
        c->buflen = (uint32_t)len;
    }
}

void sha256_final(sha256_ctx *c, uint8_t *out)
{
    uint64_t bits = c->total << 3;
    unsigned int i;

    /* 0x80 then zeroes, then the 8-byte big-endian bit length. When the
     * 0x80 lands in the last 8 bytes of the block there is no room for the
     * length, so the block is flushed and the length goes in the next one. */
    c->buf[c->buflen] = 0x80u;
    c->buflen++;
    if (c->buflen > SHA256_BLOCK_LEN - 8u) {
        memset(c->buf + c->buflen, 0, SHA256_BLOCK_LEN - c->buflen);
        sha256_block(c, c->buf);
        c->buflen = 0;
    }
    memset(c->buf + c->buflen, 0, SHA256_BLOCK_LEN - 8u - c->buflen);
    be32_store(c->buf + SHA256_BLOCK_LEN - 8u, (uint32_t)(bits >> 32));
    be32_store(c->buf + SHA256_BLOCK_LEN - 4u, (uint32_t)bits);
    sha256_block(c, c->buf);

    for (i = 0; i < 8u; i++) {
        be32_store(out + 4u * i, c->h[i]);
    }
    crypto_wipe(c, sizeof *c);
}

void sha256(const void *data, size_t len, uint8_t *out)
{
    sha256_ctx c;

    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}
