/* Tests for falcon-core/src/tpm.
 *
 * The replay. tpm_transcripts.h holds every exchange falcon-tpm-rig had with
 * swtpm (make tpm-transcripts), in both TPM versions, with the rig's
 * deterministic randomness. The flows below repeat the rig's flows call for
 * call with the same randomness, and the replay transport checks that each
 * command is byte for byte the one swtpm accepted before handing back the
 * response swtpm gave. So a change to the marshalling, the session maths or
 * the order of commands fails here without a TPM, and the responses the
 * parser and the HMAC checks meet are real ones. When the rig's flow changes,
 * both the transcripts and these flows change with it.
 *
 * Tampering. The same replays with one response byte changed: a flipped
 * HMAC byte and a flipped ciphertext byte in the seed's response must both
 * fail as TPM_ERR_AUTH, on either version, and nothing may reach the
 * caller's buffer. A short response is TPM_ERR_PROTO, a failed transport
 * TPM_ERR_IO, failed randomness TPM_ERR_RNG.
 *
 * Offline. The policy digest and the 1.2 composite against values computed
 * elsewhere (tpm2_createpolicy against swtpm, and Python's hashlib), and the
 * marshalling buffers' sticky overrun.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include <crypto/sha1.h>
#include <crypto/sha256.h>
#include <tpm/tpm.h>
#include <tpm/tpm12.h>
#include <tpm/tpm2.h>
#include <tpm/tpmbuf.h>

#include "tpm_transcripts.h"

#define PCRS ((1u << 0) | (1u << 2) | (1u << 4) | (1u << 7))

static const uint8_t SEED[20] = {
    'f', 'a', 'l', 'c', 'o', 'n', ' ', 't', 'o', 't',
    'p', ' ', 's', 'e', 'e', 'd', ' ', 'r', 'i', 'g'
};

/* ---- the replay transport ---- */

typedef struct {
    const xchg *x;
    size_t      n;
    size_t      i;
    int         mismatch;       /* a command that was not the recorded one */
    /* Faults, each at one exchange index (-1 for none). */
    long        corrupt_at;     /* flip a response byte */
    long        corrupt_off;    /* from the end when negative */
    long        truncate_at;    /* drop the response's last byte */
    long        fail_at;        /* the transport fails */
} replay;

static int replay_xfer(void *ctx, const uint8_t *cmd, size_t cmd_len,
                       uint8_t *rsp, size_t rsp_cap, size_t *rsp_len)
{
    replay *r = ctx;
    const xchg *e;
    size_t n;

    if (r->i >= r->n) {
        r->mismatch++;
        return -1;
    }
    e = &r->x[r->i];
    if ((long)r->i == r->fail_at) {
        r->i++;
        return -1;
    }
    if (cmd_len != e->cmd_len || memcmp(cmd, e->cmd, cmd_len) != 0) {
        r->mismatch++;
        return -1;
    }
    n = e->rsp_len;
    assert_true(n <= rsp_cap);
    memcpy(rsp, e->rsp, n);
    if ((long)r->i == r->corrupt_at) {
        long off = r->corrupt_off < 0 ? (long)n + r->corrupt_off
                                      : r->corrupt_off;

        rsp[off] ^= 0x01u;
    }
    if ((long)r->i == r->truncate_at) {
        n--;
    }
    *rsp_len = n;
    r->i++;
    return 0;
}

/* The rig's recording randomness (falcon-core/tools/tpmrig.c, det_rng). */
typedef struct {
    uint32_t ctr;
    uint8_t  block[32];
    size_t   left;
    int      fail;
} det;

static int det_rng(void *ctx, uint8_t *out, size_t n)
{
    det *g = ctx;

    if (g->fail) {
        return -1;
    }
    while (n > 0) {
        size_t k;

        if (g->left == 0) {
            uint8_t c[4];

            c[0] = (uint8_t)(g->ctr >> 24);
            c[1] = (uint8_t)(g->ctr >> 16);
            c[2] = (uint8_t)(g->ctr >> 8);
            c[3] = (uint8_t)g->ctr;
            g->ctr++;
            sha256(c, 4, g->block);
            g->left = 32;
        }
        k = n < g->left ? n : g->left;
        memcpy(out, g->block + 32 - g->left, k);
        g->left -= k;
        out += k;
        n -= k;
    }
    return 0;
}

