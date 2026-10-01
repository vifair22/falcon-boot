/* Tests for falcon-core/src/tpm/evlog.c: the event log replay the station's
 * TOTP enrollment seals to (docs/DESIGN.md Section 12.15).
 *
 * The logs are built here, event by event, in both formats, and the
 * expected PCRs are extended alongside by hand, so each test states what a
 * replay must give. The real logs, OVMF's in both TPM versions, are the
 * TPM rig's (falcon-efi/tools/run-tpm-rig.sh): an enrollment from them
 * that the station then unseals is the proof that the replay agrees with
 * the TPM.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <crypto/sha1.h>
#include <crypto/sha256.h>
#include <tpm/evlog.h>
#include <tpm/evsave.h>

static uint8_t g_log[4096];
static size_t g_len;

static void put32(uint32_t v)
{
    g_log[g_len++] = (uint8_t)v;
    g_log[g_len++] = (uint8_t)(v >> 8);
    g_log[g_len++] = (uint8_t)(v >> 16);
    g_log[g_len++] = (uint8_t)(v >> 24);
}

static void put16(uint16_t v)
{
    g_log[g_len++] = (uint8_t)v;
    g_log[g_len++] = (uint8_t)(v >> 8);
}

static void putb(const void *p, size_t n)
{
    memcpy(g_log + g_len, p, n);
    g_len += n;
}

/* A 1.2 event (TCG_PCR_EVENT). */
static void ev12(uint32_t pcr, uint32_t type, const uint8_t d[20],
                 const char *data)
{
    size_t n = strlen(data);

    put32(pcr);
    put32(type);
    putb(d, 20);
    put32((uint32_t)n);
    putb(data, n);
}

/* The crypto-agile header: SHA-1 and SHA-256. */
static void hdr2(void)
{
    static const uint8_t zero[20] = { 0 };
    uint8_t data[64];
    size_t n = 0;

    memcpy(data, "Spec ID Event03", 16);
    n = 16;
    memset(data + n, 0, 8);         /* platformClass, versions, uintnSize */
    n += 8;
    data[n++] = 2;                  /* numberOfAlgorithms */
    data[n++] = 0;
    data[n++] = 0;
    data[n++] = 0;
    data[n++] = 0x04; data[n++] = 0; data[n++] = 20; data[n++] = 0;
    data[n++] = 0x0B; data[n++] = 0; data[n++] = 32; data[n++] = 0;
    data[n++] = 0;                  /* vendorInfoSize */
    put32(0);
    put32(EVLOG_EV_NO_ACTION);
    putb(zero, 20);
    put32((uint32_t)n);
    putb(data, n);
}

/* A TCG_PCR_EVENT2 carrying both banks. */
static void ev2(uint32_t pcr, uint32_t type, const uint8_t d1[20],
                const uint8_t d256[32], const void *data, size_t n)
{
    put32(pcr);
    put32(type);
    put32(2);
    put16(0x0004);
    putb(d1, 20);
    put16(0x000B);
    putb(d256, 32);
    put32((uint32_t)n);
    putb(data, n);
}

static void ext1(uint8_t v[20], const uint8_t d[20])
{
    sha1_ctx h;

    sha1_init(&h);
    sha1_update(&h, v, 20);
    sha1_update(&h, d, 20);
    sha1_final(&h, v);
}

static void ext256(uint8_t v[32], const uint8_t d[32])
{
    sha256_ctx h;

    sha256_init(&h);
    sha256_update(&h, v, 32);
    sha256_update(&h, d, 32);
    sha256_final(&h, v);
}

static void digests(uint8_t tag, uint8_t d1[20], uint8_t d256[32])
{
    memset(d1, tag, 20);
    memset(d256, tag, 32);
}

