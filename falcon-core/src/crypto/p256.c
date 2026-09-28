/* falcon-boot: NIST P-256 key agreement. See p256.h.
 *
 * Curve parameters from FIPS 186-4 Appendix D.1.2.3 (SEC 2 secp256r1). The
 * formulas are the Explicit-Formulas Database's dbl-2001-b (a = -3) and
 * add-2007-bl, both in Jacobian coordinates. */

#include <crypto/p256.h>
#include <crypto/bn.h>
#include <crypto/crypto.h>
#include <lib/string.h>

#define NL 8u   /* limbs per coordinate */

static const uint8_t P256_P[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
/* p - 2, the exponent that inverts by Fermat's little theorem. */
static const uint8_t P256_PM2[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfd
};
static const uint8_t P256_N[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84,
    0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51
};
static const uint8_t P256_B[32] = {
    0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7,
    0xb3, 0xeb, 0xbd, 0x55, 0x76, 0x98, 0x86, 0xbc,
    0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53, 0xb0, 0xf6,
    0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b
};
static const uint8_t P256_GX[32] = {
    0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47,
    0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
    0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0,
    0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96
};
static const uint8_t P256_GY[32] = {
    0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b,
    0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
    0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce,
    0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5
};

/* The field, set up per call on the caller's stack: the context points into
 * itself (f.m is p), so it is never copied. */
typedef struct {
    bn_limb p[NL];
    bn_mont f;
    bn_limb b[NL];      /* Montgomery form */
    bn_limb one[NL];    /* Montgomery form, R mod p */
} field;

typedef struct {
    bn_limb x[NL], y[NL], z[NL];   /* Montgomery form; z = 0 is the identity */
} jpoint;

static void field_init(field *F)
{
    bn_limb t[NL];

    (void)bn_from_be(F->p, NL, P256_P, sizeof P256_P);
    (void)bn_mont_init(&F->f, F->p, NL);
    (void)bn_from_be(t, NL, P256_B, sizeof P256_B);
    bn_to_mont(F->b, t, &F->f);
    bn_zero(t, NL);
    t[0] = 1u;
    bn_to_mont(F->one, t, &F->f);
}

static void fmul(bn_limb *r, const bn_limb *a, const bn_limb *b,
                 const field *F)
{
    bn_mont_mul(r, a, b, &F->f);
}

static void fadd(bn_limb *r, const bn_limb *a, const bn_limb *b,
                 const field *F)
{
    bn_mod_add(r, a, b, &F->f);
}

static void fsub(bn_limb *r, const bn_limb *a, const bn_limb *b,
                 const field *F)
{
    bn_mod_sub(r, a, b, &F->f);
}

static void pcopy(jpoint *r, const jpoint *a)
{
    memcpy(r, a, sizeof *r);
}

/* dbl-2001-b. The identity (z = 0) doubles to z3 = 2yz = 0, the identity,
 * with no special case. */
static void pdouble(jpoint *r, const jpoint *a, const field *F)
{
    bn_limb delta[NL], gamma[NL], beta[NL], alpha[NL], t1[NL], t2[NL];

    fmul(delta, a->z, a->z, F);
    fmul(gamma, a->y, a->y, F);
    fmul(beta, a->x, gamma, F);

    fsub(t1, a->x, delta, F);
    fadd(t2, a->x, delta, F);
    fmul(t1, t1, t2, F);
    fadd(alpha, t1, t1, F);
    fadd(alpha, alpha, t1, F);

    /* z3 first: it needs y and z, which r may alias. */
    fadd(t1, a->y, a->z, F);
    fmul(t1, t1, t1, F);
    fsub(t1, t1, gamma, F);
    fsub(r->z, t1, delta, F);

    fmul(t1, alpha, alpha, F);          /* alpha^2 */
    fadd(t2, beta, beta, F);            /* 2 beta */
    fadd(t2, t2, t2, F);                /* 4 beta */
    fadd(beta, t2, t2, F);              /* 8 beta */
    fsub(r->x, t1, beta, F);

    fsub(t2, t2, r->x, F);              /* 4 beta - x3 */
    fmul(t2, alpha, t2, F);
    fmul(gamma, gamma, gamma, F);       /* gamma^2 */
    fadd(gamma, gamma, gamma, F);
    fadd(gamma, gamma, gamma, F);
    fadd(gamma, gamma, gamma, F);       /* 8 gamma^2 */
    fsub(r->y, t2, gamma, F);
}

/* add-2007-bl, with the identity on either side selected in by mask.
 *
 * The one input it gets wrong is a == b (h = 0 and r = 0 give the identity
 * where 2a was meant). The ladder below always adds R0 and R1 = R0 + P, so
 * a == b would need P to be the identity, which validation excludes. The
 * neighbouring case a == -b (h = 0, r != 0) comes out right: z3 carries the
 * factor h, so the sum is the identity, as it should be. */
static void padd(jpoint *r, const jpoint *a, const jpoint *b, const field *F)
{
    bn_limb z1z1[NL], z2z2[NL], u1[NL], u2[NL], s1[NL], s2[NL];
    bn_limb h[NL], i[NL], j[NL], rr[NL], v[NL], t[NL];
    jpoint out;
    bn_limb a_inf = (bn_limb)bn_is_zero(a->z, NL);
    bn_limb b_inf = (bn_limb)bn_is_zero(b->z, NL);

    fmul(z1z1, a->z, a->z, F);
    fmul(z2z2, b->z, b->z, F);
    fmul(u1, a->x, z2z2, F);
    fmul(u2, b->x, z1z1, F);
    fmul(s1, a->y, b->z, F);
    fmul(s1, s1, z2z2, F);
    fmul(s2, b->y, a->z, F);
    fmul(s2, s2, z1z1, F);
    fsub(h, u2, u1, F);
    fadd(i, h, h, F);
    fmul(i, i, i, F);
    fmul(j, h, i, F);
    fsub(rr, s2, s1, F);
    fadd(rr, rr, rr, F);
    fmul(v, u1, i, F);

    fmul(out.x, rr, rr, F);
    fsub(out.x, out.x, j, F);
    fsub(out.x, out.x, v, F);
    fsub(out.x, out.x, v, F);

    fsub(t, v, out.x, F);
    fmul(out.y, rr, t, F);
    fmul(t, s1, j, F);
    fadd(t, t, t, F);
    fsub(out.y, out.y, t, F);

    fadd(t, a->z, b->z, F);
    fmul(t, t, t, F);
    fsub(t, t, z1z1, F);
    fsub(t, t, z2z2, F);
    fmul(out.z, t, h, F);

    bn_select(out.x, b->x, NL, a_inf);
    bn_select(out.y, b->y, NL, a_inf);
    bn_select(out.z, b->z, NL, a_inf);
    bn_select(out.x, a->x, NL, b_inf);
    bn_select(out.y, a->y, NL, b_inf);
    bn_select(out.z, a->z, NL, b_inf);
    pcopy(r, &out);
}

static void pswap(jpoint *a, jpoint *b, bn_limb cond)
{
    bn_cswap(a->x, b->x, NL, cond);
    bn_cswap(a->y, b->y, NL, cond);
    bn_cswap(a->z, b->z, NL, cond);
}

/* Affine big-endian coordinates to a Jacobian point, validating them. */
static int load_point(jpoint *pt, const uint8_t x[32], const uint8_t y[32],
                      const field *F)
{
    bn_limb ax[NL], ay[NL], lhs[NL], rhs[NL], t[NL];

    if (bn_from_be(ax, NL, x, 32) != CRYPTO_OK ||
        bn_from_be(ay, NL, y, 32) != CRYPTO_OK ||
        bn_cmp(ax, F->p, NL) >= 0 || bn_cmp(ay, F->p, NL) >= 0) {
        return CRYPTO_ERR_BAD_ARG;
    }
    bn_to_mont(pt->x, ax, &F->f);
    bn_to_mont(pt->y, ay, &F->f);
    memcpy(pt->z, F->one, sizeof pt->z);

    /* y^2 == x^3 - 3x + b */
    fmul(lhs, pt->y, pt->y, F);
    fmul(rhs, pt->x, pt->x, F);
    fmul(rhs, rhs, pt->x, F);
    fadd(t, pt->x, pt->x, F);
    fadd(t, t, pt->x, F);
    fsub(rhs, rhs, t, F);
    fadd(rhs, rhs, F->b, F);
    return bn_cmp(lhs, rhs, NL) == 0 ? CRYPTO_OK : CRYPTO_ERR_BAD_ARG;
}

static int scalar_ok(const uint8_t d[32])
{
    bn_limb k[NL], n[NL];

    (void)bn_from_be(k, NL, d, 32);
    (void)bn_from_be(n, NL, P256_N, sizeof P256_N);
    return !bn_is_zero(k, NL) && bn_cmp(k, n, NL) < 0;
}

/* d * pt, returned affine. CRYPTO_ERR_BAD_ARG if the result is the identity,
 * which a valid scalar and a validated point cannot produce. */
static int scalar_mult(const uint8_t d[32], const jpoint *pt,
                       uint8_t x[32], uint8_t y[32], const field *F)
{
    jpoint r0, r1;
    bn_limb zi[NL], zi2[NL], t[NL], ord[NL];
    int i;
    int rc = CRYPTO_OK;

    memcpy(r0.x, F->one, sizeof r0.x);
    memcpy(r0.y, F->one, sizeof r0.y);
    bn_zero(r0.z, NL);
    pcopy(&r1, pt);

    for (i = 255; i >= 0; i--) {
        bn_limb bit = (bn_limb)((d[31 - i / 8] >> (i % 8)) & 1u);

        pswap(&r0, &r1, bit);
        padd(&r1, &r0, &r1, F);
        pdouble(&r0, &r0, F);
        pswap(&r0, &r1, bit);
    }

    if (bn_is_zero(r0.z, NL)) {
        rc = CRYPTO_ERR_BAD_ARG;
    } else {
        /* z^-1 = z^(p-2), in ordinary form for bn_modexp, then back. */
        bn_from_mont(ord, r0.z, &F->f);
        bn_modexp(t, ord, P256_PM2, sizeof P256_PM2, &F->f);
        bn_to_mont(zi, t, &F->f);
        fmul(zi2, zi, zi, F);
        fmul(t, r0.x, zi2, F);
        bn_from_mont(ord, t, &F->f);
        if (x != NULL) {
            bn_to_be(x, 32, ord, NL);
        }
        fmul(zi2, zi2, zi, F);
        fmul(t, r0.y, zi2, F);
        bn_from_mont(ord, t, &F->f);
        if (y != NULL) {
            bn_to_be(y, 32, ord, NL);
        }
    }
    crypto_wipe(&r0, sizeof r0);
    crypto_wipe(&r1, sizeof r1);
    crypto_wipe(zi, sizeof zi);
    crypto_wipe(zi2, sizeof zi2);
    crypto_wipe(t, sizeof t);
    crypto_wipe(ord, sizeof ord);
    return rc;
}

int p256_check_point(const uint8_t x[P256_BYTES], const uint8_t y[P256_BYTES])
{
    field F;
    jpoint pt;

    if (x == NULL || y == NULL) {
        return CRYPTO_ERR_BAD_ARG;
    }
    field_init(&F);
    return load_point(&pt, x, y, &F);
}

int p256_public(const uint8_t d[P256_BYTES],
                uint8_t x[P256_BYTES], uint8_t y[P256_BYTES])
{
    field F;
    jpoint g;

    if (d == NULL || x == NULL || y == NULL || !scalar_ok(d)) {
        return CRYPTO_ERR_BAD_ARG;
    }
    field_init(&F);
    (void)load_point(&g, P256_GX, P256_GY, &F);
    return scalar_mult(d, &g, x, y, &F);
}

int p256_ecdh(const uint8_t d[P256_BYTES],
              const uint8_t px[P256_BYTES], const uint8_t py[P256_BYTES],
              uint8_t zx[P256_BYTES])
{
    field F;
    jpoint pt;

    if (d == NULL || px == NULL || py == NULL || zx == NULL ||
        !scalar_ok(d)) {
        return CRYPTO_ERR_BAD_ARG;
    }
    field_init(&F);
    if (load_point(&pt, px, py, &F) != CRYPTO_OK) {
        return CRYPTO_ERR_BAD_ARG;
    }
    return scalar_mult(d, &pt, zx, NULL, &F);
}