static tpm_dev dev;
static replay rp;
static det rng;

static tpm_dev *setup(const xchg *x, size_t n)
{
    memset(&dev, 0, sizeof dev);
    memset(&rp, 0, sizeof rp);
    memset(&rng, 0, sizeof rng);
    rp.x = x;
    rp.n = n;
    rp.corrupt_at = -1;
    rp.truncate_at = -1;
    rp.fail_at = -1;
    dev.xfer = replay_xfer;
    dev.xfer_ctx = &rp;
    dev.rng = det_rng;
    dev.rng_ctx = &rng;
    return &dev;
}

/* The exchange index of the k-th command with code cc (0-based). */
static long find_cc(const xchg *x, size_t n, uint32_t cc, int k)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (tpm_be32(x[i].cmd + 6) == cc && k-- == 0) {
            return (long)i;
        }
    }
    fail();
    return -1;
}

/* ---- TPM 2.0 ---- */

/* The rig's flow_tpm2 up to and including the first seed read, which is
 * where the tamper tests stop. Returns that read's status. */
static int t2_to_first_read(tpm_dev *d, uint8_t name[TPM2_NAME_LEN],
                            uint8_t out[20])
{
    tpm_version v;

    assert_int_equal(tpm2_startup(d), TPM_OK);
    assert_int_equal(tpm_detect(d, &v), TPM_OK);
    assert_int_equal(v, TPM_VER_20);
    assert_int_equal(tpm2_nv_undefine(d, NULL, 0, TPM2_TOTP_INDEX),
                     TPM_ERR_RC);
    assert_int_equal(tpm2_seed_enroll(d, NULL, 0, TPM2_TOTP_INDEX, PCRS, SEED,
                                      sizeof SEED, name),
                     TPM_OK);
    memset(out, 0, 20);
    return tpm2_seed_read(d, TPM2_TOTP_INDEX, PCRS, name, out, 20);
}

static void test_tpm2_replay(void **state)
{
    tpm_dev *d = setup(t2_xchg, sizeof t2_xchg / sizeof t2_xchg[0]);
    uint8_t name[TPM2_NAME_LEN], bad[TPM2_NAME_LEN], out[20], ext[32];

    (void)state;
    assert_int_equal(t2_to_first_read(d, name, out), TPM_OK);
    assert_memory_equal(out, SEED, sizeof SEED);

    memcpy(bad, name, sizeof bad);
    bad[5] ^= 1u;
    assert_int_equal(tpm2_seed_read(d, TPM2_TOTP_INDEX, PCRS, bad, out,
                                    sizeof out),
                     TPM_ERR_KEY);

    assert_int_equal(tpm2_nv_write(d, NULL, 0, TPM2_TOTP_INDEX, SEED,
                                   sizeof SEED, NULL),
                     TPM_OK);
    memset(ext, 0x5a, sizeof ext);
    assert_int_equal(tpm2_pcr_extend(d, 7, ext), TPM_OK);
    assert_int_equal(tpm2_seed_read(d, TPM2_TOTP_INDEX, PCRS, name, out,
                                    sizeof out),
                     TPM_ERR_PCR);
    /* The policy failure's own code survives the cleanup flush. */
    assert_int_equal(dev.rc & 0xBFu, 0x9Du);
    assert_int_equal(tpm2_nv_undefine(d, NULL, 0, TPM2_TOTP_INDEX), TPM_OK);
    assert_int_equal(tpm2_nv_name(d, TPM2_TOTP_INDEX, name, NULL), TPM_ERR_RC);

    assert_int_equal(rp.mismatch, 0);
    assert_int_equal(rp.i, rp.n);
}

/* The seed's response, tampered: its HMAC (the last byte) or its ciphertext
 * (the first data byte, after tag, size, code, parameterSize and the TPM2B's
 * size). Either way the HMAC check refuses it and the caller's buffer is
 * left alone. */
static void t2_tamper(long off)
{
    tpm_dev *d = setup(t2_xchg, sizeof t2_xchg / sizeof t2_xchg[0]);
    uint8_t name[TPM2_NAME_LEN], out[20];
    static const uint8_t zero[20] = { 0 };

    rp.corrupt_at = find_cc(t2_xchg, rp.n, 0x14Eu, 0);
    rp.corrupt_off = off;
    assert_int_equal(t2_to_first_read(d, name, out), TPM_ERR_AUTH);
    assert_memory_equal(out, zero, sizeof zero);
}

