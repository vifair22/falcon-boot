/* airies-boot: SHA-512 (FIPS 180-4). See sha512.h.
 *
 * Structurally the same transcription as sha256.c with 64-bit words, 80
 * rounds, different rotation amounts, and a 128-bit length field. The
 * constants are the first 64 bits of the fractional parts of the cube roots
 * of the first 80 primes (FIPS 180-4 Section 4.2.3) and the initial state is
 * the same construction over the square roots (Section 5.3.5).
 */

#include <crypto/sha512.h>
#include <crypto/crypto.h>
#include <lib/string.h>

static const uint64_t K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull,
    0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull,
    0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull,
    0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull,
    0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull,
    0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull,
    0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull,
    0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull,
    0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
    0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull,
    0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull,
    0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
    0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull,
    0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull
};

static uint64_t ror64(uint64_t x, unsigned int n)
{
    return (x >> n) | (x << (64u - n));
}

static uint64_t be64_load(const uint8_t *p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8)  | (uint64_t)p[7];
}

static void be64_store(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

static void sha512_block(sha512_ctx *c, const uint8_t *p)
{
    uint64_t w[80];
    uint64_t a, b, cc, d, e, f, g, h;
    unsigned int i;

    for (i = 0; i < 16u; i++) {
        w[i] = be64_load(p + 8u * i);
    }
    for (i = 16u; i < 80u; i++) {
        uint64_t s0 = ror64(w[i - 15u], 1) ^ ror64(w[i - 15u], 8) ^
                      (w[i - 15u] >> 7);
        uint64_t s1 = ror64(w[i - 2u], 19) ^ ror64(w[i - 2u], 61) ^
                      (w[i - 2u] >> 6);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6];  h = c->h[7];

    for (i = 0; i < 80u; i++) {
        uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t t1 = h + S1 + ch + K[i] + w[i];
        uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
        uint64_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint64_t t2 = S0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g;  c->h[7] += h;

    crypto_wipe(w, sizeof w);
}

void sha512_init(sha512_ctx *c)
{
    c->h[0] = 0x6a09e667f3bcc908ull; c->h[1] = 0xbb67ae8584caa73bull;
    c->h[2] = 0x3c6ef372fe94f82bull; c->h[3] = 0xa54ff53a5f1d36f1ull;
    c->h[4] = 0x510e527fade682d1ull; c->h[5] = 0x9b05688c2b3e6c1full;
    c->h[6] = 0x1f83d9abfb41bd6bull; c->h[7] = 0x5be0cd19137e2179ull;
    c->total_lo = 0;
    c->total_hi = 0;
    c->buflen = 0;
    memset(c->buf, 0, sizeof c->buf);
}

void sha512_update(sha512_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (p == NULL || len == 0) {
        return;
    }
    /* 128-bit byte count: carry into the high word on wrap. Nothing this
     * loader hashes comes close, but the field is specified and costs a
     * compare. */
    if (c->total_lo + (uint64_t)len < c->total_lo) {
        c->total_hi++;
    }
    c->total_lo += (uint64_t)len;

    if (c->buflen != 0) {
        size_t want = SHA512_BLOCK_LEN - c->buflen;
        size_t take = (len < want) ? len : want;

        memcpy(c->buf + c->buflen, p, take);
        c->buflen += (uint32_t)take;
        p += take;
        len -= take;
        if (c->buflen < SHA512_BLOCK_LEN) {
            return;
        }
        sha512_block(c, c->buf);
        c->buflen = 0;
    }

    while (len >= SHA512_BLOCK_LEN) {
        sha512_block(c, p);
        p += SHA512_BLOCK_LEN;
        len -= SHA512_BLOCK_LEN;
    }
    if (len != 0) {
        memcpy(c->buf, p, len);
        c->buflen = (uint32_t)len;
    }
}

void sha512_final(sha512_ctx *c, uint8_t *out)
{
    uint64_t bits_lo = c->total_lo << 3;
    uint64_t bits_hi = (c->total_hi << 3) | (c->total_lo >> 61);
    unsigned int i;

    c->buf[c->buflen] = 0x80u;
    c->buflen++;
    if (c->buflen > SHA512_BLOCK_LEN - 16u) {
        memset(c->buf + c->buflen, 0, SHA512_BLOCK_LEN - c->buflen);
        sha512_block(c, c->buf);
        c->buflen = 0;
    }
    memset(c->buf + c->buflen, 0, SHA512_BLOCK_LEN - 16u - c->buflen);
    be64_store(c->buf + SHA512_BLOCK_LEN - 16u, bits_hi);
    be64_store(c->buf + SHA512_BLOCK_LEN - 8u, bits_lo);
    sha512_block(c, c->buf);

    for (i = 0; i < 8u; i++) {
        be64_store(out + 8u * i, c->h[i]);
    }
    crypto_wipe(c, sizeof *c);
}

void sha512(const void *data, size_t len, uint8_t *out)
{
    sha512_ctx c;

    sha512_init(&c);
    sha512_update(&c, data, len);
    sha512_final(&c, out);
}
