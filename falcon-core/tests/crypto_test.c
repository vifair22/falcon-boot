/* Vector tests for falcon-core/src/crypto.
 *
 * This is the one module in the repo where "it looks right" is worth nothing.
 * Hand-written SHA-2, HMAC, PBKDF2 and AES-XTS that have only ever been
 * checked against themselves will happily be self-consistently wrong, and the
 * failure mode is not a crash: it is a master key that does not verify, which
 * is indistinguishable at the UX layer from the user mistyping their
 * passphrase. So every primitive here is pinned to a published vector from
 * the document that defines it.
 *
 * Provenance of every expected value below, and how it was confirmed:
 *   SHA-1                 FIPS 180-2 Appendix A, and the same padding
 *                         boundaries as SHA-256 (it names WIM streams).
 *   SHA-256, SHA-512      FIPS 180-4 / the standard one- and two-block
 *                         examples, plus lengths chosen to straddle the
 *                         padding boundary (55/56 for a 64-byte block,
 *                         111/112 for a 128-byte one), which is where a
 *                         length-encoding bug hides.
 *   HMAC                  RFC 4231 test cases 1, 2, 6 and 7. Cases 6 and 7
 *                         use a 131-byte key, longer than either block size,
 *                         so they are the ones that exercise RFC 2104's
 *                         hash-the-key rule for both hashes at once.
 *   PBKDF2-HMAC-SHA256    the RFC 6070 parameter sets restated for SHA-256,
 *                         including the c=4096 dkLen=40 case that spans two
 *                         PRF blocks and so pins the big-endian block counter.
 *   PBKDF2-HMAC-SHA512    the same "password"/"salt" parameters.
 *   AES ECB               FIPS 197 Appendix C.1 (AES-128) and C.3 (AES-256).
 *   AES-XTS               IEEE 1619 vector 10 (256-bit keys, data unit 0xff).
 *
 * Each was re-derived on this machine from an independent implementation
 * before being written down: the hashes, HMACs and PBKDF2 outputs from
 * OpenSSL through python3's hashlib, the AES blocks from `openssl enc`, and
 * the XTS vector from the kernel's own dm-crypt (a plain aes-xts-plain64
 * mapping written at sector 0xff). That last one matters more than the
 * others: dm-crypt is precisely the producer whose output the loader has to
 * read back, so agreeing with it is the property under test, not a proxy for
 * it.
 *
 * Both XTS implementations are exercised. xts_setkey_portable forces aes.c
 * regardless of CPUID, and the AES-NI path is entered through xts_setkey and
 * then confirmed to have been selected via xts_impl; on a CPU without the
 * instructions those comparisons skip rather than fail, because there is
 * nothing there to compare against.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include <crypto/crypto.h>
#include <crypto/sha1.h>
#include <crypto/hash.h>
#include <crypto/hmac.h>
#include <crypto/pbkdf2.h>
#include <crypto/aes.h>
#include <crypto/xts.h>

/* ------------------------------------------------------------- helpers */

static uint8_t hex_nib(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint8_t)(c - 'a' + 10);
    }
    assert_true(c >= 'A' && c <= 'F');
    return (uint8_t)(c - 'A' + 10);
}

/* Parses a hex literal into out and returns the byte count. The vectors are
 * written as hex strings rather than brace-initialised byte arrays so they
 * can be compared character for character against the standards documents
 * they came from. */
static size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex);
    size_t i;

    assert_int_equal(n % 2u, 0);
    assert_true(n / 2u <= cap);
    for (i = 0; i < n / 2u; i++) {
        out[i] = (uint8_t)((hex_nib(hex[2u * i]) << 4) |
                           hex_nib(hex[2u * i + 1u]));
    }
    return n / 2u;
}

/* Compares a buffer against a hex literal, reporting the offending byte
 * rather than "assert_memory_equal failed". */
static void assert_hex(const uint8_t *got, size_t got_len, const char *want)
{
    uint8_t exp[512];
    size_t n = unhex(want, exp, sizeof exp);
    size_t i;

    assert_int_equal(got_len, n);
    for (i = 0; i < n; i++) {
        if (got[i] != exp[i]) {
            fail_msg("byte %zu: got 0x%02x, want 0x%02x", i, got[i], exp[i]);
        }
    }
}

/* ------------------------------------------------------------- SHA-256 */

/* msg is a literal when lit is nonzero, otherwise `len` bytes of 'a'. The
 * repeated-'a' rows exist only to place the message length either side of a
 * padding boundary. */
typedef struct {
    const char *label;
    const char *lit;
    size_t      len;
    const char *want;
} hash_vec;

static const hash_vec sha256_vecs[] = {
    { "abc", "abc", 0,
      "ba7816bf8f01cfea414140de5dae2223"
      "b00361a396177a9cb410ff61f20015ad" },
    { "empty", "", 0,
      "e3b0c44298fc1c149afbf4c8996fb924"
      "27ae41e4649b934ca495991b7852b855" },
    { "two-block",
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 0,
      "248d6a61d20638b8e5c026930c3e6039"
      "a33ce45964ff2167f6ecedd419db06c1" },
    /* 55 bytes: the last length whose 1-bit and 8-byte length field still
     * fit in the same 64-byte block. 56 is the first that does not, so the
     * padding spills into a second block. */
    { "55 x 'a'", NULL, 55,
      "9f4390f8d30c2dd92ec9f095b65e2b9a"
      "e9b0a925a5258e241c9f1e910f734318" },
    { "56 x 'a'", NULL, 56,
      "b35439a4ac6f0948b6d6f9e3c6af0f5f"
      "590ce20f1bde7090ef7970686ec6738a" },
    { "63 x 'a'", NULL, 63,
      "7d3e74a05d7db15bce4ad9ec0658ea98"
      "e3f06eeecf16b4c6fff2da457ddc2f34" },
    { "64 x 'a'", NULL, 64,
      "ffe054fe7ae0cb6dc65c3af9b61d5209"
      "f439851db43d0ba5997337df154668eb" },
};

