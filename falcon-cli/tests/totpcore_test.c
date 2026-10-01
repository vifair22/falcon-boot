/* Tests for falcon-cli/src/mkstick/totpcore.c: the pure half of `station
 * totp` (docs/DESIGN.md Section 12.15).
 *
 * The base32 vectors are RFC 4648 Section 10's; the URI is the Key Uri
 * Format authenticators read, checked whole; the STATION.INI update must
 * leave everything but [totp] byte for byte, and refuse a file that does
 * not parse rather than write over it. The two-log merge runs on the
 * T430's own logs (tests/data/t430/), against the PCRs the probe read in
 * the same boot.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <stdio.h>
#include <stdlib.h>

#include <crypto/sha1.h>
#include <mkstick/totpcore.h>
#include <tpm/evlog.h>
#include <tpm/evsave.h>

static void test_pcrs(void **state)
{
    uint32_t m = 0;

    (void)state;
    assert_int_equal(totpc_parse_pcrs("0,2,4,7", &m), TOTPC_OK);
    assert_true(m == 0x95u);
    assert_int_equal(totpc_parse_pcrs(" 15 , 1", &m), TOTPC_OK);
    assert_true(m == 0x8002u);
    assert_int_equal(totpc_parse_pcrs("16", &m), TOTPC_ERR_ARG);
    assert_int_equal(totpc_parse_pcrs("1,1", &m), TOTPC_ERR_ARG);
    assert_int_equal(totpc_parse_pcrs("", &m), TOTPC_ERR_ARG);
    assert_int_equal(totpc_parse_pcrs("1,", &m), TOTPC_ERR_ARG);
    assert_int_equal(totpc_parse_pcrs("1;2", &m), TOTPC_ERR_ARG);
    assert_int_equal(totpc_parse_pcrs("0007", &m), TOTPC_ERR_ARG);
    assert_int_equal(totpc_parse_pcrs(NULL, &m), TOTPC_ERR_ARG);
}

static void test_base32(void **state)
{
    static const struct {
        const char *in;
        const char *out;
    } v[] = {
        { "", "" }, { "f", "MY" }, { "fo", "MZXQ" }, { "foo", "MZXW6" },
        { "foob", "MZXW6YQ" }, { "fooba", "MZXW6YTB" },
        { "foobar", "MZXW6YTBOI" },
    };
    char out[32];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        assert_int_equal(totpc_base32((const uint8_t *)v[i].in,
                                      strlen(v[i].in), out, sizeof out),
                         TOTPC_OK);
        assert_string_equal(out, v[i].out);
    }
    assert_int_equal(totpc_base32((const uint8_t *)"foobar", 6, out, 10),
                     TOTPC_ERR_SPACE);
    assert_int_equal(totpc_base32((const uint8_t *)"foobar", 6, out, 11),
                     TOTPC_OK);
}

static void test_uri(void **state)
{
    static const uint8_t seed[21] = "12345678901234567890";
    char out[TOTP_URI_MAX];

    (void)state;
    assert_int_equal(totpc_uri("desk", seed, 20, out, sizeof out), TOTPC_OK);
    assert_string_equal(out, "otpauth://totp/falcon-station:desk?secret="
                        "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer="
                        "falcon-station&algorithm=SHA1&digits=6&period=30");
    assert_int_equal(totpc_uri("a b/c", seed, 20, out, sizeof out),
                     TOTPC_OK);
    assert_non_null(strstr(out, "falcon-station:a%20b%2Fc?secret="));
    assert_int_equal(totpc_uri("desk", seed, 20, out, 40), TOTPC_ERR_SPACE);
    assert_int_equal(totpc_uri("desk", seed, 20, out, 20), TOTPC_ERR_SPACE);
    assert_int_equal(totpc_uri("desk", seed, 20, out, 120), TOTPC_ERR_SPACE);
    assert_int_equal(totpc_uri(NULL, seed, 20, out, sizeof out),
                     TOTPC_ERR_ARG);
    assert_int_equal(totpc_uri("desk", seed, 33, out, sizeof out),
                     TOTPC_ERR_ARG);
}

static void test_owner(void **state)
{
    uint8_t o[20];
    uint8_t want[20];
    uint8_t raw[20];

    (void)state;
    assert_int_equal(totpc_trim((const uint8_t *)"pw\n", 3), 2);
    assert_int_equal(totpc_trim((const uint8_t *)"pw\r\n", 4), 2);
    assert_int_equal(totpc_trim((const uint8_t *)"pw", 2), 2);
    assert_int_equal(totpc_trim(NULL, 2), 0);

    totpc_owner12(NULL, 0, o);
    memset(want, 0, sizeof want);
    assert_memory_equal(o, want, 20);
    memset(raw, 0x42, sizeof raw);
    totpc_owner12(raw, 20, o);
    assert_memory_equal(o, raw, 20);
    totpc_owner12((const uint8_t *)"owner\n", 6, o);
    sha1("owner", 5, want);
    assert_memory_equal(o, want, 20);
}

#define NAME2 "000b00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

static void totp2(stationini_totp *t)
{
    static const uint8_t key[34] = {
        0x00, 0x0b, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11, 0x22, 0x33,
        0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
        0xff
    };

    memset(t, 0, sizeof *t);
    t->present = 1;
    t->tpm = 20;
    t->index = 0x01800FA1u;
    t->pcrs = 0x95u;
    memcpy(t->key, key, sizeof key);
    t->key_len = sizeof key;
    strcpy(t->label, "falcon-station:desk");
}

static const char k_sect[] =
    "[totp]\ntpm = 2.0\nindex = 0x01800fa1\npcrs = 0, 2, 4, 7\n"
    "key = " NAME2 "\nlabel = falcon-station:desk\n";

static void test_ini_update(void **state)
{
    static const char before[] =
        "# my notes\n[general]\ndefault = memtest ; keep\ntimeout = 3\n";
    static const char with_old[] =
        "# my notes\n[general]\ndefault = memtest ; keep\ntimeout = 3\n"
        "\n[totp]\ntpm = 1.2\nindex = 0x1\npcrs = 1\nkey = " NAME2 NAME2
        NAME2 NAME2 "\n[m]\nhide = yes";
    stationini_totp t;
    char out[4096];
    char why[STATIONINI_WHY_MAX];
    char want[4096];
    unsigned line = 0;
    size_t n = 0;

    (void)state;
    totp2(&t);
    /* No file yet. */
    assert_int_equal(totpc_ini_update(NULL, 0, &t, out, sizeof out, &n,
                                      &line, why), TOTPC_OK);
    assert_string_equal(out, k_sect);
    assert_int_equal(n, strlen(k_sect));

    /* Appended to a file, which keeps its comments. */
    assert_int_equal(totpc_ini_update(before, strlen(before), &t, out,
                                      sizeof out, &n, &line, why), TOTPC_OK);
    strcpy(want, before);
    strcat(want, "\n");
    strcat(want, k_sect);
    assert_string_equal(out, want);

    /* An old section is replaced; the one after it stays, and its last line
     * gets its newline. */
    assert_int_equal(totpc_ini_update(with_old, strlen(with_old), &t, out,
                                      sizeof out, &n, &line, why), TOTPC_OK);
    strcpy(want, before);
    strcat(want, "\n[m]\nhide = yes\n\n");
    strcat(want, k_sect);
    assert_string_equal(out, want);

    /* Removed. */
    assert_int_equal(totpc_ini_update(with_old, strlen(with_old), NULL, out,
                                      sizeof out, &n, &line, why), TOTPC_OK);
    strcpy(want, before);
    strcat(want, "\n[m]\nhide = yes");
    assert_string_equal(out, want);

    /* A file that does not parse is refused, with its line. */
    assert_int_equal(totpc_ini_update("[general]\nbogus = 1\n", 20, &t, out,
                                      sizeof out, &n, &line, why),
                     TOTPC_ERR_INI);
    assert_int_equal(line, 2);
    assert_non_null(strstr(why, "unknown key"));

    /* Too small, a bad section, and arguments. */
    assert_int_equal(totpc_ini_update(before, strlen(before), &t, out, 40,
                                      &n, &line, why), TOTPC_ERR_SPACE);
    assert_int_equal(totpc_ini_update(before, strlen(before), &t, out,
                                      strlen(before) + 1u, &n, &line, why),
                     TOTPC_ERR_SPACE);
    t.tpm = 3;
    assert_int_equal(totpc_ini_update(before, strlen(before), &t, out,
                                      sizeof out, &n, &line, why),
                     TOTPC_ERR_ARG);
    assert_int_equal(totpc_ini_update(before, strlen(before), NULL, NULL,
                                      sizeof out, &n, &line, why),
                     TOTPC_ERR_ARG);
    assert_int_equal(totpc_ini_update(NULL, 5, NULL, out, sizeof out, &n,
                                      &line, why), TOTPC_ERR_ARG);
}

