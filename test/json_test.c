/* Tests for src/luks/json.c, the LUKS2 metadata scanner.
 *
 * Threat model first, because it decides what these tests are for. This
 * scanner is the first code in the loader to touch attacker-controlled bytes:
 * 12 KB of JSON out of a USB stick's LUKS2 header, read before a passphrase
 * has been checked and before anything has been authenticated. The header
 * checksum is verified first, but an attacker who rewrote the JSON can rewrite
 * the checksum too, so the scanner has to be safe on arbitrary input rather
 * than on cryptsetup's output. "Safe" here means: never read outside the
 * buffer, never recurse without a bound, and return a clean negative status
 * for anything malformed instead of guessing.
 *
 * So the bulk of this file is refusals and the truncation sweep, not happy
 * paths. The happy paths that do appear are shaped like the real thing: the
 * document in HDR below is a cryptsetup 2.8.7 LUKS2 header verbatim in
 * structure (member names, nesting, which quantities are JSON numbers and
 * which are decimal strings), with the base64 fields replaced by values whose
 * decoding is easy to state.
 *
 * Two documented limits are pinned as tests rather than left implicit, so
 * that a later "fix" has to argue with a failing assertion: object member
 * names are matched as raw bytes (an escaped spelling does not match), and a
 * duplicate member resolves to the first occurrence. Both are fine for
 * cryptsetup's output and both would cost real code to change.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include <luks/json.h>

/* A LUKS2 header's JSON area, structurally identical to what cryptsetup
 * wrote into build/test-luks.img. Note which quantities are numbers and
 * which are decimal strings: the format writes 64-bit offsets and sizes as
 * strings to dodge the double-precision limit of JSON numbers, and a reader
 * that assumed either form would fail on a real header. */
static const char HDR[] =
"{\"keyslots\":{"
  "\"0\":{"
    "\"type\":\"luks2\","
    "\"key_size\":64,"
    "\"af\":{\"type\":\"luks1\",\"stripes\":4000,\"hash\":\"sha256\"},"
    "\"area\":{\"type\":\"raw\",\"offset\":\"32768\",\"size\":\"258048\","
             "\"encryption\":\"aes-xts-plain64\",\"key_size\":64},"
    "\"kdf\":{\"type\":\"pbkdf2\",\"hash\":\"sha256\",\"iterations\":10000,"
            "\"salt\":\"QUJD\"}"
  "},"
  "\"3\":{"
    "\"type\":\"luks2\","
    "\"key_size\":64,"
    "\"af\":{\"type\":\"luks1\",\"stripes\":4000,\"hash\":\"sha512\"},"
    "\"area\":{\"type\":\"raw\",\"offset\":\"290816\",\"size\":\"258048\","
             "\"encryption\":\"aes-xts-plain64\",\"key_size\":64},"
    "\"kdf\":{\"type\":\"argon2id\",\"time\":4,\"memory\":1048576,"
            "\"cpus\":4,\"salt\":\"QUJDRA==\"}"
  "}"
"},"
"\"tokens\":{},"
"\"segments\":{"
  "\"0\":{\"type\":\"crypt\",\"offset\":\"16777216\",\"size\":\"dynamic\","
         "\"iv_tweak\":\"0\",\"encryption\":\"aes-xts-plain64\","
         "\"sector_size\":4096}"
"},"
"\"digests\":{"
  "\"0\":{\"type\":\"pbkdf2\",\"keyslots\":[\"0\",\"3\"],"
         "\"segments\":[\"0\"],\"hash\":\"sha256\",\"iterations\":1000,"
         "\"salt\":\"QQ==\",\"digest\":\"QUJDRUY=\"}"
"},"
"\"config\":{\"json_size\":\"12288\",\"keyslots_size\":\"16744448\"}"
"}";

#define HDR_LEN (sizeof HDR - 1u)

/* ------------------------------------------------------------- helpers */

/* Every embedded snippet is a NUL-free C literal, so its length is its
 * strlen; writing that out at each call site is one more thing to get
 * wrong. */
static void parse_ok(const char *doc, json_value *root)
{
    assert_int_equal(json_parse(doc, strlen(doc), root), JSON_OK);
}

static void assert_str(const json_value *v, const char *want)
{
    assert_int_equal(v->type, JSON_STRING);
    assert_int_equal(json_str_eq(v, want), 1);
}

static uint64_t get_u64(const json_value *root, const char *path)
{
    json_value v;
    uint64_t n = 0;

    assert_int_equal(json_get(root, path, &v), JSON_OK);
    assert_int_equal(json_u64(&v, &n), JSON_OK);
    return n;
}

/* ------------------------------------------------------- the real shape */

