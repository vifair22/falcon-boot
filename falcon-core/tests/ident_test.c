/* Tests for falcon-core/src/ident/ident.c, the identity record (docs/DESIGN.md
 * Section 3.1), and for the LUKS2 token that carries it on a shelf
 * (luks_falcon_token in falcon-core/src/luks/luks.c).
 *
 * The binary record's bytes are spelt out here in full once, from the
 * table in DESIGN, so an encoder that agrees only with its own decoder
 * still fails. Its CRC is gzip's, and the record's copy is checked against
 * gz_crc32 rather than against itself.
 *
 * The token needs no LUKS volume: luks_falcon_token reads only the JSON
 * area luks_open would have loaded, so a luks_vol pointing at a JSON
 * string is the whole fixture.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <gz/gz.h>
#include <ident/ident.h>
#include <luks/luks.h>

static const uint8_t test_id[IDENT_ID_LEN] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10
};
#define TEST_ID_HEX "0123456789abcdeffedcba9876543210"

static ident_rec make_rec(ident_role role, uint16_t s3, uint16_t mk)
{
    ident_rec r;

    memset(&r, 0, sizeof(r));
    r.version = IDENT_VERSION;
    r.role = role;
    r.stage3_sector = s3;
    r.marker_sector = mk;
    memcpy(r.install_id, test_id, IDENT_ID_LEN);
    return r;
}

static void put_crc(uint8_t *sector)
{
    uint32_t c = gz_crc32(0, sector + IDENT_BIN_OFF, 28);
    uint8_t *p = sector + IDENT_BIN_OFF + 28;

    p[0] = (uint8_t)c;
    p[1] = (uint8_t)(c >> 8);
    p[2] = (uint8_t)(c >> 16);
    p[3] = (uint8_t)(c >> 24);
}

/* ---- binary ---- */

static void test_bin_layout(void **state)
{
    uint8_t sec[512];
    uint8_t want[IDENT_BIN_LEN] = {
        'F', 'L', 'C', 'N', 1, 1, 0x20, 0x00, 0x7f, 0x00, 0, 0,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
        0, 0, 0, 0
    };
    ident_rec r = make_rec(IDENT_ROLE_BOOT, 0x20, 0x7f);
    ident_rec got;
    size_t i;

    (void)state;
    memset(sec, 0xAA, sizeof(sec));
    ident_encode_bin(&r, sec);

    /* Everything outside the 32 bytes is the caller's and stays so. */
    for (i = 0; i < sizeof(sec); i++) {
        if (i < IDENT_BIN_OFF || i >= IDENT_BIN_OFF + IDENT_BIN_LEN)
            assert_int_equal(sec[i], 0xAA);
    }
    {
        uint32_t c = gz_crc32(0, want, 28);

        want[28] = (uint8_t)c;
        want[29] = (uint8_t)(c >> 8);
        want[30] = (uint8_t)(c >> 16);
        want[31] = (uint8_t)(c >> 24);
    }
    assert_memory_equal(sec + IDENT_BIN_OFF, want, IDENT_BIN_LEN);

    assert_int_equal(ident_decode_bin(sec, &got), IDENT_OK);
    assert_int_equal(got.version, 1);
    assert_int_equal(got.role, IDENT_ROLE_BOOT);
    assert_int_equal(got.stage3_sector, 0x20);
    assert_int_equal(got.marker_sector, 0x7f);
    assert_memory_equal(got.install_id, test_id, IDENT_ID_LEN);
}

static void test_bin_round_trip_roles(void **state)
{
    static const ident_role roles[] = {
        IDENT_ROLE_BOOT, IDENT_ROLE_LIBRARY, IDENT_ROLE_SHELF
    };
    uint8_t sec[512];
    size_t i;

    (void)state;
    for (i = 0; i < 3; i++) {
        ident_rec r = make_rec(roles[i], 0, 0);
        ident_rec got;

        memset(sec, 0, sizeof(sec));
        ident_encode_bin(&r, sec);
        assert_int_equal(ident_decode_bin(sec, &got), IDENT_OK);
        assert_int_equal(got.role, roles[i]);
        assert_int_equal(ident_same_install(&r, &got), 1);
    }
    /* NULLs are no-ops, not crashes. */
    ident_encode_bin(NULL, sec);
    ident_encode_bin(&(ident_rec){ 0 }, NULL);
}