static const hash_vec sha512_vecs[] = {
    { "abc", "abc", 0,
      "ddaf35a193617abacc417349ae204131"
      "12e6fa4e89a97ea20a9eeee64b55d39a"
      "2192992a274fc1a836ba3c23a3feebbd"
      "454d4423643ce80e2a9ac94fa54ca49f" },
    { "empty", "", 0,
      "cf83e1357eefb8bdf1542850d66d8007"
      "d620e4050b5715dc83f4a921d36ce9ce"
      "47d0d13c5d85f2b0ff8318d2877eec2f"
      "63b931bd47417a81a538327af927da3e" },
    { "two-block",
      "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
      "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 0,
      "8e959b75dae313da8cf4f72814fc143f"
      "8f7779c6eb9f7fa17299aeadb6889018"
      "501d289e4900f7e4331b99dec4b5433a"
      "c7d329eeb6dd26545e96e55b874be909" },
    /* 111/112 is the 128-byte block's padding boundary. */
    { "111 x 'a'", NULL, 111,
      "fa9121c7b32b9e01733d034cfc78cbf6"
      "7f926c7ed83e82200ef8681819692176"
      "0b4beff48404df811b95382827446167"
      "3c68d04e297b0eb7b2b4d60fc6b566a2" },
    { "112 x 'a'", NULL, 112,
      "c01d080efd492776a1c43bd23dd99d0a"
      "2e626d481e16782e75d54c2503b5dc32"
      "bd05f0f1ba33e568b88fd2d970929b71"
      "9ecbb152f58f130a407c8830604b70ca" },
};

/* FIPS 180-2 Appendix A, and the 55/56 and 63/64 padding boundaries. */
static const hash_vec sha1_vecs[] = {
    { "abc", "abc", 0, "a9993e364706816aba3e25717850c26c9cd0d89d" },
    { "empty", "", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709" },
    { "two-block", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
      0, "84983e441c3bd26ebaae4aa1f95129e5e54670f1" },
    { "55 x 'a'", NULL, 55, "c1c8bbdc22796e28c0e15163d20899b65621d65a" },
    { "56 x 'a'", NULL, 56, "c2db330f6083854c99d4b5bfb6e8f29f201be699" },
    { "63 x 'a'", NULL, 63, "03f09f5b158a7a8cdad920bddc29b81c18a551f5" },
    { "64 x 'a'", NULL, 64, "0098ba824b5c16427bd7a1122a5a442a25ec644d" },
};

#define MSG_MAX 256

static size_t vec_msg(const hash_vec *v, uint8_t *buf)
{
    if (v->lit != NULL) {
        size_t n = strlen(v->lit);

        assert_true(n <= MSG_MAX);
        memcpy(buf, v->lit, n);
        return n;
    }
    assert_true(v->len <= MSG_MAX);
    memset(buf, 'a', v->len);
    return v->len;
}

static void test_sha256_vectors(void **state)
{
    uint8_t msg[MSG_MAX];
    uint8_t out[SHA256_DIGEST_LEN];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof sha256_vecs / sizeof sha256_vecs[0]; i++) {
        const hash_vec *v = &sha256_vecs[i];
        size_t n = vec_msg(v, msg);

        sha256(msg, n, out);
        assert_hex(out, sizeof out, v->want);
    }
}

static void test_sha1_vectors(void **state)
{
    uint8_t msg[MSG_MAX];
    uint8_t out[SHA1_DIGEST_LEN];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof sha1_vecs / sizeof sha1_vecs[0]; i++) {
        const hash_vec *v = &sha1_vecs[i];
        size_t n = vec_msg(v, msg);
        sha1_ctx c;
        size_t k;

        sha1(msg, n, out);
        assert_hex(out, sizeof out, v->want);
        /* The same bytes one at a time. */
        sha1_init(&c);
        for (k = 0; k < n; k++)
            sha1_update(&c, msg + k, 1);
        sha1_update(&c, NULL, 0);
        sha1_final(&c, out);
        assert_hex(out, sizeof out, v->want);
    }
}

static void test_sha512_vectors(void **state)
{
    uint8_t msg[MSG_MAX];
    uint8_t out[SHA512_DIGEST_LEN];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof sha512_vecs / sizeof sha512_vecs[0]; i++) {
        const hash_vec *v = &sha512_vecs[i];
        size_t n = vec_msg(v, msg);

        sha512(msg, n, out);
        assert_hex(out, sizeof out, v->want);
    }
}

/* Splitting a message at every possible offset walks the context's buffer
 * through every fill level, which is where an off-by-one in the "how much
 * fits in buf" arithmetic lives. The one-shot path above never reaches it. */
