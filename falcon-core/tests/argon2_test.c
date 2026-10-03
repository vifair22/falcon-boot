/* Vector tests for falcon-core/src/crypto/blake2b.c and argon2.c.
 *
 * Same rule as crypto_test.c: every expected value comes from an
 * independent implementation, never from this code.
 *
 *   BLAKE2b   RFC 7693 Appendix A ("abc", 64-byte digest); every other row
 *             from python3's hashlib.blake2b (OpenSSL's/CPython's own
 *             BLAKE2), with the call written beside it. The keyed rows use
 *             message lengths either side of the 128-byte block, which is
 *             where holding back the final block goes wrong.
 *   Argon2id  RFC 9106 Section 5.3, re-derived on this machine through
 *             libargon2's argon2_ctx (the reference implementation, which
 *             the CLI cannot drive because it takes no secret or associated
 *             data); every other row from the reference CLI /usr/bin/argon2,
 *             with the exact command beside it. The multi-lane rows are the
 *             ones that prove filling lanes one after another gives the
 *             parallel result; the m=100, p=3 row exercises m' < m.
 *   Runners   the same reference CLI rows again through argon2_runner:
 *             lanes in reverse, and one thread a lane (pthreads), each of
 *             which must give the reference output, with every lane of
 *             every slice run exactly once.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include <crypto/crypto.h>
#include <crypto/blake2b.h>
#include <crypto/argon2.h>

static uint8_t nib(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    assert_true(c >= 'a' && c <= 'f');
    return (uint8_t)(c - 'a' + 10);
}

static void assert_hex(const uint8_t *got, size_t got_len, const char *want)
{
    size_t n = strlen(want);
    size_t i;

    assert_int_equal(n, 2u * got_len);
    for (i = 0; i < got_len; i++) {
        uint8_t w = (uint8_t)((nib(want[2u * i]) << 4) | nib(want[2u * i + 1u]));

        if (got[i] != w) {
            fail_msg("byte %zu: got 0x%02x, want 0x%02x", i, got[i], w);
        }
    }
}

static double ms_since(const struct timespec *t0)
{
    struct timespec t1;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e3 +
           (double)(t1.tv_nsec - t0->tv_nsec) / 1e6;
}

/* ------------------------------------------------------------- BLAKE2b */

static void test_blake2b_rfc7693(void **state)
{
    uint8_t out[64];

    (void)state;
    /* RFC 7693 Appendix A. */
    assert_int_equal(blake2b(out, 64, NULL, 0, "abc", 3), CRYPTO_OK);
    assert_hex(out, 64,
        "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
        "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");
    /* hashlib.blake2b(b"") */
    assert_int_equal(blake2b(out, 64, NULL, 0, "", 0), CRYPTO_OK);
    assert_hex(out, 64,
        "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419"
        "d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce");
    /* hashlib.blake2b(b"abc", digest_size=1) */
    assert_int_equal(blake2b(out, 1, NULL, 0, "abc", 3), CRYPTO_OK);
    assert_hex(out, 1, "6b");
    /* hashlib.blake2b(bytes(200), digest_size=20) */
    {
        uint8_t zero[200];

        memset(zero, 0, sizeof zero);
        assert_int_equal(blake2b(out, 20, NULL, 0, zero, sizeof zero),
                         CRYPTO_OK);
        assert_hex(out, 20, "0e77cb4b5d4e32f2597090fe2c88ab496249615c");
    }
}