/* ---- the two-log merge, on the T430's logs ---- */

/* PCRs 0-7 as the probe read them after edk2's log was saved; 8-15 are
 * zero. */
static const char *const k_t430_pcr[8] = {
    "b2a83b0ebf2f8374299a5b2bdfc31ea955ad7236",
    "b2a83b0ebf2f8374299a5b2bdfc31ea955ad7236",
    "cd0cc5fb51315f1bed4c6cf00d26d9e5e862b2ee",
    "b2a83b0ebf2f8374299a5b2bdfc31ea955ad7236",
    "e261826830c6814b4aa146604b180975df51b2f5",
    "799d42c8e4f1e04515a202e0b057564ea3122aae",
    "b2a83b0ebf2f8374299a5b2bdfc31ea955ad7236",
    "273f46ed2c9483b2acf4a96b9d7efae2e250835c",
};

/* An image edk2 measured into PCR 4 after the separators, standing in for
 * the station's (the probe ran from the Shell, not the station). */
static const char k_t430_image[] = "942422fe19444f104d2426f17fa62590ec5bd495";

static void unhex(const char *s, uint8_t *out, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned v = 0;

        assert_int_equal(sscanf(s + 2u * i, "%2x", &v), 1);
        out[i] = (uint8_t)v;
    }
}