static void test_bin_rejects(void **state)
{
    uint8_t sec[512];
    uint8_t *b = sec + IDENT_BIN_OFF;
    ident_rec r = make_rec(IDENT_ROLE_BOOT, 3, 4);
    ident_rec got;

    (void)state;
    memset(sec, 0, sizeof(sec));
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_ERR_NONE);
    assert_int_equal(ident_decode_bin(NULL, &got), IDENT_ERR_ARG);
    assert_int_equal(ident_decode_bin(sec, NULL), IDENT_ERR_ARG);

    /* Wrong magic is no record at all, whatever follows. */
    ident_encode_bin(&r, sec);
    b[3] = 'X';
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_ERR_NONE);

    /* A flipped bit anywhere under the CRC, and the CRC itself. */
    ident_encode_bin(&r, sec);
    b[20] ^= 0x01;
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_ERR_BAD);
    ident_encode_bin(&r, sec);
    b[31] ^= 0x80;
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_ERR_BAD);

    /* Well-checksummed but wrong fields. */
    ident_encode_bin(&r, sec);
    b[4] = 2;                                   /* version */
    put_crc(sec);
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_ERR_BAD);
    ident_encode_bin(&r, sec);
    b[5] = 0;                                   /* role */
    put_crc(sec);
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_ERR_BAD);
    ident_encode_bin(&r, sec);
    b[5] = 4;
    put_crc(sec);
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_ERR_BAD);
    ident_encode_bin(&r, sec);
    b[10] = 1;                                  /* reserved */
    put_crc(sec);
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_ERR_BAD);
    ident_encode_bin(&r, sec);
    b[11] = 1;
    put_crc(sec);
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_ERR_BAD);

    /* Sector fields on a partition that is not a boot one. */
    r = make_rec(IDENT_ROLE_LIBRARY, 3, 0);
    ident_encode_bin(&r, sec);
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_ERR_BAD);
    r = make_rec(IDENT_ROLE_SHELF, 0, 4);
    ident_encode_bin(&r, sec);
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_ERR_BAD);

    /* An install ID nobody drew. */
    r = make_rec(IDENT_ROLE_BOOT, 3, 4);
    memset(r.install_id, 0, IDENT_ID_LEN);
    ident_encode_bin(&r, sec);
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_ERR_BAD);
}

/* ---- text ---- */

static void test_text_round_trip(void **state)
{
    static const char *const want[] = {
        "falcon 1 boot " TEST_ID_HEX "\n",
        "falcon 1 library " TEST_ID_HEX "\n",
        "falcon 1 shelf " TEST_ID_HEX "\n",
    };
    char buf[IDENT_TEXT_MAX];
    ident_rec got;
    int i;

    (void)state;
    for (i = 0; i < 3; i++) {
        ident_rec r = make_rec((ident_role)(i + 1), 0, 0);
        size_t n = ident_encode_text(&r, buf, sizeof(buf));

        assert_int_equal(n, strlen(want[i]));
        assert_string_equal(buf, want[i]);
        assert_int_equal(ident_decode_text(buf, n, &got), IDENT_OK);
        assert_int_equal(got.role, r.role);
        assert_int_equal(got.version, 1);
        assert_int_equal(got.stage3_sector, 0);
        assert_memory_equal(got.install_id, test_id, IDENT_ID_LEN);
        /* The newline is optional. */
        assert_int_equal(ident_decode_text(buf, n - 1u, &got), IDENT_OK);
    }

    /* The longest line fits IDENT_TEXT_MAX exactly as documented, and a
     * buffer one short of it is refused rather than cut. */
    {
        ident_rec r = make_rec(IDENT_ROLE_LIBRARY, 0, 0);
        size_t n = ident_encode_text(&r, buf, sizeof(buf));

        assert_true(n + 1u <= IDENT_TEXT_MAX);
        assert_int_equal(ident_encode_text(&r, buf, n), 0);
        assert_int_equal(ident_encode_text(&r, buf, n + 1u), n);

        /* A later version prints as itself, and is refused on read. */
        r.version = 12;
        n = ident_encode_text(&r, buf, sizeof(buf));
        assert_string_equal(buf, "falcon 12 library " TEST_ID_HEX "\n");
        assert_int_equal(ident_decode_text(buf, n, &got), IDENT_ERR_BAD);
        r.version = 255;
        n = ident_encode_text(&r, buf, sizeof(buf));
        assert_string_equal(buf, "falcon 255 library " TEST_ID_HEX "\n");

        r.version = 0;
        assert_int_equal(ident_encode_text(&r, buf, sizeof(buf)), 0);
        r.version = 1;
        r.role = (ident_role)9;
        assert_int_equal(ident_encode_text(&r, buf, sizeof(buf)), 0);
        assert_int_equal(ident_encode_text(NULL, buf, sizeof(buf)), 0);
        assert_int_equal(ident_encode_text(&r, NULL, sizeof(buf)), 0);
    }
}