static void test_sha_incremental_split(void **state)
{
    static const char msg[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    size_t n = sizeof msg - 1u;
    uint8_t want256[SHA256_DIGEST_LEN], want512[SHA512_DIGEST_LEN];
    uint8_t got256[SHA256_DIGEST_LEN], got512[SHA512_DIGEST_LEN];
    size_t split;

    (void)state;
    sha256(msg, n, want256);
    sha512(msg, n, want512);

    for (split = 0; split <= n; split++) {
        sha256_ctx c2;
        sha512_ctx c5;

        sha256_init(&c2);
        sha256_update(&c2, msg, split);
        sha256_update(&c2, msg + split, n - split);
        sha256_final(&c2, got256);
        assert_memory_equal(got256, want256, sizeof want256);

        sha512_init(&c5);
        sha512_update(&c5, msg, split);
        sha512_update(&c5, msg + split, n - split);
        sha512_final(&c5, got512);
        assert_memory_equal(got512, want512, sizeof want512);
    }
}

/* The header promises final() leaves the context wiped. Nothing in the
 * loader depends on it, but it is the kind of promise that silently stops
 * being true. */
static void test_sha_final_wipes_context(void **state)
{
    sha256_ctx c2;
    sha512_ctx c5;
    uint8_t zero[sizeof c5];
    uint8_t out[SHA512_DIGEST_LEN];

    (void)state;
    memset(zero, 0, sizeof zero);

    sha256_init(&c2);
    sha256_update(&c2, "abc", 3);
    sha256_final(&c2, out);
    assert_memory_equal(&c2, zero, sizeof c2);

    sha512_init(&c5);
    sha512_update(&c5, "abc", 3);
    /* A zero-length update and a NULL buffer are both no-ops rather than
     * errors, and they are separate arms of one guard. sha256 gets this from
     * the split-update loop above; sha512's arm was never the deciding one. */
    sha512_update(&c5, "ignored", 0);
    sha512_update(&c5, NULL, 4u);
    sha512_final(&c5, out);
    assert_memory_equal(&c5, zero, sizeof c5);
}

/* ------------------------------------------------------ hash_by_name */

static void test_hash_by_name(void **state)
{
    (void)state;

    assert_ptr_equal(hash_by_name("sha256", 6), hash_sha256());
    assert_ptr_equal(hash_by_name("sha512", 6), hash_sha512());

    /* The span need not be terminated: this is how a JSON string view is
     * passed straight in. */
    assert_ptr_equal(hash_by_name("sha256\"}", 6), hash_sha256());

    assert_null(hash_by_name("sha1", 4));
    assert_null(hash_by_name("sha384", 6));
    assert_null(hash_by_name("argon2id", 8));
    assert_null(hash_by_name("", 0));
    assert_null(hash_by_name(NULL, 6));

    /* Case sensitive by contract: hash.c compares the raw bytes, and a
     * LUKS2 header writes the names lowercase. A header that spelled it
     * otherwise is not one cryptsetup wrote, so refusing is correct. */
    assert_null(hash_by_name("SHA256", 6));
    assert_null(hash_by_name("Sha256", 6));

    /* A prefix or an over-long span must not match. */
    assert_null(hash_by_name("sha256", 5));
    assert_null(hash_by_name("sha2567", 7));

    assert_int_equal(hash_sha256()->digest_len, SHA256_DIGEST_LEN);
    assert_int_equal(hash_sha256()->block_len, SHA256_BLOCK_LEN);
    assert_int_equal(hash_sha512()->digest_len, SHA512_DIGEST_LEN);
    assert_int_equal(hash_sha512()->block_len, SHA512_BLOCK_LEN);
    assert_string_equal(hash_sha256()->name, "sha256");
    assert_string_equal(hash_sha512()->name, "sha512");
}

/* ---------------------------------------------------------------- HMAC */

/* key is `key_fill` repeated `key_len` times when key_lit is NULL. RFC 4231
 * writes its keys that way and cases 6 and 7 use 131 bytes of 0xaa, which is
 * longer than either block size. */
typedef struct {
    const char *label;
    const char *key_lit;
    uint8_t     key_fill;
    size_t      key_len;
    const char *data;
    const char *want256;
    const char *want512;
} hmac_vec;

static const hmac_vec hmac_vecs[] = {
    { "RFC 4231 case 1", NULL, 0x0b, 20, "Hi There",
      "b0344c61d8db38535ca8afceaf0bf12b"
      "881dc200c9833da726e9376c2e32cff7",
      "87aa7cdea5ef619d4ff0b4241a1d6cb0"
      "2379f4e2ce4ec2787ad0b30545e17cde"
      "daa833b7d6b8a702038b274eaea3f4e4"
      "be9d914eeb61f1702e696c203a126854" },
    { "RFC 4231 case 2", "Jefe", 0, 0, "what do ya want for nothing?",
      "5bdcc146bf60754e6a042426089575c7"
      "5a003f089d2739839dec58b964ec3843",
      "164b7a7bfcf819e2e395fbe73b56e0a3"
      "87bd64222e831fd610270cd7ea250554"
      "9758bf75c05a994a6d034f65f8f0e6fd"
      "caeab1a34d4a6b4b636e070a38bce737" },
    { "RFC 4231 case 6 (131-byte key)", NULL, 0xaa, 131,
      "Test Using Larger Than Block-Size Key - Hash Key First",
      "60e431591ee0b67f0d8a26aacbf5b77f"
      "8e0bc6213728c5140546040f0ee37f54",
      "80b24263c7c1a3ebb71493c1dd7be8b4"
      "9b46d1f41b4aeec1121b013783f8f352"
      "6b56d037e05f2598bd0fd2215d6a1e52"
      "95e64f73f63f0aec8b915a985d786598" },
    { "RFC 4231 case 7 (131-byte key, long data)", NULL, 0xaa, 131,
      "This is a test using a larger than block-size key and a larger "
      "than block-size data. The key needs to be hashed before being "
      "used by the HMAC algorithm.",
      "9b09ffa71b942fcb27635fbcd5b0e944"
      "bfdc63644f0713938a7f51535c3a35e2",
      "e37b6a775dc87dbaa4dfa9f96e5e3ffd"
      "debd71f8867289865df5a32d20cdc944"
      "b6022cac3c4982b10d5eeb55c3e4de15"
      "134676fb6de0446065c97440fa8c6a58" },
};

#define HMAC_KEY_MAX 200

static size_t hmac_vec_key(const hmac_vec *v, uint8_t *buf)
{
    if (v->key_lit != NULL) {
        size_t n = strlen(v->key_lit);

        assert_true(n <= HMAC_KEY_MAX);
        memcpy(buf, v->key_lit, n);
        return n;
    }
    assert_true(v->key_len <= HMAC_KEY_MAX);
    memset(buf, v->key_fill, v->key_len);
    return v->key_len;
}

static void test_hmac_vectors(void **state)
{
    uint8_t key[HMAC_KEY_MAX];
    uint8_t out[HASH_MAX_DIGEST];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof hmac_vecs / sizeof hmac_vecs[0]; i++) {
        const hmac_vec *v = &hmac_vecs[i];
        size_t klen = hmac_vec_key(v, key);

        assert_int_equal(hmac(hash_sha256(), key, klen,
                              v->data, strlen(v->data), out), CRYPTO_OK);
        assert_hex(out, SHA256_DIGEST_LEN, v->want256);

        assert_int_equal(hmac(hash_sha512(), key, klen,
                              v->data, strlen(v->data), out), CRYPTO_OK);
        assert_hex(out, SHA512_DIGEST_LEN, v->want512);
    }
}