static void test_header_paths(void **state)
{
    json_value root, v;

    (void)state;
    parse_ok(HDR, &root);
    assert_int_equal(root.type, JSON_OBJECT);
    assert_int_equal(root.len, HDR_LEN);

    /* The four paths src/luks/luks.c actually walks. */
    assert_int_equal(json_get(&root, "keyslots.0.kdf.type", &v), JSON_OK);
    assert_str(&v, "pbkdf2");
    assert_int_equal(json_get(&root, "keyslots.3.kdf.type", &v), JSON_OK);
    assert_str(&v, "argon2id");
    assert_int_equal(json_get(&root, "keyslots.0.af.hash", &v), JSON_OK);
    assert_str(&v, "sha256");
    assert_int_equal(json_get(&root, "segments.0.encryption", &v), JSON_OK);
    assert_str(&v, "aes-xts-plain64");

    /* A JSON number and a decimal string both reach json_u64. */
    assert_int_equal(get_u64(&root, "keyslots.0.kdf.iterations"), 10000);
    assert_int_equal(get_u64(&root, "keyslots.0.key_size"), 64);
    assert_int_equal(get_u64(&root, "keyslots.0.af.stripes"), 4000);
    assert_int_equal(get_u64(&root, "keyslots.0.area.offset"), 32768);
    assert_int_equal(get_u64(&root, "keyslots.0.area.size"), 258048);
    assert_int_equal(get_u64(&root, "segments.0.sector_size"), 4096);
    assert_true(get_u64(&root, "segments.0.offset") == 16777216ull);
    assert_int_equal(get_u64(&root, "segments.0.iv_tweak"), 0);
    assert_true(get_u64(&root, "config.keyslots_size") == 16744448ull);

    /* "dynamic" is a string where a size would otherwise be, which is how
     * the format spells a segment that runs to the end of the device. It
     * must come back as a string and refuse to be a number. */
    assert_int_equal(json_get(&root, "segments.0.size", &v), JSON_OK);
    assert_str(&v, "dynamic");
    {
        uint64_t n = 0;

        assert_int_equal(json_u64(&v, &n), JSON_ERR_SYNTAX);
    }

    /* An empty object is a legal value and iterating it yields nothing. */
    assert_int_equal(json_get(&root, "tokens", &v), JSON_OK);
    assert_int_equal(v.type, JSON_OBJECT);
    assert_int_equal(v.len, 2);
}

static void test_header_array_indexing(void **state)
{
    json_value root, v;

    (void)state;
    parse_ok(HDR, &root);

    /* A digest names its keyslots and segments as arrays of decimal
     * strings; luks.c indexes them to find which digest verifies which
     * slot. */
    assert_int_equal(json_get(&root, "digests.0.keyslots.0", &v), JSON_OK);
    assert_str(&v, "0");
    assert_int_equal(json_get(&root, "digests.0.keyslots.1", &v), JSON_OK);
    assert_str(&v, "3");
    assert_int_equal(json_get(&root, "digests.0.keyslots.2", &v),
                     JSON_ERR_NOTFOUND);
    assert_int_equal(json_get(&root, "digests.0.segments.0", &v), JSON_OK);
    assert_str(&v, "0");
    assert_int_equal(json_get(&root, "digests.0.segments.1", &v),
                     JSON_ERR_NOTFOUND);

    /* json_arr_get reaches the same elements directly. */
    {
        json_value arr, e;

        assert_int_equal(json_get(&root, "digests.0.keyslots", &arr),
                         JSON_OK);
        assert_int_equal(arr.type, JSON_ARRAY);
        assert_int_equal(json_arr_get(&arr, 0, &e), JSON_OK);
        assert_str(&e, "0");
        assert_int_equal(json_arr_get(&arr, 1, &e), JSON_OK);
        assert_str(&e, "3");
        assert_int_equal(json_arr_get(&arr, 2, &e), JSON_ERR_NOTFOUND);
        assert_int_equal(json_arr_get(&arr, 1000000, &e), JSON_ERR_NOTFOUND);
        assert_int_equal(json_arr_get(NULL, 0, &e), JSON_ERR_BAD_ARG);
        assert_int_equal(json_arr_get(&arr, 0, NULL), JSON_ERR_BAD_ARG);
    }

    /* An object is not an array and must not be indexed as one. */
    {
        json_value obj, e;

        assert_int_equal(json_get(&root, "keyslots", &obj), JSON_OK);
        assert_int_equal(json_arr_get(&obj, 0, &e), JSON_ERR_TYPE);
    }
}

/* Slot ids are object member names, not array indices: cryptsetup writes
 * "0" and can write "3" with nothing between. Iteration is the only way to
 * learn which exist, which is what luks_slot_count does. */
static void test_header_iteration(void **state)
{
    json_value root, keyslots, k, v;
    json_iter it;
    unsigned seen = 0;
    int rc;

    (void)state;
    parse_ok(HDR, &root);
    assert_int_equal(json_get(&root, "keyslots", &keyslots), JSON_OK);
    assert_int_equal(json_iter_init(&keyslots, &it), JSON_OK);

    for (;;) {
        rc = json_iter_next(&it, &k, &v);
        assert_true(rc >= 0);
        if (rc == 0) {
            break;
        }
        assert_int_equal(k.type, JSON_STRING);
        assert_int_equal(v.type, JSON_OBJECT);
        if (seen == 0u) {
            assert_int_equal(json_str_eq(&k, "0"), 1);
        } else {
            assert_int_equal(json_str_eq(&k, "3"), 1);
        }
        seen++;
    }
    assert_int_equal(seen, 2);

    /* Over an array the key is a zero-length JSON_NULL, per json.h. */
    {
        json_value arr;

        assert_int_equal(json_get(&root, "digests.0.keyslots", &arr),
                         JSON_OK);
        assert_int_equal(json_iter_init(&arr, &it), JSON_OK);
        rc = json_iter_next(&it, &k, &v);
        assert_int_equal(rc, 1);
        assert_int_equal(k.type, JSON_NULL);
        assert_int_equal(k.len, 0);
        assert_str(&v, "0");
    }

    /* An empty container ends immediately. */
    {
        json_value tokens;

        assert_int_equal(json_get(&root, "tokens", &tokens), JSON_OK);
        assert_int_equal(json_iter_init(&tokens, &it), JSON_OK);
        assert_int_equal(json_iter_next(&it, &k, &v), 0);
    }

    /* A scalar is not iterable. */
    {
        json_value s;

        assert_int_equal(json_get(&root, "keyslots.0.type", &s), JSON_OK);
        assert_int_equal(json_iter_init(&s, &it), JSON_ERR_TYPE);
        assert_int_equal(json_iter_init(NULL, &it), JSON_ERR_BAD_ARG);
        assert_int_equal(json_iter_init(&keyslots, NULL), JSON_ERR_BAD_ARG);
    }
}

