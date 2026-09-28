/* falcon-boot: fixed-width modular arithmetic. See bn.h. */

#include <crypto/bn.h>
#include <crypto/crypto.h>
#include <lib/string.h>

int bn_from_be(bn_limb *r, size_t n, const uint8_t *in, size_t len)
{
    size_t i;

    if (r == NULL || (in == NULL && len != 0) || n == 0) {
        return CRYPTO_ERR_BAD_ARG;
    }
    /* Leading zeros past the width are allowed; a nonzero byte there is a
     * value too wide for the caller's array. */
    while (len > n * 4u) {
        if (in[0] != 0u) {
            return CRYPTO_ERR_BAD_ARG;
        }
        in++;
        len--;
    }
    bn_zero(r, n);
    for (i = 0; i < len; i++) {
        size_t bit = (len - 1u - i) * 8u;

        r[bit / 32u] |= (bn_limb)in[i] << (bit % 32u);
    }
    return CRYPTO_OK;
}

void bn_to_be(uint8_t *out, size_t len, const bn_limb *a, size_t n)
{
    size_t i;

    for (i = 0; i < len; i++) {
        size_t bit = (len - 1u - i) * 8u;

        out[i] = (bit / 32u < n) ? (uint8_t)(a[bit / 32u] >> (bit % 32u)) : 0u;
    }
}

void bn_zero(bn_limb *r, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        r[i] = 0;
    }
}

int bn_is_zero(const bn_limb *a, size_t n)
{
    bn_limb acc = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        acc |= a[i];
    }
    return acc == 0u;
}

int bn_cmp(const bn_limb *a, const bn_limb *b, size_t n)
{
    size_t i = n;

    while (i-- > 0u) {
        if (a[i] != b[i]) {
            return a[i] > b[i] ? 1 : -1;
        }
    }
    return 0;
}

bn_limb bn_add(bn_limb *r, const bn_limb *a, const bn_limb *b, size_t n)
{
    uint64_t c = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        c += (uint64_t)a[i] + b[i];
        r[i] = (bn_limb)c;
        c >>= 32;
    }
    return (bn_limb)c;
}

bn_limb bn_sub(bn_limb *r, const bn_limb *a, const bn_limb *b, size_t n)
{
    uint64_t borrow = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;

        r[i] = (bn_limb)d;
        borrow = (d >> 63) & 1u;
    }
    return (bn_limb)borrow;
}

void bn_select(bn_limb *r, const bn_limb *a, size_t n, bn_limb cond)
{
    bn_limb mask = (bn_limb)0u - (bn_limb)(cond != 0u);
    size_t i;

    for (i = 0; i < n; i++) {
        r[i] = (r[i] & ~mask) | (a[i] & mask);
    }
}

void bn_cswap(bn_limb *a, bn_limb *b, size_t n, bn_limb cond)
{
    bn_limb mask = (bn_limb)0u - (bn_limb)(cond != 0u);
    size_t i;

    for (i = 0; i < n; i++) {
        bn_limb t = (a[i] ^ b[i]) & mask;

        a[i] ^= t;
        b[i] ^= t;
    }
}

void bn_mod_add(bn_limb *r, const bn_limb *a, const bn_limb *b,
                const bn_mont *c)
{
    bn_limb s[BN_MAX_LIMBS];
    bn_limb carry = bn_add(r, a, b, c->n);
    bn_limb borrow = bn_sub(s, r, c->m, c->n);

    /* The sum is reduced when it overflowed the width or is at least m;
     * m < 2^(32n), so an overflowed sum always is. */
    bn_select(r, s, c->n, carry | (borrow ^ 1u));
}

void bn_mod_sub(bn_limb *r, const bn_limb *a, const bn_limb *b,
                const bn_mont *c)
{
    bn_limb s[BN_MAX_LIMBS];
    bn_limb borrow = bn_sub(r, a, b, c->n);

    (void)bn_add(s, r, c->m, c->n);
    bn_select(r, s, c->n, borrow);
}

