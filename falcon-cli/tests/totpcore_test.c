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

static uint8_t *slurp_in(const char *dir, const char *name, size_t *len)
{
    char path[512];
    uint8_t *buf;
    FILE *f;
    long n;

    snprintf(path, sizeof path, "%s/%s/%s", CLI_TEST_DATA, dir, name);
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

static uint8_t *slurp(const char *name, size_t *len)
{
    return slurp_in("t430", name, len);
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

/* ---- the TPM 2.0 record, on the B550's log ---- */

/* PCRs 0-15 as the TPM held them in the boot evlog.bin is from. */
static const char *const k_b550_pcr[16] = {
    "17e02db14b5cb35635190e866a10d5696c7a1d5465769e4fe3aeb8515d773f99",
    "9cb4a990df60ef742a249060b3a22075084db20b11b357d03112ca79dc2a1261",
    "cc44e1bbe025d05a27eb0309772b960dc3f67fb547d520decf356bcaf3300ec7",
    "8d22c738fcd1730fb0789cb09c0f72a0012baa3ea867723b26772c9ca0ae6571",
    "09e4bdbc09ec7708ff549fa6f06896f9bfd6bab915b98356af71a3d5d7b50332",
    "d2ad27e6a6e5a6983b75f2e66b3e152632dcb757dc3d4736173029a85322df5c",
    "8d22c738fcd1730fb0789cb09c0f72a0012baa3ea867723b26772c9ca0ae6571",
    "dcbf6c347d3b0d4ed964238e4d97da337e30ef0c66553b1e9834c5fe5804158a",
    "0000000000000000000000000000000000000000000000000000000000000000",
    "f9937ba6c56bd71f48e0e8aac8c6d6d427f82d20333785a61b8be81be0b06df3",
    "b4ed9cf9ad04b04b1305f0e6c4779d6b22d54d05d3a95a4bc4cca512b37d134b",
    "7beda3f22c4e069fc374f1acf8753a4327da75f1fcf154c916f4cc44abb88655",
    "21679e8ea8da4e4a5a148205aa08bf7a6568fd9b07607877647b6119986d848b",
    "3345a4e7857aa5ae65e97702ade84a3755fd6144724779536b5773128676c99c",
    "62816aa88f122531a1c5514c5ad4935d3e243db12a794189cc77bb87d5b23550",
    "62465c0a72bfbe12bea245c80b82a074ca5102b31f4f031552bd2ad00ac9c77f",
};

/* The station's SHA-256 Authenticode digest, PCR 4's second application. */
static const char k_b550_station[] =
    "3d8e399c34ff43838d1758aa5fcf7b3fc09f72345433fdacd5cc087d9e9ba5e7";

#define B550_MASK 0x94u     /* PCRs 2, 4 and 7 */

/* The station started without shim (README.md): the TPM as it stood after
 * the boot, and the station's digest, PCR 4's first application. */
static const char *const k_b550_direct_pcr[16] = {
    "17e02db14b5cb35635190e866a10d5696c7a1d5465769e4fe3aeb8515d773f99",
    "5f51b2da472140f3f36228fcf211953122e30149561033649371844dce9b855c",
    "cc44e1bbe025d05a27eb0309772b960dc3f67fb547d520decf356bcaf3300ec7",
    "8d22c738fcd1730fb0789cb09c0f72a0012baa3ea867723b26772c9ca0ae6571",
    "a076d6cef20c508631975fea64ad96edb22d69e48573ea24a6c252df39d7438d",
    "d2ad27e6a6e5a6983b75f2e66b3e152632dcb757dc3d4736173029a85322df5c",
    "8d22c738fcd1730fb0789cb09c0f72a0012baa3ea867723b26772c9ca0ae6571",
    "5d5526f0246817eeb62ba746a2b8522cea2c018dc11418c054f39335d1a26d05",
    "0000000000000000000000000000000000000000000000000000000000000000",
    "ca4382ad02c70424fbed2e81b1eb0d9acc228b7d86ca99a434a2e1dd9ef4bfcc",
    "5cf3e7a62612df12dd2e2072610d3587545837c4bafc9d9a8bd2c404d54288bb",
    "7beda3f22c4e069fc374f1acf8753a4327da75f1fcf154c916f4cc44abb88655",
    "21679e8ea8da4e4a5a148205aa08bf7a6568fd9b07607877647b6119986d848b",
    "3345a4e7857aa5ae65e97702ade84a3755fd6144724779536b5773128676c99c",
    "3345a4e7857aa5ae65e97702ade84a3755fd6144724779536b5773128676c99c",
    "62465c0a72bfbe12bea245c80b82a074ca5102b31f4f031552bd2ad00ac9c77f",
};
static const char k_b550_direct_station[] =
    "af44a2d64b3f2e13f9f951f1170aeda7f0ff5665fbd593343b189cad8ff0bfbd";

static uint8_t g_rec2[EVSAVE_BODY2];

static size_t record2(const uint8_t pcr[16][32])
{
    size_t n = 0;

    assert_int_equal(evsave_write2(g_rec2, sizeof g_rec2, pcr, &n),
                     EVSAVE_OK);
    return n;
}

/* The firmware extends PCRs 0 to 7 after the station's menu without logging
 * it, so none of the station's record is what the TPM holds by the time
 * enrollment runs. The log, replayed to the station's measurement, gives
 * every PCR of the record, and that pins the record to this boot. */
static void test_station20_pinned(void **state)
{
    static uint8_t live[16][32];
    static uint8_t out[16][32];
    static uint8_t rec[EVSAVE_BODY2];
    uint8_t station[32];
    size_t len = 0, rlen = 0;
    uint8_t *log = slurp_in("b550", "evlog-direct.bin", &len);
    uint8_t *saved = slurp_in("b550", "record-direct.bin", &rlen);
    evsave_view v;
    unsigned bad = 99;
    int whole = -1;
    int pinned = -1;
    unsigned i;

    (void)state;
    for (i = 0; i < 16u; i++)
        unhex(k_b550_direct_pcr[i], live[i], 32);
    unhex(k_b550_direct_station, station, 32);
    assert_true(rlen <= sizeof rec);
    memcpy(rec, saved, rlen);
    assert_int_equal(evsave_read(rec, rlen, &v), EVSAVE_OK);
    assert_int_equal(v.version, 2);

    /* Every PCR the station read differs from the TPM's now. */
    for (i = 0; i < 8u; i++)
        assert_memory_not_equal(v.pcr + (size_t)i * 32u, live[i], 32);

    assert_int_equal(totpc_station20(rec, rlen, log, len, station,
                                     (const uint8_t (*)[32])live, B550_MASK,
                                     out, &whole, &pinned, &bad), TOTPC_OK);
    assert_int_equal(whole, 0);
    assert_int_equal(pinned, 1);
    assert_memory_equal(out, v.pcr, sizeof out);

    /* A record the log does not pin is held against the TPM, which it
     * fails here at the first stable PCR. */
    rec[EVSAVE_HDR + 2u * EVSAVE_DIGEST_LEN2] ^= 1u;
    assert_int_equal(totpc_station20(rec, rlen, log, len, station,
                                     (const uint8_t (*)[32])live, B550_MASK,
                                     out, &whole, &pinned, &bad), TOTPC_ERR_BOOT);
    assert_int_equal(pinned, 0);
    assert_int_equal(bad, 0);
    free(saved);
    free(log);
}

/* The firmware extends PCRs it does not log: the log is set aside and the
 * station's record stands, as long as it is of this boot. */
static void test_station20_unlogged(void **state)
{
    static uint8_t live[16][32];
    static uint8_t rec[16][32];
    static uint8_t out[16][32];
    uint8_t station[32];
    size_t len = 0;
    uint8_t *log = slurp_in("b550", "evlog.bin", &len);
    unsigned bad = 99;
    int whole = -1;
    int pinned = -1;
    size_t n;
    unsigned i;

    (void)state;
    for (i = 0; i < 16u; i++)
        unhex(k_b550_pcr[i], live[i], 32);
    unhex(k_b550_station, station, 32);

    /* The station's reading: this boot's stable PCRs, and its own 4 and 7,
     * which the OS has moved on since. */
    memcpy(rec, live, sizeof rec);
    memset(rec[4], 0x44, 32);
    memset(rec[7], 0x77, 32);
    n = record2((const uint8_t (*)[32])rec);
    assert_int_equal(totpc_station20(g_rec2, n, log, len, station,
                                     (const uint8_t (*)[32])live, B550_MASK,
                                     out, &whole, &pinned, &bad), TOTPC_OK);
    assert_int_equal(whole, 0);
    assert_memory_equal(out, rec, sizeof rec);

    /* PCR 1 moved after the station (the UKI's stub extends it), so the
     * record's PCR 1 is not held against the TPM's. */
    memset(rec[1], 0x11, 32);
    n = record2((const uint8_t (*)[32])rec);
    assert_int_equal(totpc_station20(g_rec2, n, log, len, station,
                                     (const uint8_t (*)[32])live, B550_MASK,
                                     out, &whole, &pinned, &bad), TOTPC_OK);

    /* A stable PCR that is not this boot's. */
    rec[3][0] ^= 1u;
    n = record2((const uint8_t (*)[32])rec);
    assert_int_equal(totpc_station20(g_rec2, n, log, len, station,
                                     (const uint8_t (*)[32])live, B550_MASK,
                                     out, &whole, &pinned, &bad), TOTPC_ERR_BOOT);
    assert_int_equal(bad, 3);
    rec[3][0] ^= 1u;
    n = record2((const uint8_t (*)[32])rec);

    /* No measurement of this station in the log. */
    station[0] ^= 1u;
    assert_int_equal(totpc_station20(g_rec2, n, log, len, station,
                                     (const uint8_t (*)[32])live, B550_MASK,
                                     out, &whole, &pinned, &bad), TOTPC_ERR_STATION);
    station[0] ^= 1u;

    /* A version 1 record, a broken one, and arguments. */
    {
        static uint8_t pcr1[16][20];
        size_t n1 = record((const uint8_t (*)[20])pcr1, log, 16);

        assert_int_equal(totpc_station20(g_rec, n1, log, len, station,
                                         (const uint8_t (*)[32])live,
                                         B550_MASK, out, &whole, &pinned, &bad),
                         TOTPC_ERR_LOG);
    }
    assert_int_equal(totpc_station20(g_rec2, n - 1u, log, len, station,
                                     (const uint8_t (*)[32])live, B550_MASK,
                                     out, &whole, &pinned, &bad), TOTPC_ERR_LOG);
    assert_int_equal(totpc_station20(g_rec2, n, log, len, station, NULL,
                                     1u << 16, out, &whole, &pinned, &bad),
                     TOTPC_ERR_ARG);
    assert_int_equal(totpc_station20(NULL, n, log, len, station, NULL,
                                     B550_MASK, out, &whole, &pinned, &bad),
                     TOTPC_ERR_ARG);
    free(log);
}

/* A log that accounts for the TPM (live is its own replay here): the
 * station's record must be what the log gives up to its measurement. */
static void test_station20_whole(void **state)
{
    static evlog_replay all;
    static evlog_replay rep;
    static uint8_t live[16][32];
    static uint8_t rec[16][32];
    static uint8_t out[16][32];
    uint8_t station[32];
    size_t len = 0;
    uint8_t *log = slurp_in("b550", "evlog.bin", &len);
    unsigned bad = 99;
    int whole = -1;
    int pinned = -1;
    size_t n;
    unsigned i;

    (void)state;
    unhex(k_b550_station, station, 32);
    assert_int_equal(evlog_replay_run(log, len, EVLOG_ALG_SHA256, 0, NULL,
                                      &all), EVLOG_OK);
    assert_int_equal(evlog_replay_run(log, len, EVLOG_ALG_SHA256, 4, station,
                                      &rep), EVLOG_OK);
    assert_true(rep.stopped);
    for (i = 0; i < 16u; i++) {
        memcpy(live[i], all.pcr[i], 32);
        memcpy(rec[i], rep.pcr[i], 32);
    }
    n = record2((const uint8_t (*)[32])rec);
    assert_int_equal(totpc_station20(g_rec2, n, log, len, station,
                                     (const uint8_t (*)[32])live, B550_MASK,
                                     out, &whole, &pinned, &bad), TOTPC_OK);
    assert_int_equal(whole, 1);
    assert_memory_equal(out, rec, sizeof rec);

    /* Without the live TPM (the rig's swtpm) the log is taken as whole. */
    assert_int_equal(totpc_station20(g_rec2, n, log, len, station, NULL,
                                     B550_MASK, out, &whole, &pinned, &bad), TOTPC_OK);
    assert_int_equal(whole, 1);

    /* The record and a whole log disagree on PCR 7. */
    rec[7][0] ^= 1u;
    n = record2((const uint8_t (*)[32])rec);
    assert_int_equal(totpc_station20(g_rec2, n, log, len, station,
                                     (const uint8_t (*)[32])live, B550_MASK,
                                     out, &whole, &pinned, &bad), TOTPC_ERR_PCR);
    assert_int_equal(bad, 7);

    /* A PCR outside the mask may differ. */
    rec[7][0] ^= 1u;
    rec[5][0] ^= 1u;
    n = record2((const uint8_t (*)[32])rec);
    assert_int_equal(totpc_station20(g_rec2, n, log, len, station,
                                     (const uint8_t (*)[32])live, B550_MASK,
                                     out, &whole, &pinned, &bad), TOTPC_OK);
    free(log);
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
        cmocka_unit_test(test_station20_unlogged),
        cmocka_unit_test(test_station20_pinned),
        cmocka_unit_test(test_station20_whole),
    };

    return cmocka_run_group_tests_name("totpcore", tests, NULL, NULL);
}
