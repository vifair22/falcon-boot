/* Vector tests for the crypto under the TPM path (falcon-core/src/crypto:
 * cfb, bn, p256, kdf, rsa, otp, and hash.c's SHA-1 descriptor).
 *
 * Provenance of every expected value, and how it was confirmed:
 *   AES-CFB128      SP 800-38A Appendix F.3.13 (CFB128-AES128), confirmed
 *                   with `openssl enc -aes-128-cfb`, and a 19-byte prefix of
 *                   the same, which is the partial-block case.
 *   P-256 ECDH      NIST CAVS "ECC CDH Primitive" P-256 counts 0 and 1 (the
 *                   peer point, dIUT, QIUT and ZIUT), each re-derived with
 *                   `openssl pkeyutl -derive` from DER keys built of the raw
 *                   values; and d = n - 1, whose public point is -G and whose
 *                   shared x is the peer's own x, from the same OpenSSL run.
 *   KDFa, KDFe      TPM 2.0 Part 1 Section 11.4.10's formulas run by Python's
 *                   hmac and hashlib over fixed inputs; one-block and
 *                   two-block outputs, since the counter is where a KDF goes
 *                   wrong. The swtpm rig (make test-tpm) is the proof that
 *                   the reading of the formula is the TPM's.
 *   HMAC-SHA1       RFC 2202 test cases 1 and 2.
 *   RSAES-OAEP      see tpm_vectors.h.
 *   HOTP, TOTP      RFC 4226 Appendix D (all ten counts) and RFC 6238
 *                   Appendix B's SHA-1 column, truncated to 6 digits as
 *                   well as at the 8 the RFC prints.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include <crypto/aes.h>
#include <crypto/bn.h>
#include <crypto/cfb.h>
#include <crypto/crypto.h>
#include <crypto/hash.h>
#include <crypto/hmac.h>
#include <crypto/kdf.h>
#include <crypto/otp.h>
#include <crypto/p256.h>
#include <crypto/rsa.h>

#include "tpm_vectors.h"

static void unhex(const char *h, uint8_t *out, size_t n)
{
    size_t i;

    assert_int_equal(strlen(h), 2u * n);
    for (i = 0; i < n; i++) {
        unsigned int v = 0;

        assert_int_equal(sscanf(h + 2u * i, "%2x", &v), 1);
        out[i] = (uint8_t)v;
    }
}

static void assert_hex(const uint8_t *got, const char *want)
{
    uint8_t w[128];
    size_t n = strlen(want) / 2u;

    assert_true(n <= sizeof w);
    unhex(want, w, n);
    assert_memory_equal(got, w, n);
}

/* ---- AES-CFB128 ---- */

static const char *CFB_PT = "6bc1bee22e409f96e93d7e117393172a"
                            "ae2d8a571e03ac9c9eb76fac45af8e51"
                            "30c81c46a35ce411e5fbc1191a0a52ef"
                            "f69f2445df4f9b17ad2b417be66c3710";
static const char *CFB_CT = "3b3fd92eb72dad20333449f8e83cfb4a"
                            "c8a64537a0b3a93fcde3cdad9f1ce58b"
                            "26751f67a3cbb140b1808cf187a4f4df"
                            "c04b05357c5d1c0eeac4c66f9ff7f2e6";

