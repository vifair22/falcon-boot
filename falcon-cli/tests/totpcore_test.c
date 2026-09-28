/* Tests for falcon-cli/src/mkstick/totpcore.c: the pure half of `station
 * totp` (docs/DESIGN.md Section 13.15).
 *
 * The base32 vectors are RFC 4648 Section 10's; the URI is the Key Uri
 * Format authenticators read, checked whole; the STATION.INI update must
 * leave everything but [totp] byte for byte, and refuse a file that does
 * not parse rather than write over it.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <crypto/sha1.h>
#include <mkstick/totpcore.h>

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

int run_totpcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_pcrs),
        cmocka_unit_test(test_base32),
        cmocka_unit_test(test_uri),
        cmocka_unit_test(test_owner),
        cmocka_unit_test(test_ini_update),
    };

    return cmocka_run_group_tests_name("totpcore", tests, NULL, NULL);
}