static void test_tpm2_tamper_hmac(void **state)
{
    (void)state;
    t2_tamper(-1);
}

static void test_tpm2_tamper_ciphertext(void **state)
{
    (void)state;
    t2_tamper(10 + 4 + 2);
}

static void test_tpm2_faults(void **state)
{
    tpm_dev *d;
    uint8_t name[TPM2_NAME_LEN], out[20];

    (void)state;
    /* A short response is refused by the header check. */
    d = setup(t2_xchg, sizeof t2_xchg / sizeof t2_xchg[0]);
    rp.truncate_at = find_cc(t2_xchg, rp.n, 0x14Eu, 0);
    assert_int_equal(t2_to_first_read(d, name, out), TPM_ERR_PROTO);

    /* The transport fails at the session's start. */
    d = setup(t2_xchg, sizeof t2_xchg / sizeof t2_xchg[0]);
    rp.fail_at = find_cc(t2_xchg, rp.n, 0x176u, 1);
    assert_int_equal(t2_to_first_read(d, name, out), TPM_ERR_IO);

    /* No randomness: the enrollment's session cannot start. */
    d = setup(t2_xchg, sizeof t2_xchg / sizeof t2_xchg[0]);
    assert_int_equal(tpm2_startup(d), TPM_OK);
    rng.fail = 1;
    assert_int_equal(tpm2_saltkey_open(d, NULL, 0, NULL), TPM_ERR_ARG);
    {
        tpm2_saltkey k;
        tpm2_session s;

        memset(&k, 0, sizeof k);
        assert_int_equal(tpm2_session_start(d, &k, 1, &s), TPM_ERR_RNG);
    }
}

static void test_tpm2_args(void **state)
{
    tpm_dev *d = setup(t2_xchg, 0);
    uint8_t buf[TPM2_NAME_LEN], dig[32];
    static const uint8_t vals[1][32] = { { 0 } };

    (void)state;
    assert_int_equal(tpm2_pcr_read(d, 24, dig), TPM_ERR_ARG);
    assert_int_equal(tpm2_pcr_extend(d, 24, dig), TPM_ERR_ARG);
    assert_int_equal(tpm2_policy_pcr_digest(0, vals, dig), TPM_ERR_ARG);
    assert_int_equal(tpm2_policy_pcr_digest(1u << 24, vals, dig), TPM_ERR_ARG);
    assert_int_equal(tpm2_nv_define(d, NULL, 0, 1, 0, dig), TPM_ERR_ARG);
    assert_int_equal(tpm2_nv_define(d, NULL, 0, 1, TPM2_NV_MAX + 1u, dig),
                     TPM_ERR_ARG);
    assert_int_equal(tpm2_nv_write(d, NULL, 0, 1, buf, 0, NULL), TPM_ERR_ARG);
    assert_int_equal(tpm2_nv_read_policy(d, NULL, 1, buf, buf, 1),
                     TPM_ERR_ARG);
    assert_int_equal(tpm2_seed_read(d, 1, 0, NULL, buf, 1), TPM_ERR_ARG);
    assert_int_equal(tpm2_seed_enroll(d, NULL, 0, 1, PCRS, buf, 0, NULL),
                     TPM_ERR_ARG);
    assert_int_equal(tpm2_policy_pcr(d, NULL, PCRS), TPM_ERR_ARG);
    assert_int_equal(tpm2_saltkey_close(NULL, NULL), TPM_ERR_ARG);
    assert_int_equal(tpm2_flush(NULL, 0), TPM_ERR_ARG);
    assert_int_equal(tpm2_startup(NULL), TPM_ERR_ARG);
    assert_int_equal(tpm2_nv_name(d, 1, NULL, NULL), TPM_ERR_ARG);
    assert_int_equal(tpm2_nv_undefine(NULL, NULL, 0, 1), TPM_ERR_ARG);
}

/* Against tpm2_createpolicy --policy-pcr -l sha256:0,2,4,7 on a fresh
 * swtpm (all eight PCRs zero), and against Python's hashlib for PCR i
 * holding 32 bytes of value i. */