/* --------------------------------------------------------- path errors */

static void test_path_errors(void **state)
{
    json_value root, v;

    (void)state;
    parse_ok(HDR, &root);

    assert_int_equal(json_get(&root, "nosuch", &v), JSON_ERR_NOTFOUND);
    assert_int_equal(json_get(&root, "keyslots.9", &v), JSON_ERR_NOTFOUND);
    assert_int_equal(json_get(&root, "keyslots.0.kdf.nosuch", &v),
                     JSON_ERR_NOTFOUND);

    /* Descending into a scalar is a type error, not "not found": the path
     * named something that exists and is the wrong kind of thing. */
    assert_int_equal(json_get(&root, "keyslots.0.type.x", &v),
                     JSON_ERR_TYPE);
    assert_int_equal(json_get(&root, "keyslots.0.key_size.x", &v),
                     JSON_ERR_TYPE);

    /* A non-numeric component where an array index belongs. */
    assert_int_equal(json_get(&root, "digests.0.keyslots.first", &v),
                     JSON_ERR_BAD_ARG);

    /* Malformed paths. An empty component cannot name anything. */
    assert_int_equal(json_get(&root, "keyslots..0", &v), JSON_ERR_BAD_ARG);
    assert_int_equal(json_get(&root, ".keyslots", &v), JSON_ERR_BAD_ARG);
    assert_int_equal(json_get(&root, "keyslots.", &v), JSON_ERR_BAD_ARG);

    assert_int_equal(json_get(NULL, "keyslots", &v), JSON_ERR_BAD_ARG);
    assert_int_equal(json_get(&root, NULL, &v), JSON_ERR_BAD_ARG);
    assert_int_equal(json_get(&root, "keyslots", NULL), JSON_ERR_BAD_ARG);

    /* The empty path is the identity, which is what makes json_get usable
     * on a value a caller already holds. */
    assert_int_equal(json_get(&root, "", &v), JSON_OK);
    assert_int_equal(v.type, JSON_OBJECT);
    assert_int_equal(v.len, root.len);

    /* json_obj_get's own argument checks. */
    assert_int_equal(json_obj_get(NULL, "a", 1, &v), JSON_ERR_BAD_ARG);
    assert_int_equal(json_obj_get(&root, NULL, 1, &v), JSON_ERR_BAD_ARG);
    assert_int_equal(json_obj_get(&root, "a", 1, NULL), JSON_ERR_BAD_ARG);
    {
        json_value arr;

        assert_int_equal(json_get(&root, "digests.0.keyslots", &arr),
                         JSON_OK);
        assert_int_equal(json_obj_get(&arr, "0", 1, &v), JSON_ERR_TYPE);
    }
}

/* ------------------------------------------------- documented limits */

/* A duplicate member resolves to the first occurrence. Stated in json.h;
 * pinned here so a change has to be deliberate. */
static void test_duplicate_keys_take_the_first(void **state)
{
    static const char doc[] =
        "{\"a\":1,\"b\":2,\"a\":3,\"a\":4}";
    json_value root, v;

    (void)state;
    parse_ok(doc, &root);
    assert_int_equal(get_u64(&root, "a"), 1);
    assert_int_equal(get_u64(&root, "b"), 2);

    /* Iteration still reports every occurrence; only lookup collapses. */
    {
        json_iter it;
        json_value k;
        unsigned n = 0;

        assert_int_equal(json_iter_init(&root, &it), JSON_OK);
        while (json_iter_next(&it, &k, &v) == 1) {
            n++;
        }
        assert_int_equal(n, 4);
    }
}

/* Member names are compared as raw bytes, so an escaped spelling of a name
 * does not match its plain form. json.h calls this out; cryptsetup writes
 * plain ASCII names, so the cost of unescaping every name on every lookup
 * buys nothing. */
static void test_escaped_member_name_does_not_match(void **state)
{
    static const char doc[] = "{\"a\\u0062\":1,\"cd\":2}";
    json_value root, v;

    (void)state;
    parse_ok(doc, &root);

    /* The document is well-formed and the member is there... */
    assert_int_equal(json_get(&root, "cd", &v), JSON_OK);
    /* ...but "ab" spelled with an escape is not found under "ab". */
    assert_int_equal(json_get(&root, "ab", &v), JSON_ERR_NOTFOUND);
    /* It is found under its raw bytes, which is the honest description of
     * what the scanner does. */
    assert_int_equal(json_obj_get(&root, "a\\u0062", 7, &v), JSON_OK);
}

/* ------------------------------------------------------------- numbers */