int bn_mont_init(bn_mont *c, const bn_limb *m, size_t n)
{
    bn_limb x;
    size_t i;
    int k;

    if (c == NULL || m == NULL || n == 0 || n > BN_MAX_LIMBS ||
        (m[0] & 1u) == 0u || m[n - 1u] == 0u) {
        return CRYPTO_ERR_BAD_ARG;
    }
    c->m = m;
    c->n = n;

    /* Newton's iteration for m0^-1 mod 2^32: an odd m0 is its own inverse
     * mod 8, and each step doubles the correct low bits (3, 6, 12, 24, 48). */
    x = m[0];
    for (k = 0; k < 4; k++) {
        x *= 2u - m[0] * x;
    }
    c->m0inv = (bn_limb)0u - x;

    /* R^2 mod m by doubling 1 2*32*n times. A few thousand limb passes, once
     * per context, against a division routine that would exist only here. */
    bn_zero(c->rr, n);
    c->rr[0] = 1u;
    for (i = 0; i < 64u * n; i++) {
        bn_mod_add(c->rr, c->rr, c->rr, c);
    }
    return CRYPTO_OK;
}

void bn_mont_mul(bn_limb *r, const bn_limb *a, const bn_limb *b,
                 const bn_mont *c)
{
    bn_limb t[BN_MAX_LIMBS + 2u];
    bn_limb s[BN_MAX_LIMBS];
    size_t n = c->n;
    size_t i, j;

    memset(t, 0, sizeof t);
    /* CIOS (Koc, Acar, Kaliski 1996): interleave one row of the product with
     * one reduction step, so t never exceeds n + 2 limbs. */
    for (i = 0; i < n; i++) {
        uint64_t cy = 0;
        bn_limb q;

        for (j = 0; j < n; j++) {
            cy += (uint64_t)t[j] + (uint64_t)a[j] * b[i];
            t[j] = (bn_limb)cy;
            cy >>= 32;
        }
        cy += t[n];
        t[n] = (bn_limb)cy;
        t[n + 1u] = (bn_limb)(cy >> 32);

        q = t[0] * c->m0inv;
        cy = (uint64_t)t[0] + (uint64_t)q * c->m[0];
        cy >>= 32;
        for (j = 1; j < n; j++) {
            cy += (uint64_t)t[j] + (uint64_t)q * c->m[j];
            t[j - 1u] = (bn_limb)cy;
            cy >>= 32;
        }
        cy += t[n];
        t[n - 1u] = (bn_limb)cy;
        t[n] = t[n + 1u] + (bn_limb)(cy >> 32);
    }
    /* t < 2m: one masked subtraction. */
    {
        bn_limb borrow = bn_sub(s, t, c->m, n);

        bn_select(t, s, n, t[n] | (borrow ^ 1u));
    }
    for (i = 0; i < n; i++) {
        r[i] = t[i];
    }
}

void bn_to_mont(bn_limb *r, const bn_limb *a, const bn_mont *c)
{
    bn_mont_mul(r, a, c->rr, c);
}

void bn_from_mont(bn_limb *r, const bn_limb *a, const bn_mont *c)
{
    bn_limb one[BN_MAX_LIMBS];

    bn_zero(one, c->n);
    one[0] = 1u;
    bn_mont_mul(r, a, one, c);
}

void bn_modexp(bn_limb *r, const bn_limb *base, const uint8_t *exp,
               size_t exp_len, const bn_mont *c)
{
    bn_limb acc[BN_MAX_LIMBS];
    bn_limb b[BN_MAX_LIMBS];
    bn_limb t[BN_MAX_LIMBS];
    bn_limb one[BN_MAX_LIMBS];
    size_t n = c->n;
    size_t i;

    bn_zero(one, n);
    one[0] = 1u;
    bn_to_mont(acc, one, c);
    bn_to_mont(b, base, c);
    for (i = 0; i < exp_len; i++) {
        int bit;

        for (bit = 7; bit >= 0; bit--) {
            bn_mont_mul(acc, acc, acc, c);
            bn_mont_mul(t, acc, b, c);
            bn_select(acc, t, n, (bn_limb)((exp[i] >> bit) & 1u));
        }
    }
    bn_from_mont(r, acc, c);
    crypto_wipe(acc, sizeof acc);
    crypto_wipe(b, sizeof b);
    crypto_wipe(t, sizeof t);
}