static void test_tpm2_policy_digest(void **state)
{
    uint8_t vals[4][32], out[32];
    static const uint8_t tools[32] = {
        0x4f, 0x04, 0xc8, 0x37, 0x29, 0x1a, 0xc0, 0x5d,
        0x8c, 0x8f, 0xce, 0xb5, 0xd0, 0x3a, 0xc5, 0x30,
        0xd3, 0x18, 0x9c, 0x1d, 0xd2, 0x4d, 0xc7, 0x44,
        0xf2, 0x14, 0x5c, 0xf7, 0x1e, 0xaa, 0x9c, 0xf6
    };
    static const uint8_t py[32] = {
        0xa1, 0xf6, 0x40, 0xeb, 0x3d, 0x79, 0xa0, 0x83,
        0x2b, 0xc3, 0x99, 0x28, 0xb4, 0x3c, 0xa2, 0x7c,
        0xeb, 0xc6, 0xd5, 0x36, 0x5d, 0xe1, 0xd7, 0xb2,
        0xed, 0x8e, 0x51, 0x38, 0x83, 0x48, 0xfc, 0x96
    };

    (void)state;
    memset(vals, 0, sizeof vals);
    assert_int_equal(tpm2_policy_pcr_digest(PCRS,
                                            (const uint8_t (*)[32])vals, out),
                     TPM_OK);
    assert_memory_equal(out, tools, sizeof tools);
    memset(vals[1], 2, 32);
    memset(vals[2], 4, 32);
    memset(vals[3], 7, 32);
    assert_int_equal(tpm2_policy_pcr_digest(PCRS,
                                            (const uint8_t (*)[32])vals, out),
                     TPM_OK);
    assert_memory_equal(out, py, sizeof py);
}

/* ---- TPM 1.2 ---- */

static void owner_auth(uint8_t out[20])
{
    static const uint8_t zeros[20] = { 0 };

    /* What swtpm_setup's --owner-well-known stores (see tpmrig.c). */
    sha1(zeros, sizeof zeros, out);
}

static int t12_to_first_read(tpm_dev *d, tpm12_pubkey *srk, uint8_t out[20])
{
    uint8_t owner[20];
    tpm_version v;
    int owned = 0, locked = 0;

    owner_auth(owner);
    assert_int_equal(tpm12_startup(d), TPM_OK);
    assert_int_equal(tpm_detect(d, &v), TPM_OK);
    assert_int_equal(v, TPM_VER_12);
    assert_int_equal(tpm12_flags(d, &owned, &locked), TPM_OK);
    assert_true(owned && locked);
    assert_int_equal(tpm12_nv_undefine(d, owner, TPM12_TOTP_INDEX),
                     TPM_ERR_RC);
    assert_int_equal(tpm12_seed_enroll(d, owner, TPM12_TOTP_INDEX, PCRS, SEED,
                                       sizeof SEED, srk),
                     TPM_OK);
    assert_int_equal(srk->mod_len, 256u);
    assert_int_equal(tpm12_nv_read(d, TPM12_TOTP_INDEX, out, 20), TPM_OK);
    assert_memory_equal(out, SEED, sizeof SEED);
    memset(out, 0, 20);
    return tpm12_seed_read(d, TPM12_TOTP_INDEX, owner, srk, out, 20);
}

static void test_tpm12_replay(void **state)
{
    tpm_dev *d = setup(t12_xchg, sizeof t12_xchg / sizeof t12_xchg[0]);
    uint8_t owner[20], out[20], ext[20];
    tpm12_pubkey srk;

    (void)state;
    owner_auth(owner);
    assert_int_equal(t12_to_first_read(d, &srk, out), TPM_OK);
    assert_memory_equal(out, SEED, sizeof SEED);

    memset(ext, 0x5a, sizeof ext);
    assert_int_equal(tpm12_extend(d, 7, ext), TPM_OK);
    assert_int_equal(tpm12_seed_read(d, TPM12_TOTP_INDEX, owner, &srk, out,
                                     sizeof out),
                     TPM_ERR_PCR);
    assert_int_equal(dev.rc, 24u);              /* TPM_WRONGPCRVAL */
    assert_int_equal(tpm12_nv_read(d, TPM12_TOTP_INDEX, out, sizeof out),
                     TPM_ERR_PCR);
    assert_int_equal(tpm12_nv_undefine(d, owner, TPM12_TOTP_INDEX), TPM_OK);
    assert_int_equal(tpm12_nv_read(d, TPM12_TOTP_INDEX, out, sizeof out),
                     TPM_ERR_RC);

    assert_int_equal(rp.mismatch, 0);
    assert_int_equal(rp.i, rp.n);
}