/* RFC 8259 forbids a leading zero on an integer part. A scanner that
 * accepted "01" would accept documents no other JSON reader does, which is
 * the beginning of a parser-differential bug: cryptsetup's own reader would
 * reject the header this one acted on. Found by fuzzing; pinned here. */
static void test_leading_zero_rejected(void **state)
{
    json_value root;
    size_t i;
    static const char *const bad[] = {
        "{\"a\":01}",
        "{\"a\":00}",
        "{\"a\":-01}",
        "{\"a\":0123}",
        "[00]",
        "01",
        "-00",
        "{\"a\":007}",
    };
    static const char *const good[] = {
        "{\"a\":0}",
        "{\"a\":-0}",
        "{\"a\":0.5}",
        "{\"a\":0e1}",
        "{\"a\":0E-3}",
        "{\"a\":10}",
        "0",
        "-0",
    };

    (void)state;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = json_parse(bad[i], strlen(bad[i]), &root);

        if (rc != JSON_ERR_SYNTAX) {
            fail_msg("leading zero accepted: %s (rc %d)", bad[i], rc);
        }
    }
    for (i = 0; i < sizeof good / sizeof good[0]; i++) {
        int rc = json_parse(good[i], strlen(good[i]), &root);

        if (rc != JSON_OK) {
            fail_msg("valid number rejected: %s (rc %d)", good[i], rc);
        }
    }
}

static void test_json_u64(void **state)
{
    json_value root, v;
    uint64_t n;

    (void)state;

    /* LUKS2 writes 64-bit quantities as decimal strings; both forms parse. */
    parse_ok("{\"num\":4096,\"str\":\"16777216\",\"max\":"
             "\"18446744073709551615\"}", &root);
    assert_int_equal(get_u64(&root, "num"), 4096);
    assert_true(get_u64(&root, "str") == 16777216ull);
    assert_true(get_u64(&root, "max") == UINT64_MAX);

    /* One past the 64-bit range must be refused rather than wrapped: the
     * value becomes a device offset, and a wrapped one would point at the
     * wrong place instead of failing. */
    parse_ok("{\"over\":\"18446744073709551616\"}", &root);
    assert_int_equal(json_get(&root, "over", &v), JSON_OK);
    assert_int_equal(json_u64(&v, &n), JSON_ERR_SYNTAX);

    parse_ok("{\"huge\":\"99999999999999999999999\"}", &root);
    assert_int_equal(json_get(&root, "huge", &v), JSON_OK);
    assert_int_equal(json_u64(&v, &n), JSON_ERR_SYNTAX);

    /* Integer forms only: a negative, fractional or exponent value is not
     * something this reader has any use for, and accepting it would mean
     * silently truncating. */
    parse_ok("{\"neg\":-1,\"frac\":1.5,\"exp\":1e3,\"empty\":\"\","
             "\"junk\":\"12a\",\"space\":\" 12\",\"t\":true}", &root);
    assert_int_equal(json_get(&root, "neg", &v), JSON_OK);
    assert_int_equal(json_u64(&v, &n), JSON_ERR_SYNTAX);
    assert_int_equal(json_get(&root, "frac", &v), JSON_OK);
    assert_int_equal(json_u64(&v, &n), JSON_ERR_SYNTAX);
    assert_int_equal(json_get(&root, "exp", &v), JSON_OK);
    assert_int_equal(json_u64(&v, &n), JSON_ERR_SYNTAX);
    assert_int_equal(json_get(&root, "empty", &v), JSON_OK);
    assert_int_equal(json_u64(&v, &n), JSON_ERR_SYNTAX);
    assert_int_equal(json_get(&root, "junk", &v), JSON_OK);
    assert_int_equal(json_u64(&v, &n), JSON_ERR_SYNTAX);
    assert_int_equal(json_get(&root, "space", &v), JSON_OK);
    assert_int_equal(json_u64(&v, &n), JSON_ERR_SYNTAX);

    /* A bool or a container is the wrong kind of thing entirely. */
    assert_int_equal(json_get(&root, "t", &v), JSON_OK);
    assert_int_equal(json_u64(&v, &n), JSON_ERR_TYPE);
    assert_int_equal(json_u64(&root, &n), JSON_ERR_TYPE);

    assert_int_equal(json_u64(NULL, &n), JSON_ERR_BAD_ARG);
    assert_int_equal(json_u64(&v, NULL), JSON_ERR_BAD_ARG);
}

/* --------------------------------------------------------------- b64 */