static void test_tcg12(void **state)
{
    uint8_t a[20], b[20], c[20], st[20];
    uint8_t p0[20] = { 0 }, p4[20] = { 0 }, p7[20] = { 0 };
    evlog_replay r;

    (void)state;
    g_len = 0;
    memset(a, 1, 20);
    memset(b, 2, 20);
    memset(c, 3, 20);
    memset(st, 0x5a, 20);
    ev12(0, 0x08, a, "crtm");
    ext1(p0, a);
    ev12(7, 0x80000001u, b, "SecureBoot");
    ext1(p7, b);
    ev12(4, EVLOG_EV_EFI_BOOT_APP, c, "shim");
    ext1(p4, c);
    ev12(18, 0x1, a, "not a boot PCR");
    ev12(4, EVLOG_EV_NO_ACTION, a, "not extended");
    ev12(4, EVLOG_EV_EFI_BOOT_APP, st, "the station");
    ext1(p4, st);
    ev12(4, EVLOG_EV_EFI_BOOT_APP, c, "a UKI");
    ev12(7, 0x800000E0u, b, "its authority");
    memset(g_log + g_len, 0, 64);   /* trailing zeros end the log */
    g_len += 64;

    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA1, 4, st,
                                      &r), EVLOG_OK);
    assert_int_equal(r.format, EVLOG_FMT_TCG12);
    assert_true(r.stopped);
    assert_int_equal(r.events, 6);
    assert_memory_equal(r.pcr[0], p0, 20);
    assert_memory_equal(r.pcr[4], p4, 20);
    assert_memory_equal(r.pcr[7], p7, 20);

    /* To the end: the UKI and its authority are in. */
    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA1, 0, NULL,
                                      &r), EVLOG_OK);
    ext1(p4, c);
    ext1(p7, b);
    assert_false(r.stopped);
    assert_int_equal(r.events, 8);
    assert_memory_equal(r.pcr[4], p4, 20);
    assert_memory_equal(r.pcr[7], p7, 20);

    /* A digest nothing carries is not found. */
    memset(st, 0x77, 20);
    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA1, 4, st,
                                      &r), EVLOG_OK);
    assert_false(r.stopped);

    /* SHA-256 is not in a 1.2 log. */
    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA256, 4, st,
                                      &r), EVLOG_ERR_BANK);
    /* An event that runs past the end. */
    assert_int_equal(evlog_replay_run(g_log, 40, EVLOG_ALG_SHA1, 0, NULL,
                                      &r), EVLOG_ERR_FORMAT);
    g_log[28] = 0xFF;
    assert_int_equal(evlog_replay_run(g_log, 200, EVLOG_ALG_SHA1, 0, NULL,
                                      &r), EVLOG_ERR_FORMAT);
}

/* The station's measurement replaced by a new loader's, as mkstick update
 * seals to it: PCR 4 reaches what it would with the new image measured in
 * the old one's place, the stop is still the old digest in the log, and
 * no other PCR moves. */
static void test_substitute(void **state)
{
    uint8_t a[20], c[20], st[20], nw[20];
    uint8_t d256a[32], d256st[32], d256nw[32];
    uint8_t p4[20] = { 0 };
    uint8_t q4[32] = { 0 };
    evlog_replay r;

    (void)state;
    g_len = 0;
    memset(a, 1, 20);
    memset(c, 3, 20);
    memset(st, 0x5a, 20);
    memset(nw, 0x6b, 20);
    ev12(0, 0x08, a, "crtm");
    ev12(4, EVLOG_EV_EFI_BOOT_APP, c, "shim");
    ext1(p4, c);
    ev12(4, EVLOG_EV_EFI_BOOT_APP, st, "the station");
    ext1(p4, nw);
    ev12(4, EVLOG_EV_EFI_BOOT_APP, c, "a UKI");
    memset(g_log + g_len, 0, 64);
    g_len += 64;

    assert_int_equal(evlog_replay_init(EVLOG_ALG_SHA1, &r), EVLOG_OK);
    evlog_replay_substitute(&r, 4, st, nw);
    assert_int_equal(evlog_replay_more(g_log, g_len, 4, st, &r), EVLOG_OK);
    assert_true(r.stopped);
    assert_int_equal(r.substituted, 1);
    assert_memory_equal(r.pcr[4], p4, 20);
    {
        uint8_t p0[20] = { 0 };

        ext1(p0, a);
        assert_memory_equal(r.pcr[0], p0, 20);
    }

    /* Another PCR's event with the same digest is not the station's. */
    assert_int_equal(evlog_replay_init(EVLOG_ALG_SHA1, &r), EVLOG_OK);
    evlog_replay_substitute(&r, 7, st, nw);
    assert_int_equal(evlog_replay_more(g_log, g_len, 4, st, &r), EVLOG_OK);
    assert_int_equal(r.substituted, 0);

    /* Nothing to substitute with, or no replay: nothing happens. */
    assert_int_equal(evlog_replay_init(EVLOG_ALG_SHA1, &r), EVLOG_OK);
    evlog_replay_substitute(&r, 4, NULL, nw);
    evlog_replay_substitute(NULL, 4, st, nw);
    evlog_replay_substitute(&r, EVLOG_PCRS, st, nw);
    assert_false(r.sub_on);

    /* The crypto-agile log, in its SHA-256 bank. */
    g_len = 0;
    memset(d256a, 1, 32);
    memset(d256st, 0x5a, 32);
    memset(d256nw, 0x6b, 32);
    hdr2();
    ev2(4, EVLOG_EV_EFI_BOOT_APP, a, d256a, "shim", 4);
    ext256(q4, d256a);
    ev2(4, EVLOG_EV_EFI_BOOT_APP, st, d256st, "station", 7);
    ext256(q4, d256nw);
    assert_int_equal(evlog_replay_init(EVLOG_ALG_SHA256, &r), EVLOG_OK);
    evlog_replay_substitute(&r, 4, d256st, d256nw);
    assert_int_equal(evlog_replay_more(g_log, g_len, 4, d256st, &r),
                     EVLOG_OK);
    assert_true(r.stopped);
    assert_int_equal(r.substituted, 1);
    assert_memory_equal(r.pcr[4], q4, 32);
}