/* ExecuteTransport's response, tampered: transAuth (the last byte), or the
 * wrapped response's first data byte (header 10, currentTicks 8, locality 4,
 * wrappedRspSize 4, the wrapped header 10, dataSize 4). */
static void t12_tamper(long off)
{
    tpm_dev *d = setup(t12_xchg, sizeof t12_xchg / sizeof t12_xchg[0]);
    tpm12_pubkey srk;
    uint8_t out[20];
    static const uint8_t zero[20] = { 0 };

    rp.corrupt_at = find_cc(t12_xchg, rp.n, 0xE7u, 0);
    rp.corrupt_off = off;
    assert_int_equal(t12_to_first_read(d, &srk, out), TPM_ERR_AUTH);
    assert_memory_equal(out, zero, sizeof zero);
}

static void test_tpm12_tamper_auth(void **state)
{
    (void)state;
    t12_tamper(-1);
}

static void test_tpm12_tamper_ciphertext(void **state)
{
    (void)state;
    t12_tamper(10 + 8 + 4 + 4 + 10 + 4);
}

static void test_tpm12_tamper_osap(void **state)
{
    tpm_dev *d = setup(t12_xchg, sizeof t12_xchg / sizeof t12_xchg[0]);
    uint8_t owner[20];
    tpm12_pubkey srk;
    int owned, locked;

    (void)state;
    owner_auth(owner);
    assert_int_equal(tpm12_startup(d), TPM_OK);
    {
        tpm_version v;

        assert_int_equal(tpm_detect(d, &v), TPM_OK);
    }
    assert_int_equal(tpm12_flags(d, &owned, &locked), TPM_OK);
    assert_int_equal(tpm12_nv_undefine(d, owner, TPM12_TOTP_INDEX),
                     TPM_ERR_RC);
    /* OwnerReadInternalPub's response HMAC. */
    rp.corrupt_at = find_cc(t12_xchg, rp.n, 0x81u, 0);
    rp.corrupt_off = -1;
    assert_int_equal(tpm12_seed_enroll(d, owner, TPM12_TOTP_INDEX, PCRS, SEED,
                                       sizeof SEED, &srk),
                     TPM_ERR_AUTH);
}

static void test_tpm12_args(void **state)
{
    tpm_dev *d = setup(t12_xchg, 0);
    uint8_t b[20];
    tpm12_pubkey pk;
    tpm12_transport t;

    (void)state;
    memset(&pk, 0, sizeof pk);
    memset(b, 0, sizeof b);
    assert_int_equal(tpm12_pcr_read(d, 24, b), TPM_ERR_ARG);
    assert_int_equal(tpm12_extend(d, 24, b), TPM_ERR_ARG);
    assert_int_equal(tpm12_composite_digest(0, (const uint8_t (*)[20])&b, b),
                     TPM_ERR_ARG);
    assert_int_equal(tpm12_nv_define(d, b, 1, 0, PCRS, b), TPM_ERR_ARG);
    assert_int_equal(tpm12_nv_write(d, b, 1, b, 0), TPM_ERR_ARG);
    assert_int_equal(tpm12_nv_read(d, 1, b, 0), TPM_ERR_ARG);
    assert_int_equal(tpm12_transport_open(d, b, &pk, &t), TPM_ERR_ARG);
    assert_int_equal(tpm12_nv_read_transport(d, NULL, 1, b, 1), TPM_ERR_ARG);
    assert_int_equal(tpm12_seed_read(d, 1, NULL, NULL, b, 1), TPM_ERR_ARG);
    assert_int_equal(tpm12_seed_enroll(d, b, 1, 0, b, 1, &pk), TPM_ERR_ARG);
    assert_int_equal(tpm12_srk_pub(d, NULL, &pk), TPM_ERR_ARG);
    assert_int_equal(tpm12_flags(NULL, NULL, NULL), TPM_ERR_ARG);
    assert_int_equal(tpm12_startup(NULL), TPM_ERR_ARG);
    assert_int_equal(tpm12_nv_undefine(NULL, b, 1), TPM_ERR_ARG);
}