static void test_json_b64(void **state)
{
    json_value root, v;
    uint8_t out[64];
    size_t len = 0;

    (void)state;
    /* RFC 4648 vectors, which is what a LUKS2 salt or digest field is. */
    parse_ok("{\"a\":\"\",\"b\":\"Zg==\",\"c\":\"Zm8=\",\"d\":\"Zm9v\","
             "\"e\":\"Zm9vYg==\",\"f\":\"Zm9vYmE=\",\"g\":\"Zm9vYmFy\","
             "\"h\":\"+/8=\"}", &root);

    assert_int_equal(json_get(&root, "a", &v), JSON_OK);
    assert_int_equal(json_b64(&v, out, sizeof out, &len), JSON_OK);
    assert_int_equal(len, 0);

    assert_int_equal(json_get(&root, "b", &v), JSON_OK);
    assert_int_equal(json_b64(&v, out, sizeof out, &len), JSON_OK);
    assert_int_equal(len, 1);
    assert_memory_equal(out, "f", 1);

    assert_int_equal(json_get(&root, "c", &v), JSON_OK);
    assert_int_equal(json_b64(&v, out, sizeof out, &len), JSON_OK);
    assert_int_equal(len, 2);
    assert_memory_equal(out, "fo", 2);

    assert_int_equal(json_get(&root, "d", &v), JSON_OK);
    assert_int_equal(json_b64(&v, out, sizeof out, &len), JSON_OK);
    assert_int_equal(len, 3);
    assert_memory_equal(out, "foo", 3);

    assert_int_equal(json_get(&root, "e", &v), JSON_OK);
    assert_int_equal(json_b64(&v, out, sizeof out, &len), JSON_OK);
    assert_int_equal(len, 4);
    assert_memory_equal(out, "foob", 4);

    assert_int_equal(json_get(&root, "f", &v), JSON_OK);
    assert_int_equal(json_b64(&v, out, sizeof out, &len), JSON_OK);
    assert_int_equal(len, 5);
    assert_memory_equal(out, "fooba", 5);

    assert_int_equal(json_get(&root, "g", &v), JSON_OK);
    assert_int_equal(json_b64(&v, out, sizeof out, &len), JSON_OK);
    assert_int_equal(len, 6);
    assert_memory_equal(out, "foobar", 6);

    /* The standard alphabet's last two characters, which the URL-safe
     * variant spells differently. The format uses the standard one. */
    assert_int_equal(json_get(&root, "h", &v), JSON_OK);
    assert_int_equal(json_b64(&v, out, sizeof out, &len), JSON_OK);
    assert_int_equal(len, 2);
    assert_int_equal(out[0], 0xfb);
    assert_int_equal(out[1], 0xff);

    /* out_len may be NULL. */
    assert_int_equal(json_b64(&v, out, sizeof out, NULL), JSON_OK);
}

static void test_json_b64_rejects(void **state)
{
    json_value root, v;
    uint8_t out[64];
    size_t len = 0;
    size_t i;
    static const char *const bad[] = {
        "{\"x\":\"Zm9vY\"}",      /* length not a multiple of 4 */
        "{\"x\":\"Z\"}",
        "{\"x\":\"Zm9vYmFyZ\"}",
        "{\"x\":\"Zm-v\"}",       /* URL-safe alphabet */
        "{\"x\":\"Zm_v\"}",
        "{\"x\":\"Zm v\"}",       /* embedded whitespace */
        "{\"x\":\"=m9v\"}",       /* padding out of position */
        "{\"x\":\"Z=9v\"}",
        "{\"x\":\"====\"}",
        "{\"x\":\"Zm9=Zm9v\"}",   /* data after padding */
        "{\"x\":\"Zm9v!!!!\"}",
    };

    (void)state;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc;

        parse_ok(bad[i], &root);
        assert_int_equal(json_get(&root, "x", &v), JSON_OK);
        rc = json_b64(&v, out, sizeof out, &len);
        if (rc != JSON_ERR_SYNTAX) {
            fail_msg("bad base64 accepted: %s (rc %d)", bad[i], rc);
        }
    }

    /* A short output buffer must be refused, not overrun: the salt and
     * digest buffers in luks.c are fixed-size and the header names the
     * length. */
    parse_ok("{\"x\":\"Zm9vYmFy\"}", &root);
    assert_int_equal(json_get(&root, "x", &v), JSON_OK);
    assert_int_equal(json_b64(&v, out, 5, &len), JSON_ERR_NOSPACE);
    assert_int_equal(json_b64(&v, out, 0, &len), JSON_ERR_NOSPACE);
    assert_int_equal(json_b64(&v, out, 6, &len), JSON_OK);

    /* A number is not a base64 string. */
    parse_ok("{\"n\":5}", &root);
    assert_int_equal(json_get(&root, "n", &v), JSON_OK);
    assert_int_equal(json_b64(&v, out, sizeof out, &len), JSON_ERR_TYPE);
    assert_int_equal(json_b64(NULL, out, sizeof out, &len),
                     JSON_ERR_BAD_ARG);
    assert_int_equal(json_b64(&v, NULL, sizeof out, &len), JSON_ERR_BAD_ARG);
}

/* ------------------------------------------------------------ strings */