/* The precomputed-pad path is the one PBKDF2 actually runs, and it is a
 * different code path from the one-shot hmac() above only in that the key
 * schedule is reused. Reusing one hmac_key for several MACs is exactly what
 * PBKDF2 does a million times, so a state leak between hmac_begin calls
 * would corrupt every derivation after the first. */
static void test_hmac_key_reuse(void **state)
{
    uint8_t key[HMAC_KEY_MAX];
    uint8_t oneshot[HASH_MAX_DIGEST];
    uint8_t streamed[HASH_MAX_DIGEST];
    hmac_key k;
    size_t i;

    (void)state;
    for (i = 0; i < sizeof hmac_vecs / sizeof hmac_vecs[0]; i++) {
        const hmac_vec *v = &hmac_vecs[i];
        size_t klen = hmac_vec_key(v, key);
        size_t dlen = strlen(v->data);
        size_t rep;

        assert_int_equal(hmac_key_init(&k, hash_sha256(), key, klen),
                         CRYPTO_OK);
        assert_int_equal(hmac(hash_sha256(), key, klen, v->data, dlen,
                              oneshot), CRYPTO_OK);

        /* Three MACs from the one schedule, the middle one fed in pieces. */
        for (rep = 0; rep < 3; rep++) {
            hmac_ctx c;

            hmac_begin(&c, &k);
            if (rep == 1u && dlen > 3u) {
                hmac_update(&c, v->data, 3);
                hmac_update(&c, v->data + 3, dlen - 3u);
            } else {
                hmac_update(&c, v->data, dlen);
            }
            hmac_final(&c, streamed);
            assert_memory_equal(streamed, oneshot, SHA256_DIGEST_LEN);
        }
        crypto_wipe(&k, sizeof k);
    }
}

static void test_hmac_bad_args(void **state)
{
    uint8_t out[HASH_MAX_DIGEST];
    hmac_key k;

    (void)state;
    assert_int_equal(hmac_key_init(NULL, hash_sha256(), "k", 1),
                     CRYPTO_ERR_BAD_ARG);
    assert_int_equal(hmac_key_init(&k, NULL, "k", 1), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(hmac_key_init(&k, hash_sha256(), NULL, 1),
                     CRYPTO_ERR_BAD_ARG);
    assert_int_equal(hmac(NULL, "k", 1, "d", 1, out), CRYPTO_ERR_BAD_ARG);

    /* A zero-length key is legal (RFC 2104 zero-extends it), and a NULL
     * pointer is fine when the length is zero. */
    assert_int_equal(hmac_key_init(&k, hash_sha256(), NULL, 0), CRYPTO_OK);
    assert_int_equal(hmac(hash_sha256(), NULL, 0, "d", 1, out), CRYPTO_OK);
}

/* -------------------------------------------------------------- PBKDF2 */

typedef struct {
    const char *label;
    int         sha512;
    const char *pass;
    const char *salt;
    uint32_t    iters;
    size_t      dklen;
    const char *want;
} pbkdf2_vec;

static const pbkdf2_vec pbkdf2_vecs[] = {
    { "sha256 c=1", 0, "password", "salt", 1, 32,
      "120fb6cffcf8b32c43e7225256c4f837"
      "a86548c92ccc35480805987cb70be17b" },
    { "sha256 c=2", 0, "password", "salt", 2, 32,
      "ae4d0c95af6b46d32d0adff928f06dd0"
      "2a303f8ef3c251dfd6e2d85a95474c43" },
    { "sha256 c=4096", 0, "password", "salt", 4096, 32,
      "c5e478d59288c841aa530db6845c4c8d"
      "962893a001ce4e11a4963873aa98134a" },
    /* dkLen 40 > the 32-byte digest, so the derivation spans two PRF blocks
     * and the big-endian block counter is what makes the second one right. */
    { "sha256 c=4096 dkLen=40", 0,
      "passwordPASSWORDpassword",
      "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 40,
      "348c89dbcbd32b2f32d814b8116e84cf"
      "2b17347ebc1800181c4e2a1fb8dd53e1"
      "c635518c7dac47e9" },
    { "sha512 c=1", 1, "password", "salt", 1, 64,
      "867f70cf1ade02cff3752599a3a53dc4"
      "af34c7a669815ae5d513554e1c8cf252"
      "c02d470a285a0501bad999bfe943c08f"
      "050235d7d68b1da55e63f73b60a57fce" },
    { "sha512 c=4096", 1, "password", "salt", 4096, 64,
      "d197b1b33db0143e018b12f3d1d1479e"
      "6cdebdcc97c5c0f87f6902e072f457b5"
      "143f30602641b3d55cd335988cb36b84"
      "376060ecd532e039b742a239434af2d5" },
};

static void test_pbkdf2_vectors(void **state)
{
    uint8_t out[64];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof pbkdf2_vecs / sizeof pbkdf2_vecs[0]; i++) {
        const pbkdf2_vec *v = &pbkdf2_vecs[i];
        const hash_desc *d = v->sha512 ? hash_sha512() : hash_sha256();

        assert_true(v->dklen <= sizeof out);
        assert_int_equal(pbkdf2(d, v->pass, strlen(v->pass),
                                v->salt, strlen(v->salt), v->iters,
                                out, v->dklen), CRYPTO_OK);
        assert_hex(out, v->dklen, v->want);
    }
}