/* Against Python's hashlib: TPM_PCR_COMPOSITE of PCRs 0, 2, 4, 7 with PCR i
 * holding 20 bytes of value i. */
static void test_tpm12_composite(void **state)
{
    uint8_t vals[4][20], out[20];
    static const uint8_t want[20] = {
        0xbe, 0x34, 0x19, 0x06, 0x52, 0x54, 0xe0, 0xe0, 0xc4, 0x61,
        0x2b, 0xb1, 0x70, 0x0a, 0x04, 0x42, 0xb4, 0x47, 0xe0, 0xd5
    };

    (void)state;
    memset(vals[0], 0, 20);
    memset(vals[1], 2, 20);
    memset(vals[2], 4, 20);
    memset(vals[3], 7, 20);
    assert_int_equal(tpm12_composite_digest(PCRS,
                                            (const uint8_t (*)[20])vals, out),
                     TPM_OK);
    assert_memory_equal(out, want, sizeof want);
}

/* ---- the shared layer ---- */

static void test_tpmbuf(void **state)
{
    uint8_t b[6];
    tpmw w;
    tpmr r;
    uint16_t n;
    uint8_t out[4];
    static const uint8_t in[8] = { 0, 3, 'a', 'b', 'c', 0, 9, 1 };

    (void)state;
    tpmw_init(&w, b, sizeof b);
    tpmw_u32(&w, 0x01020304u);
    tpmw_u16(&w, 0x0506u);
    assert_false(w.err);
    tpmw_u8(&w, 7);                     /* one past the end */
    assert_true(w.err);
    tpmw_u32(&w, 1);                    /* and it stays failed */
    assert_int_equal(w.len, 6u);
    tpmw_init(&w, b, sizeof b);
    tpmw_2b(&w, in, 0x10000u);
    assert_true(w.err);

    tpmr_init(&r, in, sizeof in);
    tpmr_2b(&r, out, sizeof out, &n);
    assert_int_equal(n, 3u);
    assert_memory_equal(out, "abc", 3);
    tpmr_2b(&r, out, 1, &n);            /* 9 bytes do not fit in 1 */
    assert_true(r.err);
    assert_int_equal(n, 0u);
    assert_int_equal(tpmr_u64(&r), 0u);

    assert_int_equal(tpm_detect(NULL, NULL), TPM_ERR_ARG);
}

static int bad_tag_xfer(void *ctx, const uint8_t *cmd, size_t cmd_len,
                        uint8_t *rsp, size_t rsp_cap, size_t *rsp_len)
{
    static const uint8_t r[10] = { 0x12, 0x34, 0, 0, 0, 10, 0, 0, 0, 0 };

    (void)ctx;
    (void)cmd;
    (void)cmd_len;
    (void)rsp_cap;
    memcpy(rsp, r, sizeof r);
    *rsp_len = sizeof r;
    return 0;
}

static void test_detect_unknown(void **state)
{
    tpm_version v;

    (void)state;
    memset(&dev, 0, sizeof dev);
    dev.xfer = bad_tag_xfer;
    assert_int_equal(tpm_detect(&dev, &v), TPM_ERR_PROTO);
    assert_int_equal(v, TPM_VER_NONE);
}

int run_tpm_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_tpm2_replay),
        cmocka_unit_test(test_tpm2_tamper_hmac),
        cmocka_unit_test(test_tpm2_tamper_ciphertext),
        cmocka_unit_test(test_tpm2_faults),
        cmocka_unit_test(test_tpm2_args),
        cmocka_unit_test(test_tpm2_policy_digest),
        cmocka_unit_test(test_tpm12_replay),
        cmocka_unit_test(test_tpm12_tamper_auth),
        cmocka_unit_test(test_tpm12_tamper_ciphertext),
        cmocka_unit_test(test_tpm12_tamper_osap),
        cmocka_unit_test(test_tpm12_args),
        cmocka_unit_test(test_tpm12_composite),
        cmocka_unit_test(test_tpmbuf),
        cmocka_unit_test(test_detect_unknown),
    };

    return cmocka_run_group_tests_name("tpm", tests, NULL, NULL);
}