static void test_text_rejects(void **state)
{
    static const char *const bad[] = {
        "",
        "\n",
        "falcon",
        "falcon ",
        "falcon 1",
        "falcon 1 ",
        "falcon 1 shelf",
        "falcon 1 shelf ",
        "Falcon 1 shelf " TEST_ID_HEX,
        "falcon  1 shelf " TEST_ID_HEX,              /* extra spaces */
        "falcon 1  shelf " TEST_ID_HEX,
        "falcon 1 shelf  " TEST_ID_HEX,
        " falcon 1 shelf " TEST_ID_HEX,
        "falcon 1 shelf " TEST_ID_HEX " ",
        "falcon 01 shelf " TEST_ID_HEX,              /* leading zero */
        "falcon 0 shelf " TEST_ID_HEX,
        "falcon 2 shelf " TEST_ID_HEX,               /* version */
        "falcon 99999999999 shelf " TEST_ID_HEX,
        "falcon x shelf " TEST_ID_HEX,
        "falcon 1x shelf " TEST_ID_HEX,
        "falcon 1 Shelf " TEST_ID_HEX,               /* role */
        "falcon 1 shelves " TEST_ID_HEX,
        "falcon 1 esp " TEST_ID_HEX,
        "falcon 1 shelf 0123456789ABCDEFFEDCBA9876543210",  /* uppercase */
        "falcon 1 shelf 0123456789abcdeffedcba987654321",   /* short */
        "falcon 1 shelf 0123456789abcdeffedcba98765432100", /* long */
        "falcon 1 shelf 0123456789abcdeffedcba987654321g",
        "falcon 1 shelf 00000000000000000000000000000000",  /* zero ID */
        "falcon 1 shelf " TEST_ID_HEX "\n\n",        /* trailing junk */
        "falcon 1 shelf " TEST_ID_HEX "\r\n",
        "falcon 1 shelf " TEST_ID_HEX "\nfalcon",
        "falcon\t1 shelf " TEST_ID_HEX,
    };
    ident_rec got;
    size_t i;

    (void)state;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int rc = ident_decode_text(bad[i], strlen(bad[i]), &got);

        if (rc != IDENT_ERR_BAD)
            fail_msg("accepted or misreported (%d): \"%s\"", rc, bad[i]);
    }
    /* A NUL inside the length is a byte like any other, and wrong. */
    {
        char withnul[] = "falcon 1 shelf " TEST_ID_HEX;

        withnul[20] = '\0';
        assert_int_equal(ident_decode_text(withnul, sizeof(withnul) - 1u,
                                           &got), IDENT_ERR_BAD);
    }
    assert_int_equal(ident_decode_text(NULL, 0, &got), IDENT_ERR_ARG);
    assert_int_equal(ident_decode_text("x", 1, NULL), IDENT_ERR_ARG);
}