/* hashlib.blake2b(bytes(i % 256 for i in range(n)), key=bytes(range(64))) */
static void test_blake2b_keyed(void **state)
{
    static const struct {
        size_t      n;
        const char *want;
    } rows[] = {
        { 0,   "10ebb67700b1868efb4417987acf4690ae9d972fb7a590c2f02871799aaa4786"
               "b5e996e8f0f4eb981fc214b005f42d2ff4233499391653df7aefcbc13fc51568" },
        { 1,   "961f6dd1e4dd30f63901690c512e78e4b45e4742ed197c3c5e45c549fd25f2e4"
               "187b0bc9fe30492b16b0d0bc4ef9b0f34c7003fac09a5ef1532e69430234cebd" },
        { 127, "76d2d819c92bce55fa8e092ab1bf9b9eab237a25267986cacf2b8ee14d214d73"
               "0dc9a5aa2d7b596e86a1fd8fa0804c77402d2fcd45083688b218b1cdfa0dcbcb" },
        { 128, "72065ee4dd91c2d8509fa1fc28a37c7fc9fa7d5b3f8ad3d0d7a25626b57b1b44"
               "788d4caf806290425f9890a3a2a35a905ab4b37acfd0da6e4517b2525c9651e4" },
        { 129, "64475dfe7600d7171bea0b394e27c9b00d8e74dd1e416a79473682ad3dfdbb70"
               "6631558055cfc8a40e07bd015a4540dcdea15883cbbf31412df1de1cd4152b91" },
        { 255, "142709d62e28fcccd0af97fad0f8465b971e82201dc51070faa0372aa43e9248"
               "4be1c1e73ba10906d5d1853db6a4106e0a7bf9800d373d6dee2d46d62ef2a461" },
        { 256, "b72071e096277edebb8ee5134dd3714996307ba3a55aa4733d412abbe28e909e"
               "10e57e6fbfb4ef53b3b960518294ff889a90829254412e2a60b85add07a3674f" },
    };
    uint8_t key[64];
    uint8_t msg[256];
    uint8_t out[64];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof key; i++) {
        key[i] = (uint8_t)i;
    }
    for (i = 0; i < sizeof msg; i++) {
        msg[i] = (uint8_t)i;
    }
    for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        blake2b_ctx c;
        size_t off = 0;

        assert_int_equal(blake2b(out, 64, key, 64, msg, rows[i].n), CRYPTO_OK);
        assert_hex(out, 64, rows[i].want);

        /* Incrementally, in awkward chunks: the buffering must not care. */
        assert_int_equal(blake2b_init(&c, 64, key, 64), CRYPTO_OK);
        while (off < rows[i].n) {
            size_t take = rows[i].n - off;

            if (take > 7u) {
                take = 7u;
            }
            blake2b_update(&c, msg + off, take);
            off += take;
        }
        blake2b_final(&c, out);
        assert_hex(out, 64, rows[i].want);
    }

    /* hashlib.blake2b(b"The quick brown fox", key=b"0123456789abcdef",
     *                 digest_size=32) */
    assert_int_equal(blake2b(out, 32, "0123456789abcdef", 16,
                             "The quick brown fox", 19), CRYPTO_OK);
    assert_hex(out, 32,
        "ac7961ac0f6fb02a394215e21ba1f1c7e32923d0f72d15b9e92bc97ccdda6611");
}

