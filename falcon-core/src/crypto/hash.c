/* falcon-boot: hash-by-name dispatch. See hash.h. */

#include <crypto/hash.h>
#include <crypto/crypto.h>
#include <lib/string.h>

static void s256_init(hash_ctx *c)   { sha256_init(&c->u.s256); }
static void s256_update(hash_ctx *c, const void *d, size_t n)
{
    sha256_update(&c->u.s256, d, n);
}
static void s256_final(hash_ctx *c, uint8_t *out)
{
    sha256_final(&c->u.s256, out);
}

static void s512_init(hash_ctx *c)   { sha512_init(&c->u.s512); }
static void s512_update(hash_ctx *c, const void *d, size_t n)
{
    sha512_update(&c->u.s512, d, n);
}
static void s512_final(hash_ctx *c, uint8_t *out)
{
    sha512_final(&c->u.s512, out);
}

static const hash_desc sha256_desc = {
    "sha256", SHA256_DIGEST_LEN, SHA256_BLOCK_LEN,
    s256_init, s256_update, s256_final
};

static const hash_desc sha512_desc = {
    "sha512", SHA512_DIGEST_LEN, SHA512_BLOCK_LEN,
    s512_init, s512_update, s512_final
};

/* Closed table. Anything absent is reported as unsupported rather than
 * approximated; see the header for why that matters. */
static const hash_desc *const table[] = {
    &sha256_desc,
    &sha512_desc
};

const hash_desc *hash_sha256(void) { return &sha256_desc; }
const hash_desc *hash_sha512(void) { return &sha512_desc; }

const hash_desc *hash_by_name(const char *name, size_t len)
{
    size_t i;

    if (name == NULL || len == 0) {
        return NULL;
    }
    for (i = 0; i < sizeof table / sizeof table[0]; i++) {
        const char *n = table[i]->name;
        size_t nlen = 0;

        while (n[nlen] != '\0') {
            nlen++;
        }
        if (nlen == len && memcmp(n, name, len) == 0) {
            return table[i];
        }
    }
    return NULL;
}

void hash_init(hash_ctx *c, const hash_desc *d)
{
    c->d = d;
    d->init(c);
}

void hash_update(hash_ctx *c, const void *data, size_t len)
{
    c->d->update(c, data, len);
}

void hash_final(hash_ctx *c, uint8_t *out)
{
    const hash_desc *d = c->d;

    d->final(c, out);
    c->d = NULL;
}