/* A short request must be the prefix of a long one: PBKDF2 truncates the last
 * block rather than deriving something different. falcon-core/src/luks/luks.c
 * asks for exactly the master key length, which is not a multiple of the digest
 * for every key size the format allows. */
static void test_pbkdf2_truncation_is_a_prefix(void **state)
{
    uint8_t full[64];
    uint8_t part[64];
    size_t n;

    (void)state;
    assert_int_equal(pbkdf2(hash_sha256(), "pw", 2, "salt", 4, 17,
                            full, sizeof full), CRYPTO_OK);
    for (n = 1; n < sizeof full; n++) {
        assert_int_equal(pbkdf2(hash_sha256(), "pw", 2, "salt", 4, 17,
                                part, n), CRYPTO_OK);
        assert_memory_equal(part, full, n);
    }
}

static void test_pbkdf2_bad_args(void **state)
{
    uint8_t out[32];

    (void)state;
    assert_int_equal(pbkdf2(NULL, "p", 1, "s", 1, 1, out, sizeof out),
                     CRYPTO_ERR_BAD_ARG);
    assert_int_equal(pbkdf2(hash_sha256(), "p", 1, "s", 1, 1, NULL,
                            sizeof out), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(pbkdf2(hash_sha256(), "p", 1, "s", 1, 1, out, 0),
                     CRYPTO_ERR_BAD_ARG);
    /* Zero iterations is not "do it once": RFC 8018 requires c >= 1, and
     * silently treating 0 as 1 would accept a header that means something
     * this implementation cannot reproduce. */
    assert_int_equal(pbkdf2(hash_sha256(), "p", 1, "s", 1, 0, out,
                            sizeof out), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(pbkdf2(hash_sha256(), NULL, 1, "s", 1, 1, out,
                            sizeof out), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(pbkdf2(hash_sha256(), "p", 1, NULL, 1, 1, out,
                            sizeof out), CRYPTO_ERR_BAD_ARG);

    /* An empty passphrase and an empty salt are both legal. */
    assert_int_equal(pbkdf2(hash_sha256(), NULL, 0, NULL, 0, 1, out,
                            sizeof out), CRYPTO_OK);
}

/* ----------------------------------------------------------------- AES */

static void test_aes_ecb_vectors(void **state)
{
    static const char pt_hex[] = "00112233445566778899aabbccddeeff";
    static const char k128[] = "000102030405060708090a0b0c0d0e0f";
    static const char c128[] = "69c4e0d86a7b0430d8cdb78070b4c55a";
    static const char k256[] = "000102030405060708090a0b0c0d0e0f"
                               "101112131415161718191a1b1c1d1e1f";
    static const char c256[] = "8ea2b7ca516745bfeafc49904b496089";
    uint8_t pt[AES_BLOCK], key[32], buf[AES_BLOCK];
    aes_key k;

    (void)state;
    (void)unhex(pt_hex, pt, sizeof pt);

    /* FIPS 197 Appendix C.1. */
    assert_int_equal(unhex(k128, key, sizeof key), 16);
    assert_int_equal(aes_setkey(&k, key, 16), CRYPTO_OK);
    assert_int_equal(k.nr, 10);
    aes_encrypt_block(&k, pt, buf);
    assert_hex(buf, sizeof buf, c128);
    aes_decrypt_block(&k, buf, buf);
    assert_memory_equal(buf, pt, sizeof pt);

    /* FIPS 197 Appendix C.3. */
    assert_int_equal(unhex(k256, key, sizeof key), 32);
    assert_int_equal(aes_setkey(&k, key, 32), CRYPTO_OK);
    assert_int_equal(k.nr, 14);
    aes_encrypt_block(&k, pt, buf);
    assert_hex(buf, sizeof buf, c256);
    aes_decrypt_block(&k, buf, buf);
    assert_memory_equal(buf, pt, sizeof pt);
}

static void test_aes_setkey_rejects(void **state)
{
    uint8_t key[32];
    aes_key k;

    (void)state;
    memset(key, 0, sizeof key);

    /* AES-192 is a real key size this implementation does not have, which is
     * why it is UNSUPPORTED and not BAD_ARG: the caller asked for something
     * coherent. */
    assert_int_equal(aes_setkey(&k, key, 24), CRYPTO_ERR_UNSUPPORTED);
    assert_int_equal(aes_setkey(&k, key, 0), CRYPTO_ERR_UNSUPPORTED);
    assert_int_equal(aes_setkey(&k, key, 31), CRYPTO_ERR_UNSUPPORTED);
    assert_int_equal(aes_setkey(NULL, key, 16), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(aes_setkey(&k, NULL, 16), CRYPTO_ERR_BAD_ARG);
}

/* ----------------------------------------------------------------- XTS */

/* IEEE 1619 vector 10 and an AES-128 companion.
 *
 * The 1619 document's own AES-128 vectors use data unit numbers far beyond
 * anything a 512-byte-sector image can address, so they cannot be reproduced
 * through dm-crypt. The AES-128 row here was produced the same way as vector
 * 10 (a plain aes-xts-plain64 mapping, written at the stated sector) and is
 * therefore still a cross-implementation check against the kernel, just not a
 * published one. */
typedef struct {
    const char *label;
    const char *key;      /* key1 || key2 */
    uint64_t    sector;
    const char *ct;
} xts_vec;

/* 512 bytes: 0x00..0xff twice. The plaintext both rows share. */
static void xts_plaintext(uint8_t *out)
{
    unsigned i;

    for (i = 0; i < 512u; i++) {
        out[i] = (uint8_t)(i & 0xffu);
    }
}

static const xts_vec xts_vecs[] = {
    { "IEEE 1619 vector 10 (AES-256, unit 0xff)",
      "2718281828459045235360287471352662497757247093699959574966967627"
      "3141592653589793238462643383279502884197169399375105820974944592",
      0xffu,
      "1c3b3a102f770386e4836c99e370cf9bea00803f5e482357a4ae12d414a3e63b"
      "5d31e276f8fe4a8d66b317f9ac683f44680a86ac35adfc3345befecb4bb188fd"
      "5776926c49a3095eb108fd1098baec70aaa66999a72a82f27d848b21d4a741b0"
      "c5cd4d5fff9dac89aeba122961d03a757123e9870f8acf1000020887891429ca"
      "2a3e7a7d7df7b10355165c8b9a6d0a7de8b062c4500dc4cd120c0f7418dae3d0"
      "b5781c34803fa75421c790dfe1de1834f280d7667b327f6c8cd7557e12ac3a0f"
      "93ec05c52e0493ef31a12d3d9260f79a289d6a379bc70c50841473d1a8cc81ec"
      "583e9645e07b8d9670655ba5bbcfecc6dc3966380ad8fecb17b6ba02469a020a"
      "84e18e8f84252070c13e9f1f289be54fbc481457778f616015e1327a02b140f1"
      "505eb309326d68378f8374595c849d84f4c333ec4423885143cb47bd71c5edae"
      "9be69a2ffeceb1bec9de244fbe15992b11b77c040f12bd8f6a975a44a0f90c29"
      "a9abc3d4d893927284c58754cce294529f8614dcd2aba991925fedc4ae74ffac"
      "6e333b93eb4aff0479da9a410e4450e0dd7ae4c6e2910900575da401fc07059f"
      "645e8b7e9bfdef33943054ff84011493c27b3429eaedb4ed5376441a77ed4385"
      "1ad77f16f541dfd269d50d6a5f14fb0aab1cbb4c1550be97f7ab4066193c4caa"
      "773dad38014bd2092fa755c824bb5e54c4f36ffda9fcea70b9c6e693e148c151" },
    { "AES-128, unit 0x0c (dm-crypt derived)",
      "fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0"
      "bfbebdbcbbbab9b8b7b6b5b4b3b2b1b0",
      0x0cu,
      "1127ffec8556c9e65cdf05fae5615ebd0d64dc00ad423e9749d7fe2e34efafdb"
      "11fe5023c7d2a916fe48b84c0fa9b0da2a3921157efb9d2d1b2dbe7a08f2fbf5"
      "279519d3941b4a3ff3bc94a7db1c2c6f3158edce5d9bb68bcf0fe678effc4694"
      "134451f8da5d58aae2c03c7525f480fc799c6998c1712398b085db168e3593cf"
      "dcf58d5402013604105de1666f05791d464b9a09900c5f91998ad7e07f641fd7"
      "48a39c4bffe6cfe651104e9666890b87e1038cd9a435fa21f22d4e5be2517c79"
      "8d926175ef7680d5894ca1caceb76c6dd3d1f27d3fd6fdd8c4d8baec36a8d826"
      "5274abdd5dd995d196d1f12ca83331b427ae3cd1ea40a85108d6983c103b7bb5"
      "a18bfa81575cad4de4609dcb12bd88be41f7fe7e94fd3f66bb5a418bfde6c77f"
      "607a534999d7229b25d6c9f1fec7de62a26538b89887b355f964a55a68810b73"
      "ef19eab788d1aa186d6a98c5628a9191071155f56a34d2456c349e8f1f18b786"
      "2db2d85beafe1d4b4758d7f4403d04c8e470e1e4e85cca4be6e54c3c718395c8"
      "6165c15c6293da6aed737375e1bc860ad56416d202f988b067b21b3b66fe36ad"
      "be7bef02852f71d5af1a5d0ee17759395b36a6dfcdc6898a4c8c4e93b52c7ef8"
      "2df46dce7311992de7d5f0f45717f8f1b895991ca870b5bbaf4933f23b9745a4"
      "af85a8969515195c9d7ae21852b9d06269d88f95b49f2c3a6ab7a06a216565c4" },
};

static void xts_run_vector(const xts_vec *v, int portable)
{
    uint8_t key[64], pt[512], ct[512], buf[512];
    xts_ctx c;
    size_t klen;

    klen = unhex(v->key, key, sizeof key);
    xts_plaintext(pt);
    (void)unhex(v->ct, ct, sizeof ct);

    if (portable) {
        assert_int_equal(xts_setkey_portable(&c, key, klen), CRYPTO_OK);
        assert_int_equal(xts_impl(&c), XTS_IMPL_PORTABLE);
    } else {
        assert_int_equal(xts_setkey(&c, key, klen), CRYPTO_OK);
    }

    xts_encrypt_sector(&c, v->sector, pt, buf, sizeof buf);
    assert_memory_equal(buf, ct, sizeof ct);

    /* Decrypt is the direction the loader actually runs. */
    xts_decrypt_sector(&c, v->sector, ct, buf, sizeof buf);
    assert_memory_equal(buf, pt, sizeof pt);

    /* In-place, which is how falcon-core/src/luks/luks.c calls it. */
    memcpy(buf, ct, sizeof ct);
    xts_decrypt_sector(&c, v->sector, buf, buf, sizeof buf);
    assert_memory_equal(buf, pt, sizeof pt);

    xts_wipe(&c);
}

static void test_xts_vectors_portable(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < sizeof xts_vecs / sizeof xts_vecs[0]; i++) {
        xts_run_vector(&xts_vecs[i], 1);
    }
}

static void test_xts_vectors_dispatched(void **state)
{
    size_t i;

    (void)state;
    /* Whatever xts_setkey picks must produce the same bytes. On this
     * machine that is the AES-NI arm; on one without it this repeats the
     * portable run, which costs a millisecond and keeps the test honest
     * everywhere. */
    for (i = 0; i < sizeof xts_vecs / sizeof xts_vecs[0]; i++) {
        xts_run_vector(&xts_vecs[i], 0);
    }
}

/* The two implementations must agree byte for byte on a 4096-byte data unit,
 * which is the size the LUKS2 fixture's segment actually uses and 256 blocks
 * of tweak doubling rather than 32. This is the only place the SSE2 gf_double
 * in aesni.c and the byte-wise one in xts.c are compared directly.
 *
 * Skipped rather than failed without AES-NI: there is no second
 * implementation to compare against, and the portable path is already pinned
 * by the vectors above. */
static void test_xts_impls_agree_4096(void **state)
{
    uint8_t key[64], pt[4096], a[4096], b[4096];
    xts_ctx cp, cn;
    unsigned i;

    (void)state;
    if (!crypto_has_aesni()) {
        print_message("  (no AES-NI on this CPU: comparison skipped)\n");
        skip();
    }

    for (i = 0; i < sizeof key; i++) {
        key[i] = (uint8_t)(i * 7u + 3u);
    }
    for (i = 0; i < sizeof pt; i++) {
        pt[i] = (uint8_t)(i * 31u + 11u);
    }

    assert_int_equal(xts_setkey_portable(&cp, key, sizeof key), CRYPTO_OK);
    assert_int_equal(xts_setkey(&cn, key, sizeof key), CRYPTO_OK);
    assert_int_equal(xts_impl(&cp), XTS_IMPL_PORTABLE);
    assert_int_equal(xts_impl(&cn), XTS_IMPL_AESNI);

    /* A sector number above 2^32 checks that the plain64 tweak really is
     * 64-bit in both arms; a 32-bit truncation is invisible on any image
     * small enough to test end to end. */
    xts_encrypt_sector(&cp, 0x1234567890abcdefull, pt, a, sizeof a);
    xts_encrypt_sector(&cn, 0x1234567890abcdefull, pt, b, sizeof b);
    assert_memory_equal(a, b, sizeof a);

    xts_decrypt_sector(&cp, 0x1234567890abcdefull, a, a, sizeof a);
    xts_decrypt_sector(&cn, 0x1234567890abcdefull, b, b, sizeof b);
    assert_memory_equal(a, pt, sizeof pt);
    assert_memory_equal(b, pt, sizeof pt);

    xts_wipe(&cp);
    xts_wipe(&cn);
}

/* 4096-byte round trip on both arms, independent of whether AES-NI exists:
 * decrypt(encrypt(x)) == x over 256 blocks, which is the tweak sequence the
 * 512-byte vectors never reach. */
static void test_xts_round_trip_4096(void **state)
{
    uint8_t key[64], pt[4096], buf[4096];
    xts_ctx c;
    unsigned pass;
    unsigned i;

    (void)state;
    for (i = 0; i < sizeof key; i++) {
        key[i] = (uint8_t)(0xa5u ^ i);
    }
    for (i = 0; i < sizeof pt; i++) {
        pt[i] = (uint8_t)((i >> 3) ^ (i * 5u));
    }

    for (pass = 0; pass < 2u; pass++) {
        int rc = (pass == 0u) ? xts_setkey_portable(&c, key, sizeof key)
                              : xts_setkey(&c, key, sizeof key);

        assert_int_equal(rc, CRYPTO_OK);
        xts_encrypt_sector(&c, 7, pt, buf, sizeof buf);
        /* The ciphertext must not be the plaintext, which is the one way a
         * no-op implementation would sail through a round-trip test. */
        assert_int_not_equal(memcmp(buf, pt, sizeof pt), 0);
        xts_decrypt_sector(&c, 7, buf, buf, sizeof buf);
        assert_memory_equal(buf, pt, sizeof pt);

        /* A different sector must give different ciphertext: the tweak is
         * the only thing separating identical plaintext at two offsets. */
        xts_encrypt_sector(&c, 8, pt, buf, sizeof buf);
        xts_decrypt_sector(&c, 7, buf, buf, sizeof buf);
        assert_int_not_equal(memcmp(buf, pt, sizeof pt), 0);

        xts_wipe(&c);
    }
}

static void test_xts_setkey_rejects(void **state)
{
    uint8_t key[64];
    xts_ctx c;

    (void)state;
    memset(key, 0, sizeof key);

    /* 48 bytes would be two AES-192 halves. */
    assert_int_equal(xts_setkey(&c, key, 48), CRYPTO_ERR_UNSUPPORTED);
    assert_int_equal(xts_setkey(&c, key, 16), CRYPTO_ERR_UNSUPPORTED);
    assert_int_equal(xts_setkey(&c, key, 0), CRYPTO_ERR_UNSUPPORTED);
    assert_int_equal(xts_setkey(NULL, key, 64), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(xts_setkey(&c, NULL, 64), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(xts_setkey_portable(&c, key, 48), CRYPTO_ERR_UNSUPPORTED);
    assert_int_equal(xts_setkey_portable(NULL, key, 64), CRYPTO_ERR_BAD_ARG);

    assert_int_equal(xts_setkey(&c, key, 32), CRYPTO_OK);
    xts_wipe(&c);
    assert_int_equal(xts_setkey(&c, key, 64), CRYPTO_OK);
    xts_wipe(&c);
}

/* xts_wipe must leave nothing: the context holds both AES schedules. */
static void test_xts_wipe(void **state)
{
    uint8_t key[64], zero[sizeof(xts_ctx)];
    xts_ctx c;
    unsigned i;

    (void)state;
    for (i = 0; i < sizeof key; i++) {
        key[i] = (uint8_t)(i + 1u);
    }
    memset(zero, 0, sizeof zero);

    assert_int_equal(xts_setkey(&c, key, sizeof key), CRYPTO_OK);
    assert_int_not_equal(memcmp(&c, zero, sizeof c), 0);
    xts_wipe(&c);
    assert_memory_equal(&c, zero, sizeof c);
}

/* -------------------------------------------------- wipe and compare */

static void test_crypto_ct_eq(void **state)
{
    uint8_t a[32], b[32];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof a; i++) {
        a[i] = (uint8_t)(i * 3u + 1u);
        b[i] = a[i];
    }

    assert_int_equal(crypto_ct_eq(a, b, sizeof a), 1);
    assert_int_equal(crypto_ct_eq(a, a, sizeof a), 1);
    /* A zero-length compare is vacuously equal, which is what the loop
     * yields and what a caller comparing two empty digests should get. */
    assert_int_equal(crypto_ct_eq(a, b, 0), 1);

    /* One flipped bit anywhere, including the last byte, which is where a
     * loop that stopped one short would miss it. */
    for (i = 0; i < sizeof a; i++) {
        b[i] = (uint8_t)(a[i] ^ 0x01u);
        assert_int_equal(crypto_ct_eq(a, b, sizeof a), 0);
        b[i] = (uint8_t)(a[i] ^ 0x80u);
        assert_int_equal(crypto_ct_eq(a, b, sizeof a), 0);
        b[i] = a[i];
    }

    /* A difference past the compared length must not count. */
    b[31] = (uint8_t)~a[31];
    assert_int_equal(crypto_ct_eq(a, b, 31), 1);
    assert_int_equal(crypto_ct_eq(a, b, 32), 0);

    /* NULL is never equal to anything, not even itself: the digest compare
     * must not read through a pointer the caller failed to fill in. */
    assert_int_equal(crypto_ct_eq(NULL, b, 4), 0);
    assert_int_equal(crypto_ct_eq(a, NULL, 4), 0);
    assert_int_equal(crypto_ct_eq(NULL, NULL, 0), 0);
}

/* crypto_wipe's whole reason to exist is that the compiler may not delete
 * it. Reading the buffer back through a volatile pointer, in a different
 * translation unit from the one that wiped it, is the strongest check
 * available without disassembling: gcc cannot prove the read is dead. */
static void test_crypto_wipe(void **state)
{
    static uint8_t buf[256];
    volatile const uint8_t *v = buf;
    size_t i;

    (void)state;
    for (i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)(i | 0x80u);
    }
    crypto_wipe(buf, sizeof buf);
    for (i = 0; i < sizeof buf; i++) {
        if (v[i] != 0u) {
            fail_msg("byte %zu survived the wipe: 0x%02x", i, v[i]);
        }
    }

    /* Partial wipes leave the tail alone. */
    for (i = 0; i < sizeof buf; i++) {
        buf[i] = 0xeeu;
    }
    crypto_wipe(buf, 10);
    for (i = 0; i < 10u; i++) {
        assert_int_equal(v[i], 0);
    }
    for (i = 10; i < sizeof buf; i++) {
        assert_int_equal(v[i], 0xee);
    }

    /* Both no-op forms must not fault. */
    crypto_wipe(NULL, 16);
    crypto_wipe(buf, 0);
    assert_int_equal(v[10], 0xee);
}

/* ------------------------------------------------------------------ run */

int run_crypto_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_sha1_vectors),
        cmocka_unit_test(test_sha256_vectors),
        cmocka_unit_test(test_sha512_vectors),
        cmocka_unit_test(test_sha_incremental_split),
        cmocka_unit_test(test_sha_final_wipes_context),
        cmocka_unit_test(test_hash_by_name),
        cmocka_unit_test(test_hmac_vectors),
        cmocka_unit_test(test_hmac_key_reuse),
        cmocka_unit_test(test_hmac_bad_args),
        cmocka_unit_test(test_pbkdf2_vectors),
        cmocka_unit_test(test_pbkdf2_truncation_is_a_prefix),
        cmocka_unit_test(test_pbkdf2_bad_args),
        cmocka_unit_test(test_aes_ecb_vectors),
        cmocka_unit_test(test_aes_setkey_rejects),
        cmocka_unit_test(test_xts_vectors_portable),
        cmocka_unit_test(test_xts_vectors_dispatched),
        cmocka_unit_test(test_xts_impls_agree_4096),
        cmocka_unit_test(test_xts_round_trip_4096),
        cmocka_unit_test(test_xts_setkey_rejects),
        cmocka_unit_test(test_xts_wipe),
        cmocka_unit_test(test_crypto_ct_eq),
        cmocka_unit_test(test_crypto_wipe),
    };

    return cmocka_run_group_tests_name("crypto", tests, NULL, NULL);
}