static void test_blake2b_bad_args(void **state)
{
    blake2b_ctx c;
    uint8_t key[65];
    uint8_t out[64];

    (void)state;
    memset(key, 0, sizeof key);
    assert_int_equal(blake2b_init(NULL, 64, NULL, 0), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(blake2b_init(&c, 0, NULL, 0), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(blake2b_init(&c, 65, NULL, 0), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(blake2b_init(&c, 64, key, 65), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(blake2b_init(&c, 64, NULL, 1), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(blake2b(out, 0, NULL, 0, "", 0), CRYPTO_ERR_BAD_ARG);
}

/* ------------------------------------------------------------- Argon2id */

/* RFC 9106 Section 5.3, confirmed with libargon2's argon2_ctx(Argon2_id). */
static void test_argon2id_rfc9106(void **state)
{
    uint8_t pwd[32], salt[16], secret[8], ad[12], out[32];
    argon2_params p;
    struct timespec t0;
    void *mem;

    (void)state;
    memset(pwd, 0x01, sizeof pwd);
    memset(salt, 0x02, sizeof salt);
    memset(secret, 0x03, sizeof secret);
    memset(ad, 0x04, sizeof ad);
    memset(&p, 0, sizeof p);
    p.pwd = pwd;
    p.pwd_len = sizeof pwd;
    p.salt = salt;
    p.salt_len = sizeof salt;
    p.secret = secret;
    p.secret_len = sizeof secret;
    p.ad = ad;
    p.ad_len = sizeof ad;
    p.t_cost = 3;
    p.m_cost = 32;
    p.lanes = 4;

    assert_int_equal(argon2_mem_kib(32, 4), 32);
    mem = malloc(32u * 1024u);
    assert_non_null(mem);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    assert_int_equal(argon2id(&p, mem, 32, NULL, out, sizeof out),
                     CRYPTO_OK);
    printf("[ INFO     ] argon2id RFC 9106 vector: %.3f ms\n", ms_since(&t0));
    assert_hex(out, sizeof out,
        "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659");
    free(mem);
}

/* `echo -n password | argon2 somesalt -id <args> -r`, argon2 20190702. */
static void test_argon2id_reference_cli(void **state)
{
    static const struct {
        uint32_t    t, m, p, l;
        const char *want;
    } rows[] = {
        /* -t 2 -k 64 -p 1 -l 32 */
        { 2, 64, 1, 32,
          "16a1a498734609dd01456da406de9f3d9da93e6c86c300a12fc1465214ce4922" },
        /* -t 3 -k 1024 -p 4 -l 32 */
        { 3, 1024, 4, 32,
          "e3c3ff0547f31c037b332f7539fe30f6be8aabc4cf765d10eb34847a8f3fa43c" },
        /* -t 1 -k 8 -p 1 -l 4 */
        { 1, 8, 1, 4, "6b7a947d" },
        /* -t 4 -k 256 -p 2 -l 64 */
        { 4, 256, 2, 64,
          "48e689ee725c5bd939a9f45c246b8f4e65f2fb7fdf3b4381f8b41843e72a96e7"
          "59e8f310e80a0b35e1a0daa5c4f515316dad8bd5849b9599caf1a7c5a80d64d8" },
        /* -t 2 -k 100 -p 3 -l 100 */
        { 2, 100, 3, 100,
          "d98ff35be4929d949cd4de3dcf10482d8a006a5809bae1c331feae87936a1f4e"
          "a0434327ff12c78557fc368fc6c7ac185f8ae722254afc7754cd6c1bf2517030"
          "1463e34fd1495bd02cf900c3df6aecfda17b00658ff0b59f8726eac34a52d1b6"
          "2d96ab74" },
        /* -t 1 -k 4096 -p 1 -l 32 */
        { 1, 4096, 1, 32,
          "c885b78ca85ce3a260677140728184a30addeafc8ffd64d2cbfc144440269707" },
    };
    uint8_t out[100];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        argon2_params p;
        uint32_t kib = argon2_mem_kib(rows[i].m, rows[i].p);
        void *mem;

        memset(&p, 0, sizeof p);
        p.pwd = "password";
        p.pwd_len = 8;
        p.salt = "somesalt";
        p.salt_len = 8;
        p.t_cost = rows[i].t;
        p.m_cost = rows[i].m;
        p.lanes = rows[i].p;
        assert_true(kib != 0u && kib <= rows[i].m);
        mem = malloc((size_t)kib * 1024u);
        assert_non_null(mem);
        assert_int_equal(argon2id(&p, mem, kib, NULL, out, rows[i].l),
                         CRYPTO_OK);
        assert_hex(out, rows[i].l, rows[i].want);
        free(mem);
    }
}

/* m' is 96 for m=100, p=3: the caller need not supply the 4 KiB the
 * rounding drops, may supply more, and the block comes back wiped. */
static void test_argon2id_memory(void **state)
{
    argon2_params p;
    uint8_t out[32];
    uint8_t *mem;
    size_t i;

    (void)state;
    assert_int_equal(argon2_mem_kib(100, 3), 96);
    assert_int_equal(argon2_mem_kib(65536, 4), 65536);
    assert_int_equal(argon2_mem_kib(7, 1), 0);
    assert_int_equal(argon2_mem_kib(15, 2), 0);
    assert_int_equal(argon2_mem_kib(64, 0), 0);
    assert_int_equal(argon2_mem_kib(0xffffffffu, ARGON2_MAX_LANES + 1u), 0);

    memset(&p, 0, sizeof p);
    p.pwd = "password";
    p.pwd_len = 8;
    p.salt = "somesalt";
    p.salt_len = 8;
    p.t_cost = 2;
    p.m_cost = 100;
    p.lanes = 3;

    mem = malloc(200u * 1024u);
    assert_non_null(mem);
    memset(mem, 0xa5, 200u * 1024u);
    assert_int_equal(argon2id(&p, mem, 95, NULL, out, sizeof out),
                     CRYPTO_ERR_NOSPACE);
    /* Refused before a byte was written. */
    for (i = 0; i < 200u * 1024u; i++) {
        assert_int_equal(mem[i], 0xa5);
    }
    assert_int_equal(argon2id(&p, mem, 200, NULL, out, sizeof out), CRYPTO_OK);
    for (i = 0; i < 96u * 1024u; i++) {
        if (mem[i] != 0u) {
            fail_msg("byte %zu of the block was not wiped", i);
        }
    }
    /* Past m' is not the module's to touch. */
    for (i = 96u * 1024u; i < 200u * 1024u; i++) {
        assert_int_equal(mem[i], 0xa5);
    }
    free(mem);
}

static void test_argon2id_bad_args(void **state)
{
    argon2_params p, q;
    uint8_t out[32];
    uint64_t mem[8 * 128 + 1];

    (void)state;
    memset(&p, 0, sizeof p);
    p.pwd = "password";
    p.pwd_len = 8;
    p.salt = "somesalt";
    p.salt_len = 8;
    p.t_cost = 1;
    p.m_cost = 8;
    p.lanes = 1;
    assert_int_equal(argon2id(&p, mem, 8, NULL, out, sizeof out), CRYPTO_OK);

    assert_int_equal(argon2id(NULL, mem, 8, NULL, out, 32), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(argon2id(&p, NULL, 8, NULL, out, 32), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(argon2id(&p, mem, 8, NULL, NULL, 32), CRYPTO_ERR_BAD_ARG);
    assert_int_equal(argon2id(&p, mem, 8, NULL, out, 3), CRYPTO_ERR_BAD_ARG);
    /* Misaligned. */
    assert_int_equal(argon2id(&p, (uint8_t *)mem + 4, 8, NULL, out, 32),
                     CRYPTO_ERR_BAD_ARG);

    q = p; q.t_cost = 0;
    assert_int_equal(argon2id(&q, mem, 8, NULL, out, 32), CRYPTO_ERR_BAD_ARG);
    q = p; q.salt_len = 7;
    assert_int_equal(argon2id(&q, mem, 8, NULL, out, 32), CRYPTO_ERR_BAD_ARG);
    q = p; q.salt = NULL;
    assert_int_equal(argon2id(&q, mem, 8, NULL, out, 32), CRYPTO_ERR_BAD_ARG);
    q = p; q.pwd = NULL;
    assert_int_equal(argon2id(&q, mem, 8, NULL, out, 32), CRYPTO_ERR_BAD_ARG);
    q = p; q.secret_len = 1;
    assert_int_equal(argon2id(&q, mem, 8, NULL, out, 32), CRYPTO_ERR_BAD_ARG);
    q = p; q.ad_len = 1;
    assert_int_equal(argon2id(&q, mem, 8, NULL, out, 32), CRYPTO_ERR_BAD_ARG);
    q = p; q.m_cost = 7;
    assert_int_equal(argon2id(&q, mem, 8, NULL, out, 32), CRYPTO_ERR_BAD_ARG);
    q = p; q.lanes = 0;
    assert_int_equal(argon2id(&q, mem, 8, NULL, out, 32), CRYPTO_ERR_BAD_ARG);
    q = p; q.lanes = 2;
    assert_int_equal(argon2id(&q, mem, 8, NULL, out, 32), CRYPTO_ERR_BAD_ARG);
    /* An empty password is legal. */
    q = p; q.pwd = NULL; q.pwd_len = 0;
    assert_int_equal(argon2id(&q, mem, 8, NULL, out, 32), CRYPTO_OK);
}

/* ---- runners ---------------------------------------------------------- */

#define RUN_MAX_LANES 8u

/* What a runner saw: the calls, and per call how often each lane ran. */
typedef struct {
    uint32_t slices;
    uint32_t bad;        /* calls where some lane did not run exactly once */
} run_log;

static void log_slice(run_log *g, const uint32_t *hits, uint32_t lanes)
{
    uint32_t l;

    g->slices++;
    for (l = 0; l < lanes; l++) {
        if (hits[l] != 1u) {
            g->bad++;
            return;
        }
    }
}

typedef struct {
    argon2_job job;
    void      *ctx;
    uint32_t   lane;
    uint32_t  *hit;
} lane_arg;

static void run_lane(lane_arg *a)
{
    a->job(a->ctx, a->lane);
    (*a->hit)++;
}

static void run_reverse(void *self, argon2_job job, void *ctx, uint32_t lanes)
{
    uint32_t hits[RUN_MAX_LANES] = { 0 };
    uint32_t l;

    assert_true(lanes <= RUN_MAX_LANES);
    for (l = lanes; l-- > 0u;) {
        lane_arg a = { job, ctx, l, &hits[l] };

        run_lane(&a);
    }
    log_slice((run_log *)self, hits, lanes);
}

static void *lane_thread(void *arg)
{
    run_lane((lane_arg *)arg);
    return NULL;
}

/* One thread a lane, created and joined per slice: pthread_join is the
 * barrier, and the ordering the runner contract asks for. */
static void run_threads(void *self, argon2_job job, void *ctx, uint32_t lanes)
{
    uint32_t hits[RUN_MAX_LANES] = { 0 };
    pthread_t th[RUN_MAX_LANES];
    lane_arg a[RUN_MAX_LANES];
    uint32_t l;

    assert_true(lanes <= RUN_MAX_LANES);
    for (l = 0; l < lanes; l++) {
        a[l].job = job;
        a[l].ctx = ctx;
        a[l].lane = l;
        a[l].hit = &hits[l];
        assert_int_equal(pthread_create(&th[l], NULL, lane_thread, &a[l]), 0);
    }
    for (l = 0; l < lanes; l++) {
        assert_int_equal(pthread_join(th[l], NULL), 0);
    }
    log_slice((run_log *)self, hits, lanes);
}

/* `echo -n password | argon2 somesalt -id <args> -r`, argon2 20190702,
 * through a runner. */
static void check_runner(void (*fn)(void *, argon2_job, void *, uint32_t))
{
    static const struct {
        uint32_t    t, m, p;
        const char *want;
    } rows[] = {
        /* -t 3 -k 1024 -p 4 -l 32 */
        { 3, 1024, 4,
          "e3c3ff0547f31c037b332f7539fe30f6be8aabc4cf765d10eb34847a8f3fa43c" },
        /* -t 2 -k 100 -p 3 -l 32 */
        { 2, 100, 3,
          "8b443eb7df2d72e5e2a9f49d609efce929dbc2db2a153d2f76fea016b97d856d" },
        /* -t 2 -k 64 -p 1 -l 32 */
        { 2, 64, 1,
          "16a1a498734609dd01456da406de9f3d9da93e6c86c300a12fc1465214ce4922" },
    };
    uint8_t out[32];
    size_t i;

    for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        run_log g = { 0, 0 };
        argon2_runner r = { fn, &g };
        argon2_params p;
        uint32_t kib = argon2_mem_kib(rows[i].m, rows[i].p);
        void *mem = malloc((size_t)kib * 1024u);

        assert_non_null(mem);
        memset(&p, 0, sizeof p);
        p.pwd = "password";
        p.pwd_len = 8;
        p.salt = "somesalt";
        p.salt_len = 8;
        p.t_cost = rows[i].t;
        p.m_cost = rows[i].m;
        p.lanes = rows[i].p;
        assert_int_equal(argon2id(&p, mem, kib, &r, out, sizeof out),
                         CRYPTO_OK);
        assert_hex(out, sizeof out, rows[i].want);
        /* Four synchronisation points a pass, every one a runner call. */
        assert_int_equal(g.slices, 4u * rows[i].t);
        assert_int_equal(g.bad, 0);
        free(mem);
    }
}

static void test_argon2id_runner_reverse(void **state)
{
    (void)state;
    check_runner(run_reverse);
}

static void test_argon2id_runner_threads(void **state)
{
    (void)state;
    check_runner(run_threads);
}

/* A runner with no function is refused before the block is touched. */
static void test_argon2id_runner_bad(void **state)
{
    argon2_runner r = { NULL, NULL };
    argon2_params p;
    uint8_t out[32];
    uint64_t mem[8 * 128];

    (void)state;
    memset(&p, 0, sizeof p);
    p.pwd = "password";
    p.pwd_len = 8;
    p.salt = "somesalt";
    p.salt_len = 8;
    p.t_cost = 1;
    p.m_cost = 8;
    p.lanes = 1;
    assert_int_equal(argon2id(&p, mem, 8, &r, out, sizeof out),
                     CRYPTO_ERR_BAD_ARG);
}

int run_argon2_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_blake2b_rfc7693),
        cmocka_unit_test(test_blake2b_keyed),
        cmocka_unit_test(test_blake2b_bad_args),
        cmocka_unit_test(test_argon2id_rfc9106),
        cmocka_unit_test(test_argon2id_reference_cli),
        cmocka_unit_test(test_argon2id_memory),
        cmocka_unit_test(test_argon2id_bad_args),
        cmocka_unit_test(test_argon2id_runner_reverse),
        cmocka_unit_test(test_argon2id_runner_threads),
        cmocka_unit_test(test_argon2id_runner_bad),
    };

    return cmocka_run_group_tests_name("argon2", tests, NULL, NULL);
}