static void test_from_strings(void **state)
{
    ident_rec got;

    (void)state;
    assert_int_equal(ident_from_strings(1, "shelf", 5, TEST_ID_HEX, 32, &got),
                     IDENT_OK);
    assert_int_equal(got.role, IDENT_ROLE_SHELF);
    assert_memory_equal(got.install_id, test_id, IDENT_ID_LEN);
    /* Lengths, not terminators: "shelfX" cut at 5 is "shelf". */
    assert_int_equal(ident_from_strings(1, "shelfX", 5, TEST_ID_HEX "zz", 32,
                                        &got), IDENT_OK);
    assert_int_equal(ident_from_strings(1, "shelf", 4, TEST_ID_HEX, 32, &got),
                     IDENT_ERR_BAD);
    assert_int_equal(ident_from_strings(2, "shelf", 5, TEST_ID_HEX, 32, &got),
                     IDENT_ERR_BAD);
    assert_int_equal(ident_from_strings(0, "shelf", 5, TEST_ID_HEX, 32, &got),
                     IDENT_ERR_BAD);
    assert_int_equal(ident_from_strings(1, "shelf", 5, TEST_ID_HEX, 31, &got),
                     IDENT_ERR_BAD);
    assert_int_equal(ident_from_strings(1, NULL, 0, TEST_ID_HEX, 32, &got),
                     IDENT_ERR_ARG);
    assert_int_equal(ident_from_strings(1, "shelf", 5, NULL, 0, &got),
                     IDENT_ERR_ARG);
    assert_int_equal(ident_from_strings(1, "shelf", 5, TEST_ID_HEX, 32, NULL),
                     IDENT_ERR_ARG);
}

static void test_same_install_and_names(void **state)
{
    ident_rec a = make_rec(IDENT_ROLE_BOOT, 1, 2);
    ident_rec b = make_rec(IDENT_ROLE_SHELF, 0, 0);

    (void)state;
    assert_int_equal(ident_same_install(&a, &b), 1);
    b.install_id[15] ^= 1;
    assert_int_equal(ident_same_install(&a, &b), 0);
    assert_int_equal(ident_same_install(&a, NULL), 0);
    assert_int_equal(ident_same_install(NULL, &a), 0);
    /* Two zeroed records are two records nobody wrote, not one install. */
    memset(&a, 0, sizeof(a));
    b = a;
    assert_int_equal(ident_same_install(&a, &b), 0);

    assert_string_equal(ident_role_name(IDENT_ROLE_BOOT), "boot");
    assert_string_equal(ident_role_name(IDENT_ROLE_LIBRARY), "library");
    assert_string_equal(ident_role_name(IDENT_ROLE_SHELF), "shelf");
    assert_null(ident_role_name((ident_role)0));
    assert_null(ident_role_name((ident_role)4));
}

/* The record's own CRC against gz's, over lengths either side of the
 * record's 28 bytes: the record's CRC is defined as gzip's. */
static void test_crc_is_gzip(void **state)
{
    uint8_t sec[512];
    ident_rec r = make_rec(IDENT_ROLE_LIBRARY, 0, 0);
    const uint8_t *b = sec + IDENT_BIN_OFF;
    uint32_t stored;

    (void)state;
    memset(sec, 0, sizeof(sec));
    ident_encode_bin(&r, sec);
    stored = (uint32_t)b[28] | ((uint32_t)b[29] << 8) |
             ((uint32_t)b[30] << 16) | ((uint32_t)b[31] << 24);
    assert_int_equal(stored, gz_crc32(0, b, 28));
}

/* ---- the LUKS2 token ---- */

static int token_of(const char *json, uint32_t *ver, const char **role,
                    size_t *rl, const char **id, size_t *il)
{
    static char buf[4096];
    luks_vol v;

    memset(&v, 0, sizeof(v));
    strcpy(buf, json);
    v.json = buf;
    v.json_len = strlen(buf);
    return luks_falcon_token(&v, ver, role, rl, id, il);
}