static void test_json_str(void **state)
{
    json_value root, v;
    char buf[64];
    size_t len = 0;

    (void)state;
    parse_ok("{\"plain\":\"hello\","
             "\"esc\":\"a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti\","
             "\"uni\":\"\\u0041\\u00e9\\u20ac\","
             /* Upper-case hex digits, which hex_val handles on a separate
              * branch from lower-case and nothing else here reaches. */
             "\"UNI\":\"\\u00E9\\u20AC\","
             "\"pair\":\"\\ud83d\\ude00\","
             "\"lone\":\"\\ud800x\"}", &root);

    assert_int_equal(json_get(&root, "plain", &v), JSON_OK);
    assert_int_equal(json_str(&v, buf, sizeof buf, &len), JSON_OK);
    assert_int_equal(len, 5);
    assert_string_equal(buf, "hello");

    assert_int_equal(json_get(&root, "esc", &v), JSON_OK);
    assert_int_equal(json_str(&v, buf, sizeof buf, &len), JSON_OK);
    assert_int_equal(len, 17);
    assert_memory_equal(buf, "a\"b\\c/d\be\ff\ng\rh\ti", 17);

    /* ASCII, two-byte and three-byte UTF-8 from \uXXXX. */
    assert_int_equal(json_get(&root, "uni", &v), JSON_OK);
    assert_int_equal(json_str(&v, buf, sizeof buf, &len), JSON_OK);
    assert_int_equal(len, 6);
    assert_memory_equal(buf, "A\xc3\xa9\xe2\x82\xac", 6);

    /* The same two code points spelled with upper-case hex. */
    assert_int_equal(json_get(&root, "UNI", &v), JSON_OK);
    assert_int_equal(json_str(&v, buf, sizeof buf, &len), JSON_OK);
    assert_int_equal(len, 5);
    assert_memory_equal(buf, "\xc3\xa9\xe2\x82\xac", 5);

    /* A surrogate pair becomes one four-byte sequence. */
    assert_int_equal(json_get(&root, "pair", &v), JSON_OK);
    assert_int_equal(json_str(&v, buf, sizeof buf, &len), JSON_OK);
    assert_int_equal(len, 4);
    assert_memory_equal(buf, "\xf0\x9f\x98\x80", 4);

    /* An unpaired surrogate becomes U+FFFD rather than aborting: a
     * malformed escape in a field the loader does not interpret must not
     * fail the unlock. */
    assert_int_equal(json_get(&root, "lone", &v), JSON_OK);
    assert_int_equal(json_str(&v, buf, sizeof buf, &len), JSON_OK);
    assert_int_equal(len, 4);
    assert_memory_equal(buf, "\xef\xbf\xbdx", 4);
}

static void test_json_str_space_and_type(void **state)
{
    json_value root, v;
    char buf[64];

    (void)state;
    parse_ok("{\"s\":\"hello\",\"u\":\"\\u20ac\",\"n\":1}", &root);

    assert_int_equal(json_get(&root, "s", &v), JSON_OK);
    /* Exactly enough is five bytes plus the terminator. */
    assert_int_equal(json_str(&v, buf, 6, NULL), JSON_OK);
    assert_string_equal(buf, "hello");
    assert_int_equal(json_str(&v, buf, 5, NULL), JSON_ERR_NOSPACE);
    assert_int_equal(json_str(&v, buf, 1, NULL), JSON_ERR_NOSPACE);
    assert_int_equal(json_str(&v, buf, 0, NULL), JSON_ERR_BAD_ARG);
    assert_int_equal(json_str(&v, NULL, sizeof buf, NULL), JSON_ERR_BAD_ARG);
    assert_int_equal(json_str(NULL, buf, sizeof buf, NULL), JSON_ERR_BAD_ARG);

    /* A three-byte sequence needs three bytes plus the terminator, and the
     * check is made before any of them is written. */
    assert_int_equal(json_get(&root, "u", &v), JSON_OK);
    assert_int_equal(json_str(&v, buf, 4, NULL), JSON_OK);
    assert_int_equal(json_str(&v, buf, 3, NULL), JSON_ERR_NOSPACE);

    assert_int_equal(json_get(&root, "n", &v), JSON_OK);
    assert_int_equal(json_str(&v, buf, sizeof buf, NULL), JSON_ERR_TYPE);
    assert_int_equal(json_str(&root, buf, sizeof buf, NULL), JSON_ERR_TYPE);
}

static void test_json_str_eq(void **state)
{
    json_value root, v;

    (void)state;
    parse_ok("{\"s\":\"pbkdf2\",\"n\":1,\"e\":\"\"}", &root);

    assert_int_equal(json_get(&root, "s", &v), JSON_OK);
    assert_int_equal(json_str_eq(&v, "pbkdf2"), 1);
    assert_int_equal(json_str_eq(&v, "pbkdf"), 0);
    assert_int_equal(json_str_eq(&v, "pbkdf22"), 0);
    assert_int_equal(json_str_eq(&v, "PBKDF2"), 0);
    assert_int_equal(json_str_eq(&v, ""), 0);
    assert_int_equal(json_str_eq(&v, NULL), 0);
    assert_int_equal(json_str_eq(NULL, "pbkdf2"), 0);

    assert_int_equal(json_get(&root, "e", &v), JSON_OK);
    assert_int_equal(json_str_eq(&v, ""), 1);
    assert_int_equal(json_str_eq(&v, "x"), 0);

    /* A number never equals a string, whatever it spells. */
    assert_int_equal(json_get(&root, "n", &v), JSON_OK);
    assert_int_equal(json_str_eq(&v, "1"), 0);
}

/* --------------------------------------------------- structural refusals */