static void test_tcg2(void **state)
{
    uint8_t a1[20], a2[32], b1[20], b2[32], s1[20], s2[32], u1[20], u2[32];
    uint8_t p0[32] = { 0 }, p4[32] = { 0 }, p7[32] = { 0 };
    uint8_t q0[20] = { 0 }, q4[20] = { 0 };
    uint8_t loc[17];
    evlog_replay r;

    (void)state;
    g_len = 0;
    hdr2();
    digests(1, a1, a2);
    digests(2, b1, b2);
    digests(0x5a, s1, s2);
    digests(3, u1, u2);
    memcpy(loc, "StartupLocality", 16);
    loc[16] = 3;
    ev2(0, EVLOG_EV_NO_ACTION, a1, a2, loc, sizeof loc);
    p0[31] = 3;
    q0[19] = 3;
    ev2(0, 0x08, a1, a2, "crtm", 4);
    ext256(p0, a2);
    ext1(q0, a1);
    ev2(7, 0x80000001u, b1, b2, "SecureBoot", 10);
    ext256(p7, b2);
    ev2(4, EVLOG_EV_EFI_BOOT_APP, s1, s2, "the station", 11);
    ext256(p4, s2);
    ext1(q4, s1);
    ev2(4, EVLOG_EV_EFI_BOOT_APP, u1, u2, "a UKI", 5);
    ev2(20, 0x1, u1, u2, "", 0);

    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA256, 4, s2,
                                      &r), EVLOG_OK);
    assert_int_equal(r.format, EVLOG_FMT_TCG2);
    assert_true(r.stopped);
    assert_int_equal(r.events, 5);
    assert_memory_equal(r.pcr[0], p0, 32);
    assert_memory_equal(r.pcr[4], p4, 32);
    assert_memory_equal(r.pcr[7], p7, 32);

    /* The SHA-1 bank of the same log. */
    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA1, 4, s1,
                                      &r), EVLOG_OK);
    assert_true(r.stopped);
    assert_memory_equal(r.pcr[0], q0, 20);
    assert_memory_equal(r.pcr[4], q4, 20);

    /* To the end. */
    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA256, 0,
                                      NULL, &r), EVLOG_OK);
    ext256(p4, u2);
    assert_false(r.stopped);
    assert_int_equal(r.events, 7);
    assert_memory_equal(r.pcr[4], p4, 32);

    /* Cut inside the last event. */
    assert_int_equal(evlog_replay_run(g_log, g_len - 3, EVLOG_ALG_SHA256, 0,
                                      NULL, &r), EVLOG_ERR_FORMAT);
}

