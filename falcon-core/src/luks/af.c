/* falcon-boot: LUKS anti-forensic splitter, merge direction. See af.h. */

#include <luks/af.h>
#include <crypto/crypto.h>
#include <lib/string.h>

/* One diffusion block: hash BE32(index) || src, keep the first len bytes.
 * The index is what stops the diffusion from being a plain block-wise hash
 * (identical blocks would otherwise diffuse identically), and its big-endian
 * encoding is part of the on-disk contract, not a free choice. */
static void hash_block(const hash_desc *h, uint32_t index,
                       const uint8_t *src, uint8_t *dst, size_t len)
{
    hash_ctx c;
    uint8_t be[4];
    uint8_t digest[HASH_MAX_DIGEST];

    be[0] = (uint8_t)(index >> 24);
    be[1] = (uint8_t)(index >> 16);
    be[2] = (uint8_t)(index >> 8);
    be[3] = (uint8_t)index;

    hash_init(&c, h);
    hash_update(&c, be, sizeof be);
    hash_update(&c, src, len);
    hash_final(&c, digest);
    memcpy(dst, digest, len);
    crypto_wipe(digest, sizeof digest);
}

/* In place, and safe to be so: hash_block reads its whole input before it
 * writes, and the blocks do not overlap. */
static void diffuse(const hash_desc *h, uint8_t *buf, size_t len)
{
    size_t dsz = h->digest_len;
    size_t blocks = len / dsz;
    size_t pad = len % dsz;
    size_t i;

    for (i = 0; i < blocks; i++) {
        hash_block(h, (uint32_t)i, buf + dsz * i, buf + dsz * i, dsz);
    }
    if (pad != 0) {
        hash_block(h, (uint32_t)blocks, buf + dsz * blocks,
                   buf + dsz * blocks, pad);
    }
}

int af_merge_init(af_merge_ctx *c, const hash_desc *h,
                  uint32_t blocksize, uint32_t stripes)
{
    if (c == NULL || h == NULL) {
        return AF_ERR_BAD_ARG;
    }
    if (blocksize == 0u || blocksize > AF_MAX_BLOCK || stripes == 0u) {
        return AF_ERR_BAD_ARG;
    }
    c->h = h;
    c->blocksize = blocksize;
    c->stripes = stripes;
    c->seen = 0;
    memset(c->acc, 0, sizeof c->acc);
    return AF_OK;
}

int af_merge_stripe(af_merge_ctx *c, const uint8_t *stripe)
{
    uint32_t i;

    if (c == NULL || stripe == NULL) {
        return AF_ERR_BAD_ARG;
    }
    if (c->seen >= c->stripes) {
        return AF_ERR_STATE;
    }
    for (i = 0; i < c->blocksize; i++) {
        c->acc[i] = (uint8_t)(c->acc[i] ^ stripe[i]);
    }
    c->seen++;
    /* Every stripe but the last is followed by a diffusion; the last one is
     * a bare XOR, which is why final() is just a copy. */
    if (c->seen < c->stripes) {
        diffuse(c->h, c->acc, c->blocksize);
    }
    return AF_OK;
}

int af_merge_final(af_merge_ctx *c, uint8_t *out)
{
    if (c == NULL || out == NULL) {
        return AF_ERR_BAD_ARG;
    }
    if (c->seen != c->stripes) {
        return AF_ERR_STATE;
    }
    memcpy(out, c->acc, c->blocksize);
    return AF_OK;
}

void af_merge_wipe(af_merge_ctx *c)
{
    if (c != NULL) {
        crypto_wipe(c->acc, sizeof c->acc);
        c->seen = 0;
    }
}