static void test_parse_refusals(void **state)
{
    json_value root;
    size_t i;
    static const char *const bad[] = {
        "",                    /* nothing at all */
        "   ",
        "{",
        "}",
        "[",
        "]",
        "{\"a\"}",             /* member with no value */
        "{\"a\":}",
        "{\"a\":1,}",          /* trailing comma */
        "[1,]",
        "{,}",
        "{\"a\" 1}",           /* missing colon */
        "{a:1}",               /* unquoted name */
        "{'a':1}",
        "[1 2]",               /* missing comma */
        "\"unterminated",
        "\"bad\\escape\"",
        "\"\\u12\"",           /* short unicode escape */
        "\"\\uZZZZ\"",
        "tru",
        "nul",
        "truex",               /* trailing junk after a literal */
        "{\"a\":1} extra",
        "[1][2]",
        "1.",                  /* incomplete number forms */
        "1e",
        "1e+",
        ".5",
        "-",
        "+1",
        "{\"a\":\"b\"} \t\n x",
    };

    (void)state;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = json_parse(bad[i], strlen(bad[i]), &root);

        if (rc >= 0) {
            fail_msg("malformed document accepted: \"%s\"", bad[i]);
        }
    }

    /* An unescaped control character inside a string is forbidden by the
     * grammar and is exactly what a corrupted header would contain. */
    assert_int_equal(json_parse("{\"a\":\"x\ny\"}", 11, &root),
                     JSON_ERR_SYNTAX);
    assert_int_equal(json_parse("{\"a\":\"x\tz\"}", 11, &root),
                     JSON_ERR_SYNTAX);

    assert_int_equal(json_parse(NULL, 4, &root), JSON_ERR_BAD_ARG);
    assert_int_equal(json_parse("null", 4, NULL), JSON_ERR_BAD_ARG);
}

static void test_parse_accepts(void **state)
{
    json_value root;

    (void)state;
    /* Scalars at the root, and the whitespace the grammar allows around
     * them. Nothing in a LUKS2 header looks like this, but the scanner is
     * general and a value_at that mishandled a root scalar would mishandle
     * the same value nested. */
    assert_int_equal(json_parse("null", 4, &root), JSON_OK);
    assert_int_equal(root.type, JSON_NULL);
    assert_int_equal(json_parse("true", 4, &root), JSON_OK);
    assert_int_equal(root.type, JSON_BOOL);
    assert_int_equal(json_parse("false", 5, &root), JSON_OK);
    assert_int_equal(root.type, JSON_BOOL);
    assert_int_equal(json_parse(" \t\r\n 42 \n", 9, &root), JSON_OK);
    assert_int_equal(root.type, JSON_NUMBER);
    assert_int_equal(root.len, 2);
    assert_int_equal(json_parse("[]", 2, &root), JSON_OK);
    assert_int_equal(root.type, JSON_ARRAY);
    assert_int_equal(json_parse("{}", 2, &root), JSON_OK);
    assert_int_equal(root.type, JSON_OBJECT);

    /* A root string's span is the bytes between the quotes, not the quoted
     * text: json.h defines it that way so json_str never has to re-strip. */
    assert_int_equal(json_parse("\"abc\"", 5, &root), JSON_OK);
    assert_int_equal(root.type, JSON_STRING);
    assert_int_equal(root.len, 3);
    assert_memory_equal(root.ptr, "abc", 3);
}

/* JSON_MAX_DEPTH is a hard bound and not a suggestion: the scanner's stack
 * frame is that many bytes and nothing else grows with the input, which is
 * what keeps a hostile header off the end of the firmware's stack. */
static void test_depth_cap(void **state)
{
    char doc[2u * (JSON_MAX_DEPTH + 4u) + 1u];
    json_value root;
    unsigned d;

    (void)state;
    for (d = 1; d <= JSON_MAX_DEPTH + 2u; d++) {
        unsigned i;
        size_t n = 0;
        int rc;

        for (i = 0; i < d; i++) {
            doc[n++] = '[';
        }
        for (i = 0; i < d; i++) {
            doc[n++] = ']';
        }
        rc = json_parse(doc, n, &root);
        if (d <= JSON_MAX_DEPTH) {
            if (rc != JSON_OK) {
                fail_msg("depth %u rejected (rc %d)", d, rc);
            }
        } else if (rc != JSON_ERR_DEPTH) {
            fail_msg("depth %u not capped (rc %d)", d, rc);
        }
    }

    /* Objects nest through the same stack. */
    {
        char obj[16u * (JSON_MAX_DEPTH + 2u)];
        size_t n = 0;
        unsigned i;

        for (i = 0; i < JSON_MAX_DEPTH + 1u; i++) {
            memcpy(obj + n, "{\"a\":", 5);
            n += 5;
        }
        obj[n++] = '1';
        for (i = 0; i < JSON_MAX_DEPTH + 1u; i++) {
            obj[n++] = '}';
        }
        assert_int_equal(json_parse(obj, n, &root), JSON_ERR_DEPTH);
    }
}

/* The sweep that matters most. Every prefix of a real-shaped header must
 * come back as a clean negative status and must not read past the length it
 * was given: a truncated or overwritten JSON area is the single most likely
 * form of corruption on a stick that was unplugged mid-write.
 *
 * The document is copied into an exactly-sized heap-free buffer for each
 * prefix so that a read one byte past the end lands on whatever follows in
 * the stack frame rather than on the rest of a valid document, which is what
 * makes the check meaningful under ASAN as well as plain.
 *
 * Silent by construction: this runs HDR_LEN times and printing per iteration
 * would bury the rest of the suite. */
static void test_truncation_sweep(void **state)
{
    static uint8_t guard[HDR_LEN + 8u];
    json_value root;
    size_t cut;

    (void)state;
    for (cut = 0; cut < HDR_LEN; cut++) {
        int rc;

        memset(guard, 0x5a, sizeof guard);
        memcpy(guard, HDR, cut);
        rc = json_parse((const char *)guard, cut, &root);
        if (rc >= 0) {
            fail_msg("truncation at %zu parsed as valid", cut);
        }
        if (rc != JSON_ERR_SYNTAX && rc != JSON_ERR_BAD_ARG) {
            fail_msg("truncation at %zu gave rc %d, want a syntax error",
                     cut, rc);
        }
    }
    /* The untruncated document still parses, so the sweep above was not
     * passing because the buffer was broken some other way. */
    memcpy(guard, HDR, HDR_LEN);
    assert_int_equal(json_parse((const char *)guard, HDR_LEN, &root),
                     JSON_OK);
}