static void test_tcg2_bad(void **state)
{
    uint8_t a1[20], a2[32];
    evlog_replay r;
    size_t hdr;

    (void)state;
    g_len = 0;
    hdr2();
    hdr = g_len;
    digests(1, a1, a2);
    ev2(0, 0x08, a1, a2, "x", 1);

    /* An event with an algorithm the header does not name. */
    g_log[hdr + 12] = 0x0C;
    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA256, 0,
                                      NULL, &r), EVLOG_ERR_FORMAT);
    g_log[hdr + 12] = 0x04;
    /* Too many digests. */
    g_log[hdr + 8] = 9;
    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA256, 0,
                                      NULL, &r), EVLOG_ERR_FORMAT);
    /* A measurement carrying SHA-1 alone: the SHA-256 bank is missing. */
    {
        size_t keep = g_len;

        g_len = hdr;
        put32(0);
        put32(0x08);
        put32(1);
        put16(0x0004);
        putb(a1, 20);
        put32(0);
        assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA256, 0,
                                          NULL, &r), EVLOG_ERR_FORMAT);
        /* The SHA-1 bank of it replays. */
        assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA1, 0,
                                          NULL, &r), EVLOG_OK);
        g_len = hdr;
        ev2(0, 0x08, a1, a2, "x", 1);
        assert_int_equal(g_len, keep);
    }
    /* A header whose table is empty. */
    g_log[32 + 24] = 0;
    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA256, 0,
                                      NULL, &r), EVLOG_ERR_FORMAT);
    g_log[32 + 24] = 2;
    /* A bank the header does not carry. */
    g_log[32 + 32] = 0x0C;
    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA256, 0,
                                      NULL, &r), EVLOG_ERR_BANK);
    g_log[32 + 32] = 0x0B;
    /* A digest size past what any bank has. */
    g_log[32 + 34] = 99;
    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA256, 0,
                                      NULL, &r), EVLOG_ERR_FORMAT);
    g_log[32 + 34] = 32;
    /* A short tail after the header. */
    assert_int_equal(evlog_replay_run(g_log, hdr + 7, EVLOG_ALG_SHA256, 0,
                                      NULL, &r), EVLOG_ERR_FORMAT);
    assert_int_equal(evlog_replay_run(g_log, hdr + 13, EVLOG_ALG_SHA256, 0,
                                      NULL, &r), EVLOG_ERR_FORMAT);
    assert_int_equal(evlog_replay_run(g_log, hdr + 12 + 22 + 34 + 2,
                                      EVLOG_ALG_SHA256, 0, NULL, &r),
                     EVLOG_ERR_FORMAT);

    /* Arguments. */
    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA256, 0,
                                      NULL, NULL), EVLOG_ERR_ARG);
    assert_int_equal(evlog_replay_run(NULL, g_len, EVLOG_ALG_SHA256, 0,
                                      NULL, &r), EVLOG_ERR_ARG);
    assert_int_equal(evlog_replay_run(g_log, 10, EVLOG_ALG_SHA256, 0, NULL,
                                      &r), EVLOG_ERR_ARG);
    assert_int_equal(evlog_replay_run(g_log, g_len, 0x000C, 0, NULL, &r),
                     EVLOG_ERR_ARG);
}

/* Two 1.2 logs replayed in turn (coreboot's, then its payload's) give what
 * one log holding both would, and a TCG2 log cannot follow a 1.2 one. */
static void test_more(void **state)
{
    static evlog_replay one;
    static evlog_replay two;
    static uint8_t first[512];
    uint8_t a[20], b[20], c[20], d256[32];
    size_t first_len;

    (void)state;
    memset(a, 0x11, 20);
    memset(b, 0x22, 20);
    memset(c, 0x33, 20);

    g_len = 0;
    ev12(2, 5, a, "CBFS: fallback/payload");
    ev12(2, 5, b, "CBFS: bootblock");
    memcpy(first, g_log, g_len);
    first_len = g_len;
    ev12(2, 4, c, "sep");
    ev12(4, EVLOG_EV_EFI_BOOT_APP, a, "image");
    assert_int_equal(evlog_replay_run(g_log, g_len, EVLOG_ALG_SHA1, 0, NULL,
                                      &one), EVLOG_OK);

    assert_int_equal(evlog_replay_init(EVLOG_ALG_SHA1, &two), EVLOG_OK);
    assert_int_equal(evlog_replay_more(first, first_len, 0, NULL, &two),
                     EVLOG_OK);
    assert_int_equal(evlog_replay_more(g_log + first_len, g_len - first_len,
                                       4, a, &two), EVLOG_OK);
    assert_true(two.stopped);
    assert_int_equal(two.events, 4);
    assert_memory_equal(one.pcr, two.pcr, sizeof one.pcr);

    /* In the other order PCR 2 differs. */
    assert_int_equal(evlog_replay_init(EVLOG_ALG_SHA1, &two), EVLOG_OK);
    assert_int_equal(evlog_replay_more(g_log + first_len, g_len - first_len,
                                       0, NULL, &two), EVLOG_OK);
    assert_int_equal(evlog_replay_more(first, first_len, 0, NULL, &two),
                     EVLOG_OK);
    assert_memory_not_equal(one.pcr[2], two.pcr[2], 20);
    assert_memory_equal(one.pcr[4], two.pcr[4], 20);

    /* A crypto-agile log after a 1.2 one. */
    g_len = 0;
    hdr2();
    digests(0x44, c, d256);
    ev2(4, EVLOG_EV_EFI_BOOT_APP, c, d256, "x", 1);
    assert_int_equal(evlog_replay_more(g_log, g_len, 0, NULL, &two),
                     EVLOG_ERR_FORMAT);
    assert_int_equal(evlog_replay_more(NULL, 0, 0, NULL, &two),
                     EVLOG_ERR_ARG);
    assert_int_equal(evlog_replay_init(0x0012, &two), EVLOG_ERR_ARG);
}