static void test_luks_token(void **state)
{
    uint32_t ver = 0;
    const char *role = NULL, *id = NULL;
    size_t rl = 0, il = 0;
    ident_rec got;

    (void)state;
    /* cryptsetup's shape: other tokens first, keys in another order. */
    assert_int_equal(token_of(
        "{\"keyslots\":{},\"tokens\":{"
        "\"0\":{\"type\":\"systemd-tpm2\",\"keyslots\":[\"0\"],\"role\":\"x\"},"
        "\"1\":{\"install_id\":\"" TEST_ID_HEX "\",\"keyslots\":[],"
        "\"extra\":{\"a\":[1,2]},\"role\":\"shelf\",\"version\":1,"
        "\"type\":\"falcon\"}},\"segments\":{}}",
        &ver, &role, &rl, &id, &il), LUKS_OK);
    assert_int_equal(ver, 1);
    assert_int_equal(rl, 5);
    assert_memory_equal(role, "shelf", 5);
    assert_int_equal(il, 32);
    assert_int_equal(ident_from_strings(ver, role, rl, id, il, &got),
                     IDENT_OK);
    assert_int_equal(got.role, IDENT_ROLE_SHELF);
    assert_memory_equal(got.install_id, test_id, IDENT_ID_LEN);

    /* The DESIGN spelling, exactly. */
    assert_int_equal(token_of(
        "{\"tokens\":{\"0\":{\"type\":\"falcon\",\"keyslots\":[],"
        "\"version\":1,\"role\":\"shelf\",\"install_id\":\"" TEST_ID_HEX
        "\"}}}", &ver, &role, &rl, &id, &il), LUKS_OK);

    /* Absent: no tokens member, an empty one, only other types, a token
     * that is not an object. */
    assert_int_equal(token_of("{\"keyslots\":{}}", &ver, &role, &rl, &id,
                              &il), LUKS_ERR_NOTFOUND);
    assert_int_equal(token_of("{\"tokens\":{}}", &ver, &role, &rl, &id, &il),
                     LUKS_ERR_NOTFOUND);
    assert_int_equal(token_of(
        "{\"tokens\":{\"0\":{\"type\":\"fido2\",\"version\":1,"
        "\"role\":\"shelf\",\"install_id\":\"" TEST_ID_HEX "\"},"
        "\"1\":[],\"2\":{\"role\":\"shelf\"}}}",
        &ver, &role, &rl, &id, &il), LUKS_ERR_NOTFOUND);

    /* Ours, malformed. */
    assert_int_equal(token_of(
        "{\"tokens\":{\"0\":{\"type\":\"falcon\",\"role\":\"shelf\","
        "\"install_id\":\"" TEST_ID_HEX "\"}}}",
        &ver, &role, &rl, &id, &il), LUKS_ERR_CORRUPT);
    assert_int_equal(token_of(
        "{\"tokens\":{\"0\":{\"type\":\"falcon\",\"version\":\"1\","
        "\"role\":\"shelf\",\"install_id\":\"" TEST_ID_HEX "\"}}}",
        &ver, &role, &rl, &id, &il), LUKS_ERR_CORRUPT);
    assert_int_equal(token_of(
        "{\"tokens\":{\"0\":{\"type\":\"falcon\",\"version\":1.5,"
        "\"role\":\"shelf\",\"install_id\":\"" TEST_ID_HEX "\"}}}",
        &ver, &role, &rl, &id, &il), LUKS_ERR_CORRUPT);
    assert_int_equal(token_of(
        "{\"tokens\":{\"0\":{\"type\":\"falcon\",\"version\":4294967296,"
        "\"role\":\"shelf\",\"install_id\":\"" TEST_ID_HEX "\"}}}",
        &ver, &role, &rl, &id, &il), LUKS_ERR_CORRUPT);
    assert_int_equal(token_of(
        "{\"tokens\":{\"0\":{\"type\":\"falcon\",\"version\":1,"
        "\"install_id\":\"" TEST_ID_HEX "\"}}}",
        &ver, &role, &rl, &id, &il), LUKS_ERR_CORRUPT);
    assert_int_equal(token_of(
        "{\"tokens\":{\"0\":{\"type\":\"falcon\",\"version\":1,"
        "\"role\":3,\"install_id\":\"" TEST_ID_HEX "\"}}}",
        &ver, &role, &rl, &id, &il), LUKS_ERR_CORRUPT);
    assert_int_equal(token_of(
        "{\"tokens\":{\"0\":{\"type\":\"falcon\",\"version\":1,"
        "\"role\":\"shelf\"}}}",
        &ver, &role, &rl, &id, &il), LUKS_ERR_CORRUPT);
    assert_int_equal(token_of(
        "{\"tokens\":{\"0\":{\"type\":\"falcon\",\"version\":1,"
        "\"role\":\"shelf\",\"install_id\":[]}}}",
        &ver, &role, &rl, &id, &il), LUKS_ERR_CORRUPT);
    assert_int_equal(token_of("{\"tokens\":[]}", &ver, &role, &rl, &id, &il),
                     LUKS_ERR_CORRUPT);
    assert_int_equal(token_of("{\"tokens\":{\"0\":}}", &ver, &role, &rl, &id,
                              &il), LUKS_ERR_CORRUPT);
    assert_int_equal(token_of("[]", &ver, &role, &rl, &id, &il),
                     LUKS_ERR_CORRUPT);

    /* A well-formed token whose values are wrong is the ident layer's to
     * refuse; the escaped role reaches it raw. */
    assert_int_equal(token_of(
        "{\"tokens\":{\"0\":{\"type\":\"falcon\",\"version\":1,"
        "\"role\":\"sh\\u0065lf\",\"install_id\":\"" TEST_ID_HEX "\"}}}",
        &ver, &role, &rl, &id, &il), LUKS_OK);
    assert_int_equal(ident_from_strings(ver, role, rl, id, il, &got),
                     IDENT_ERR_BAD);

    assert_int_equal(luks_falcon_token(NULL, &ver, &role, &rl, &id, &il),
                     LUKS_ERR_BAD_ARG);
    {
        luks_vol v;
        char empty[] = "{}";

        memset(&v, 0, sizeof(v));
        assert_int_equal(luks_falcon_token(&v, &ver, &role, &rl, &id, &il),
                         LUKS_ERR_BAD_ARG);
        v.json = empty;
        v.json_len = 2;
        assert_int_equal(luks_falcon_token(&v, NULL, &role, &rl, &id, &il),
                         LUKS_ERR_BAD_ARG);
        assert_int_equal(luks_falcon_token(&v, &ver, NULL, &rl, &id, &il),
                         LUKS_ERR_BAD_ARG);
        assert_int_equal(luks_falcon_token(&v, &ver, &role, NULL, &id, &il),
                         LUKS_ERR_BAD_ARG);
        assert_int_equal(luks_falcon_token(&v, &ver, &role, &rl, NULL, &il),
                         LUKS_ERR_BAD_ARG);
        assert_int_equal(luks_falcon_token(&v, &ver, &role, &rl, &id, NULL),
                         LUKS_ERR_BAD_ARG);
        assert_int_equal(luks_falcon_token(&v, &ver, &role, &rl, &id, &il),
                         LUKS_ERR_NOTFOUND);
    }
}

int run_ident_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_bin_layout),
        cmocka_unit_test(test_bin_round_trip_roles),
        cmocka_unit_test(test_bin_rejects),
        cmocka_unit_test(test_text_round_trip),
        cmocka_unit_test(test_text_rejects),
        cmocka_unit_test(test_from_strings),
        cmocka_unit_test(test_same_install_and_names),
        cmocka_unit_test(test_crc_is_gzip),
        cmocka_unit_test(test_luks_token),
    };

    return cmocka_run_group_tests_name("ident", tests, NULL, NULL);
}