static uint8_t *slurp(const char *name, size_t *len)
{
    char path[512];
    uint8_t *buf;
    FILE *f;
    long n;

    snprintf(path, sizeof path, "%s/t430/%s", CLI_TEST_DATA, name);
    f = fopen(path, "rb");
    assert_non_null(f);
    assert_int_equal(fseek(f, 0, SEEK_END), 0);
    n = ftell(f);
    assert_true(n > 0);
    rewind(f);
    buf = malloc((size_t)n);
    assert_non_null(buf);
    assert_int_equal(fread(buf, 1, (size_t)n, f), (size_t)n);
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static uint8_t g_rec[EVSAVE_BODY + 8192];

static size_t record(const uint8_t pcr[16][20], const uint8_t *log,
                     size_t len)
{
    size_t n = 0;

    assert_int_equal(evsave_write(g_rec, sizeof g_rec, pcr, log, len, &n),
                     EVSAVE_OK);
    return n;
}

static void test_merge_t430(void **state)
{
    static uint8_t pcr[16][20];
    static uint8_t out[16][20];
    uint8_t image[20];
    size_t cb_len = 0;
    size_t ek_len = 0;
    uint8_t *cb = slurp("coreboot-evlog.bin", &cb_len);
    uint8_t *ek = slurp("edk2-evlog.bin", &ek_len);
    unsigned from = 0;
    unsigned bad = 99;
    size_t n;
    unsigned i;

    (void)state;
    memset(pcr, 0, sizeof pcr);
    for (i = 0; i < 8u; i++)
        unhex(k_t430_pcr[i], pcr[i], 20);
    unhex(k_t430_image, image, 20);
    n = record((const uint8_t (*)[20])pcr, ek, ek_len);

    /* coreboot's log first, then edk2's: the probe's PCRs. */
    assert_int_equal(totpc_merge12(cb, cb_len, g_rec, n, image, out, &from,
                                   &bad), TOTPC_OK);
    assert_int_equal(from, TOTPC_FROM_BOTH);
    assert_memory_equal(out, pcr, sizeof pcr);

    /* A station image not booted yet (mkstick update): the station's own
     * image as itself is the station's reading; another's moves PCR 4, and
     * PCR 4 alone. */
    {
        static uint8_t as_out[16][20];
        uint8_t other[20];

        assert_int_equal(totpc_merge12_as(cb, cb_len, g_rec, n, image, image,
                                          as_out, &from, &bad), TOTPC_OK);
        assert_memory_equal(as_out, pcr, sizeof pcr);
        memset(other, 0xA5, sizeof other);
        assert_int_equal(totpc_merge12_as(cb, cb_len, g_rec, n, image, other,
                                          as_out, &from, &bad), TOTPC_OK);
        assert_int_equal(from, TOTPC_FROM_BOTH);
        for (i = 0; i < 16u; i++) {
            if (i == 4u)
                assert_memory_not_equal(as_out[i], pcr[i], 20);
            else
                assert_memory_equal(as_out[i], pcr[i], 20);
        }
    }

    /* edk2's alone leaves PCR 2 without coreboot's measurements. */
    assert_int_equal(totpc_merge12(NULL, 0, g_rec, n, image, out, &from,
                                   &bad), TOTPC_ERR_PCR);
    assert_int_equal(bad, 2);

    /* A second copy of edk2's log is not coreboot's: counted twice, PCR 0
     * is wrong first. */
    assert_int_equal(totpc_merge12(ek, ek_len, g_rec, n, image, out, &from,
                                   &bad), TOTPC_ERR_PCR);
    assert_int_equal(bad, 0);

    /* The station's measurement must be in the saved log, not Linux's. */
    memset(image, 0, sizeof image);
    assert_int_equal(totpc_merge12(cb, cb_len, g_rec, n, image, out, &from,
                                   &bad), TOTPC_ERR_STATION);
    unhex(k_t430_image, image, 20);

    /* A PCR the station read that no replay gives. */
    pcr[7][0] ^= 1u;
    n = record((const uint8_t (*)[20])pcr, ek, ek_len);
    assert_int_equal(totpc_merge12(cb, cb_len, g_rec, n, image, out, &from,
                                   &bad), TOTPC_ERR_PCR);
    assert_int_equal(bad, 7);

    /* Not a record. */
    g_rec[0] = 'X';
    assert_int_equal(totpc_merge12(cb, cb_len, g_rec, n, image, out, &from,
                                   &bad), TOTPC_ERR_LOG);
    assert_int_equal(totpc_merge12(cb, cb_len, NULL, 0, image, out, &from,
                                   &bad), TOTPC_ERR_ARG);
    free(cb);
    free(ek);
}

/* One firmware log (edk2 without coreboot, or the TPM rig's OVMF): the
 * saved log alone is the whole story, and Linux's is not needed. */
static void test_merge_saved_alone(void **state)
{
    static evlog_replay r;
    static uint8_t pcr[16][20];
    static uint8_t out[16][20];
    uint8_t image[20];
    size_t ek_len = 0;
    uint8_t *ek = slurp("edk2-evlog.bin", &ek_len);
    unsigned from = 0;
    unsigned bad = 0;
    size_t n;
    unsigned i;

    (void)state;
    assert_int_equal(evlog_replay_run(ek, ek_len, EVLOG_ALG_SHA1, 0, NULL, &r),
                     EVLOG_OK);
    for (i = 0; i < 16u; i++)
        memcpy(pcr[i], r.pcr[i], 20);
    unhex(k_t430_image, image, 20);
    n = record((const uint8_t (*)[20])pcr, ek, ek_len);
    assert_int_equal(totpc_merge12(NULL, 0, g_rec, n, image, out, &from,
                                   &bad), TOTPC_OK);
    assert_int_equal(from, TOTPC_FROM_SAVED);
    assert_memory_equal(out, pcr, sizeof pcr);
    free(ek);
}

int run_totpcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_pcrs),
        cmocka_unit_test(test_base32),
        cmocka_unit_test(test_uri),
        cmocka_unit_test(test_owner),
        cmocka_unit_test(test_ini_update),
        cmocka_unit_test(test_merge_t430),
        cmocka_unit_test(test_merge_saved_alone),
    };

    return cmocka_run_group_tests_name("totpcore", tests, NULL, NULL);
}