/* The station's saved record: written and read back, and anything else
 * refused. */
static void test_evsave(void **state)
{
    static uint8_t pcr[EVSAVE_PCRS][EVSAVE_DIGEST_LEN];
    static uint8_t buf[EVSAVE_BODY + 64];
    static const uint8_t log[40] = { 1, 2, 3 };
    evsave_view v;
    size_t n = 0;
    unsigned i;

    (void)state;
    for (i = 0; i < EVSAVE_PCRS; i++)
        memset(pcr[i], (int)i, EVSAVE_DIGEST_LEN);
    assert_int_equal(evsave_size(sizeof log), EVSAVE_BODY + sizeof log);
    assert_int_equal(evsave_write(buf, sizeof buf,
                                  (const uint8_t (*)[EVSAVE_DIGEST_LEN])pcr,
                                  log, sizeof log, &n), EVSAVE_OK);
    assert_int_equal(n, EVSAVE_BODY + sizeof log);
    assert_memory_equal(buf, "FEVL", 4);
    assert_int_equal(evsave_read(buf, n, &v), EVSAVE_OK);
    assert_int_equal(v.log_len, sizeof log);
    assert_memory_equal(v.log, log, sizeof log);
    assert_memory_equal(v.pcr, pcr, sizeof pcr);

    /* Short, long, or not a record. */
    assert_int_equal(evsave_read(buf, n - 1u, &v), EVSAVE_ERR_FORMAT);
    assert_int_equal(evsave_read(buf, n + 1u, &v), EVSAVE_ERR_FORMAT);
    assert_int_equal(evsave_read(buf, 8, &v), EVSAVE_ERR_FORMAT);
    buf[4] = 2;
    assert_int_equal(evsave_read(buf, n, &v), EVSAVE_ERR_FORMAT);
    buf[4] = 1;
    buf[8] = 32;
    assert_int_equal(evsave_read(buf, n, &v), EVSAVE_ERR_FORMAT);
    buf[8] = 20;
    buf[0] = 'X';
    assert_int_equal(evsave_read(buf, n, &v), EVSAVE_ERR_FORMAT);
    assert_int_equal(evsave_read(NULL, n, &v), EVSAVE_ERR_ARG);

    assert_int_equal(evsave_write(buf, EVSAVE_BODY + 39u,
                                  (const uint8_t (*)[EVSAVE_DIGEST_LEN])pcr,
                                  log, sizeof log, &n), EVSAVE_ERR_SPACE);
    assert_int_equal(evsave_write(buf, sizeof buf,
                                  (const uint8_t (*)[EVSAVE_DIGEST_LEN])pcr,
                                  log, 0, &n), EVSAVE_ERR_ARG);
}

int run_evlog_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_tcg12),
        cmocka_unit_test(test_tcg2),
        cmocka_unit_test(test_substitute),
        cmocka_unit_test(test_tcg2_bad),
        cmocka_unit_test(test_more),
        cmocka_unit_test(test_evsave),
    };

    return cmocka_run_group_tests_name("evlog", tests, NULL, NULL);
}