static void test_cfb_vectors(void **state)
{
    uint8_t key[16], iv[16], pt[64], ct[64], buf[64];
    aes_key k;

    (void)state;
    unhex("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
    unhex("000102030405060708090a0b0c0d0e0f", iv, 16);
    unhex(CFB_PT, pt, 64);
    unhex(CFB_CT, ct, 64);
    assert_int_equal(aes_setkey(&k, key, 16), CRYPTO_OK);

    aes_cfb_encrypt(&k, iv, pt, buf, 64);
    assert_memory_equal(buf, ct, 64);
    aes_cfb_decrypt(&k, iv, ct, buf, 64);
    assert_memory_equal(buf, pt, 64);

    /* A partial last block uses the head of its keystream block. */
    aes_cfb_encrypt(&k, iv, pt, buf, 19);
    assert_memory_equal(buf, ct, 19);

    /* In place, both directions. */
    memcpy(buf, pt, 64);
    aes_cfb_encrypt(&k, iv, buf, buf, 64);
    assert_memory_equal(buf, ct, 64);
    aes_cfb_decrypt(&k, iv, buf, buf, 64);
    assert_memory_equal(buf, pt, 64);
}

/* ---- bn ---- */

static void test_bn_basics(void **state)
{
    bn_limb a[4], b[4], r[4];
    bn_mont c;
    uint8_t be[20];
    static const uint8_t wide[9] = { 1, 0, 0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t padded[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 7 };
    static const bn_limb even[1] = { 10 };

    (void)state;
    /* Too wide for the limbs, and leading zeros that are fine. */
    assert_int_equal(bn_from_be(a, 2, wide, sizeof wide), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(bn_from_be(a, 2, padded, sizeof padded), CRYPTO_OK);
    assert_int_equal(a[0], 7u);
    assert_int_equal(a[1], 0u);
    assert_int_equal(bn_from_be(NULL, 2, padded, 1), CRYPTO_ERR_BAD_ARG);

    /* Output wider than the number is zero-padded. */
    bn_to_be(be, sizeof be, a, 2);
    assert_int_equal(be[19], 7u);
    assert_int_equal(be[0], 0u);

    a[0] = 0xffffffffu; a[1] = 0xffffffffu; a[2] = 0; a[3] = 0;
    b[0] = 1; b[1] = 0; b[2] = 0; b[3] = 0;
    assert_int_equal(bn_add(r, a, b, 4), 0u);
    assert_int_equal(r[2], 1u);
    assert_int_equal(bn_sub(r, b, a, 4), 1u);
    assert_int_equal(bn_cmp(a, b, 4), 1);
    assert_int_equal(bn_cmp(b, a, 4), -1);
    assert_int_equal(bn_cmp(a, a, 4), 0);
    assert_false(bn_is_zero(a, 4));

    memcpy(r, b, sizeof r);
    bn_cswap(r, a, 4, 1);
    assert_int_equal(r[0], 0xffffffffu);
    assert_int_equal(a[0], 1u);
    bn_select(r, a, 4, 0);
    assert_int_equal(r[0], 0xffffffffu);

    assert_int_equal(bn_mont_init(&c, even, 1), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(bn_mont_init(NULL, even, 1), CRYPTO_ERR_BAD_ARG);
}

/* 3^200 mod 1000003, a small modexp Python's pow() confirms: 333986. */
static void test_bn_modexp_small(void **state)
{
    static const bn_limb m[1] = { 1000003u };
    bn_limb base[1] = { 3 }, r[1];
    static const uint8_t e[1] = { 200 };
    bn_mont c;

    (void)state;
    assert_int_equal(bn_mont_init(&c, m, 1), CRYPTO_OK);
    bn_modexp(r, base, e, sizeof e, &c);
    assert_int_equal(r[0], 333986u);
}

/* ---- P-256 ---- */

typedef struct {
    const char *qx, *qy, *d, *ux, *uy, *z;
} cdh_vec;

static const cdh_vec CDH[] = {
    {   "700c48f77f56584c5cc632ca65640db91b6bacce3a4df6b42ce7cc838833d287",
        "db71e509e3fd9b060ddb20ba5c51dcc5948d46fbf640dfe0441782cab85fa4ac",
        "7d7dc5f71eb29ddaf80d6214632eeae03d9058af1fb6d22ed80badb62bc1a534",
        "ead218590119e8876b29146ff89ca61770c4edbbf97d38ce385ed281d8a6b230",
        "28af61281fd35e2fa7002523acc85a429cb06ee6648325389f59edfce1405141",
        "46fc62106420ff012e54a434fbdd2d25ccc5852060561e68040dd7778997bd7b" },
    {   "809f04289c64348c01515eb03d5ce7ac1a8cb9498f5caa50197e58d43a86a7ae",
        "b29d84e811197f25eba8f5194092cb6ff440e26d4421011372461f579271cda3",
        "38f65d6dce47676044d58ce5139582d568f64bb16098d179dbab07741dd5caf5",
        "119f2f047902782ab0c9e27a54aff5eb9b964829ca99c06b02ddba95b0a3f6d0",
        "8f52b726664cac366fc98ac7a012b2682cbd962e5acb544671d41b9445704d1d",
        "057d636096cb80b67a8c038c890e887d1adfa4195e9b3ce241c8a778c59cda67" },
    /* d = n - 1: the public point is -G, and the shared x is the peer's. */
    {   "809f04289c64348c01515eb03d5ce7ac1a8cb9498f5caa50197e58d43a86a7ae",
        "b29d84e811197f25eba8f5194092cb6ff440e26d4421011372461f579271cda3",
        "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632550",
        "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
        "b01cbd1c01e58065711814b583f061e9d431cca994cea1313449bf97c840ae0a",
        "809f04289c64348c01515eb03d5ce7ac1a8cb9498f5caa50197e58d43a86a7ae" },
};

static void test_p256_cdh(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < sizeof CDH / sizeof CDH[0]; i++) {
        uint8_t qx[32], qy[32], d[32], x[32], y[32], z[32];

        unhex(CDH[i].qx, qx, 32);
        unhex(CDH[i].qy, qy, 32);
        unhex(CDH[i].d, d, 32);
        assert_int_equal(p256_check_point(qx, qy), CRYPTO_OK);
        assert_int_equal(p256_public(d, x, y), CRYPTO_OK);
        assert_hex(x, CDH[i].ux);
        assert_hex(y, CDH[i].uy);
        assert_int_equal(p256_ecdh(d, qx, qy, z), CRYPTO_OK);
        assert_hex(z, CDH[i].z);
    }
}

static void test_p256_rejects(void **state)
{
    uint8_t qx[32], qy[32], d[32], x[32], y[32], z[32];

    (void)state;
    unhex(CDH[0].qx, qx, 32);
    unhex(CDH[0].qy, qy, 32);
    unhex(CDH[0].d, d, 32);

    /* Off the curve. */
    qy[31] ^= 1u;
    assert_int_equal(p256_check_point(qx, qy), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(p256_ecdh(d, qx, qy, z), CRYPTO_ERR_BAD_ARG);
    qy[31] ^= 1u;

    /* A coordinate not below p. */
    memset(x, 0xff, sizeof x);
    assert_int_equal(p256_check_point(x, qy), CRYPTO_ERR_BAD_ARG);

    /* Scalars 0 and n. */
    memset(d, 0, sizeof d);
    assert_int_equal(p256_public(d, x, y), CRYPTO_ERR_BAD_ARG);
    unhex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
          d, 32);
    assert_int_equal(p256_public(d, x, y), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(p256_ecdh(d, qx, qy, z), CRYPTO_ERR_BAD_ARG);

    assert_int_equal(p256_check_point(NULL, qy), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(p256_public(NULL, x, y), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(p256_ecdh(d, NULL, qy, z), CRYPTO_ERR_BAD_ARG);
}

/* ---- KDFa, KDFe ---- */

static void test_kdf_vectors(void **state)
{
    uint8_t key[32], u[32], v[32], w[16], out[48];
    size_t i;

    (void)state;
    for (i = 0; i < 32; i++) {
        key[i] = (uint8_t)i;
    }
    memset(u, 0x11, sizeof u);
    memset(v, 0x22, sizeof v);
    memset(w, 0x33, sizeof w);

    assert_int_equal(kdfa_sha256(key, 32, "ATH", u, 32, v, 32, out, 256),
                     CRYPTO_OK);
    assert_hex(out, "a8679444f320a6d0a02c03cac153c458"
                    "f28758eca056594e887fcefdc2d7e95b");
    assert_int_equal(kdfa_sha256(key, 32, "CFB", u, 32, v, 32, out, 384),
                     CRYPTO_OK);
    assert_hex(out, "1f9e15f43d10d7773bf74fb59809bbd7"
                    "8172bcdfc98e3ca5b328f5104c114521"
                    "771745815786035bba3f4656a1770738");
    /* An empty key and an empty second context: HMAC under a zero-length
     * key, which a session with no salt and no bind would use. */
    assert_int_equal(kdfa_sha256(NULL, 0, "CFB", w, 16, NULL, 0, out, 256),
                     CRYPTO_OK);
    assert_hex(out, "5dfdb04440a901941b4007f466297c63"
                    "2999fb68be87aff5e6cee8faecd33362");

    assert_int_equal(kdfe_sha256(key, 32, "SECRET", u, 32, v, 32, out, 384),
                     CRYPTO_OK);
    assert_hex(out, "c21be073521259773cab940e5cbd0a66"
                    "105a8da939b0b3ae13b7d7a37555b3e3"
                    "453c92de4e6ae4904f66ca7599b30834");

    assert_int_equal(kdfa_sha256(key, 32, "ATH", u, 32, v, 32, out, 12),
                     CRYPTO_ERR_BAD_ARG);
    assert_int_equal(kdfa_sha256(key, 32, NULL, u, 32, v, 32, out, 256),
                     CRYPTO_ERR_BAD_ARG);
    assert_int_equal(kdfe_sha256(key, 32, "SECRET", NULL, 32, v, 32, out, 256),
                     CRYPTO_ERR_BAD_ARG);
    assert_int_equal(kdfe_sha256(key, 32, "SECRET", u, 32, v, 32, NULL, 256),
                     CRYPTO_ERR_BAD_ARG);
}

/* ---- HMAC-SHA1 ---- */

static void test_hmac_sha1(void **state)
{
    uint8_t key[20], out[20];

    (void)state;
    memset(key, 0x0b, sizeof key);
    assert_int_equal(hmac(hash_sha1(), key, 20, "Hi There", 8, out),
                     CRYPTO_OK);
    assert_hex(out, "b617318655057264e28bc0b6fb378c8ef146be00");
    assert_int_equal(hmac(hash_sha1(), "Jefe", 4,
                          "what do ya want for nothing?", 28, out),
                     CRYPTO_OK);
    assert_hex(out, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    /* Outside the LUKS name table on purpose. */
    assert_null(hash_by_name("sha1", 4));
}

/* ---- RSAES-OAEP ---- */

static void test_rsa_oaep(void **state)
{
    uint8_t msg[22], seed[20], out[256];
    static const uint8_t e[3] = { 0x01, 0x00, 0x01 };
    static const uint8_t label[4] = { 'T', 'C', 'P', 'A' };
    uint8_t padded[257];
    size_t i;

    (void)state;
    msg[0] = 0x00;
    msg[1] = 0x1d;
    for (i = 0; i < 20; i++) {
        msg[2 + i] = (uint8_t)(0x40 + i);
        seed[i] = (uint8_t)(0xa0 + i);
    }
    assert_int_equal(rsa_oaep_sha1_encrypt(RSA_N, sizeof RSA_N, e, sizeof e,
                                           label, sizeof label, msg,
                                           sizeof msg, seed, out),
                     CRYPTO_OK);
    assert_memory_equal(out, RSA_CT, sizeof RSA_CT);

    /* A modulus given with a leading zero byte is the same modulus. */
    padded[0] = 0;
    memcpy(padded + 1, RSA_N, sizeof RSA_N);
    assert_int_equal(rsa_oaep_sha1_encrypt(padded, sizeof padded, e, sizeof e,
                                           label, sizeof label, msg,
                                           sizeof msg, seed, out),
                     CRYPTO_OK);
    assert_memory_equal(out, RSA_CT, sizeof RSA_CT);

    /* A message too long for the modulus, and bad arguments. */
    assert_int_equal(rsa_oaep_sha1_encrypt(RSA_N, 64, e, sizeof e, label,
                                           sizeof label, msg, sizeof msg,
                                           seed, out),
                     CRYPTO_ERR_BAD_ARG);
    assert_int_equal(rsa_oaep_sha1_encrypt(RSA_N, sizeof RSA_N, e, 0, label,
                                           sizeof label, msg, sizeof msg,
                                           seed, out),
                     CRYPTO_ERR_BAD_ARG);
    assert_int_equal(rsa_oaep_sha1_encrypt(padded, sizeof padded - 1u, e,
                                           sizeof e, label, sizeof label,
                                           msg, sizeof msg, seed, out),
                     CRYPTO_ERR_BAD_ARG);   /* now even: not a modulus */
}

/* ---- HOTP, TOTP ---- */

static void test_hotp_rfc4226(void **state)
{
    static const uint32_t want[10] = {
        755224, 287082, 359152, 969429, 338314,
        254676, 287922, 162583, 399871, 520489
    };
    const uint8_t *key = (const uint8_t *)"12345678901234567890";
    uint32_t code;
    uint64_t c;

    (void)state;
    for (c = 0; c < 10; c++) {
        assert_int_equal(hotp_sha1(key, 20, c, 6, &code), CRYPTO_OK);
        assert_int_equal(code, want[c]);
    }
    assert_int_equal(hotp_sha1(key, 20, 0, 5, &code), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(hotp_sha1(NULL, 20, 0, 6, &code), CRYPTO_ERR_BAD_ARG);
}

static void test_totp_rfc6238(void **state)
{
    static const struct {
        uint64_t t;
        uint32_t code8;
    } v[] = {
        { 59u, 94287082u },
        { 1111111109u, 7081804u },
        { 1111111111u, 14050471u },
        { 1234567890u, 89005924u },
        { 2000000000u, 69279037u },
        { 20000000000u, 65353130u },
    };
    const uint8_t *key = (const uint8_t *)"12345678901234567890";
    uint32_t code;
    size_t i;

    (void)state;
    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        assert_int_equal(totp_sha1(key, 20, v[i].t, TOTP_STEP, 8, &code),
                         CRYPTO_OK);
        assert_int_equal(code, v[i].code8);
        assert_int_equal(totp_sha1(key, 20, v[i].t, TOTP_STEP, TOTP_DIGITS,
                                   &code),
                         CRYPTO_OK);
        assert_int_equal(code, v[i].code8 % 1000000u);
    }
    assert_int_equal(totp_sha1(key, 20, 59, 0, 6, &code), CRYPTO_ERR_BAD_ARG);
}

int run_tpmcrypto_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_cfb_vectors),
        cmocka_unit_test(test_bn_basics),
        cmocka_unit_test(test_bn_modexp_small),
        cmocka_unit_test(test_p256_cdh),
        cmocka_unit_test(test_p256_rejects),
        cmocka_unit_test(test_kdf_vectors),
        cmocka_unit_test(test_hmac_sha1),
        cmocka_unit_test(test_rsa_oaep),
        cmocka_unit_test(test_hotp_rfc4226),
        cmocka_unit_test(test_totp_rfc6238),
    };

    return cmocka_run_group_tests_name("tpmcrypto", tests, NULL, NULL);
}