/* The complement of the sweep: single-byte corruptions of a valid document.
 * Most produce a syntax error, some produce a different but still
 * well-formed document; neither may crash and neither may return a positive
 * status. Every byte position is tried with a handful of replacements. */
static void test_single_byte_corruption(void **state)
{
    static char buf[HDR_LEN + 1u];
    static const char repl[] = { '\0', '"', '{', '}', '[', ']', ':', ',',
                                 '\\', 'x', (char)0x7f, (char)0x01 };
    json_value root;
    size_t at;
    size_t r;

    (void)state;
    for (at = 0; at < HDR_LEN; at++) {
        for (r = 0; r < sizeof repl / sizeof repl[0]; r++) {
            int rc;

            memcpy(buf, HDR, HDR_LEN);
            buf[at] = repl[r];
            rc = json_parse(buf, HDR_LEN, &root);
            /* Only OK or a negative status is allowed; a positive return
             * would mean the contract has been broken. */
            if (rc > 0) {
                fail_msg("corruption at %zu (0x%02x) returned %d",
                         at, (unsigned)(unsigned char)repl[r], rc);
            }
            if (rc == JSON_OK) {
                /* If it still parses, walking it must also be safe. */
                json_value v;

                (void)json_get(&root, "keyslots.0.kdf.type", &v);
                (void)json_get(&root, "segments.0.offset", &v);
            }
        }
    }
}

/* ------------------------------------------------------------------ run */


/* The refusals the corruption sweep cannot reach, because the LUKS2 metadata
 * it sweeps contains no backslash and no base64 that survives a flip in the
 * one shape needed. */
static void test_json_directed_refusals(void **state)
{
    json_value root;
    json_value v;
    char small[2];
    uint8_t out[8];
    size_t n = 0;

    (void)state;

    /* A backslash as the last byte of the buffer. string_span has to notice
     * that the escape it just started has nothing after it rather than read
     * the byte past the end. */
    assert_int_equal(json_parse("\"\\", 2, &root), JSON_ERR_SYNTAX);

    /* Base64 with a data character after the padding. The position check
     * allows '=' in the last two slots of a group, so "AB=C" gets past it
     * and the run has to be refused for being out of order instead. */
    assert_int_equal(json_parse("\"AB=C\"", 6, &root), JSON_OK);
    assert_int_equal(json_b64(&root, out, sizeof out, &n), JSON_ERR_SYNTAX);

    /* An output buffer too small for the unescaped string, including its
     * terminator. */
    assert_int_equal(json_parse("\"hello\"", 7, &root), JSON_OK);
    assert_int_equal(json_str(&root, small, sizeof small, &n),
                     JSON_ERR_NOSPACE);

    /* And the same through a member, so the caller's cap is honoured
     * wherever the value came from. */
    parse_ok("{\"k\":\"a longer value\"}", &root);
    assert_int_equal(json_get(&root, "k", &v), JSON_OK);
    assert_int_equal(json_str(&v, small, sizeof small, &n),
                     JSON_ERR_NOSPACE);

    /* Running out of room on an ESCAPE rather than on a plain character.
     * Separate branch, separate bound: the plain path checks before copying
     * one byte, this one before copying the byte an escape decoded to. */
    assert_int_equal(json_parse("\"\\n\\n\"", 6, &root), JSON_OK);
    assert_int_equal(json_str(&root, small, sizeof small, &n),
                     JSON_ERR_NOSPACE);

    /* A \\uXXXX that decodes to more bytes than are left, which is the same
     * bound again inside utf8_put: one code point can be four bytes. */
    assert_int_equal(json_parse("\"\\u20ac\"", 8, &root), JSON_OK);
    assert_int_equal(json_str(&root, small, sizeof small, &n),
                     JSON_ERR_NOSPACE);
}

int run_json_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_header_paths),
        cmocka_unit_test(test_header_array_indexing),
        cmocka_unit_test(test_header_iteration),
        cmocka_unit_test(test_path_errors),
        cmocka_unit_test(test_duplicate_keys_take_the_first),
        cmocka_unit_test(test_escaped_member_name_does_not_match),
        cmocka_unit_test(test_leading_zero_rejected),
        cmocka_unit_test(test_json_u64),
        cmocka_unit_test(test_json_b64),
        cmocka_unit_test(test_json_b64_rejects),
        cmocka_unit_test(test_json_str),
        cmocka_unit_test(test_json_str_space_and_type),
        cmocka_unit_test(test_json_str_eq),
        cmocka_unit_test(test_parse_refusals),
        cmocka_unit_test(test_parse_accepts),
        cmocka_unit_test(test_depth_cap),
        cmocka_unit_test(test_truncation_sweep),
        cmocka_unit_test(test_single_byte_corruption),
        cmocka_unit_test(test_json_directed_refusals),
    };

    return cmocka_run_group_tests_name("luks json", tests, NULL, NULL);
}
