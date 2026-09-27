/* Tests for falcon-core/src/media/nextboot.c: the next-boot marker's value
 * rules, which both loaders act on, and the BIOS sector that carries a value
 * where there is no variable store (docs/DESIGN.md Section 6.3.1).
 *
 * Both carriers are writable by anything that can reach them - NVRAM by
 * anything with boot services on the machine, the sector by anything that
 * can open the disk - so every hostile shape below has to end at "no
 * marker", and the value is only ever compared against paths the survey
 * found.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <media/nextboot.h>

#define WIN11_PATH "Windows/Win11_25H2_x64.iso"

static void widen(uint16_t *dst, size_t cap, const char *src)
{
    size_t i = 0;

    assert_true(cap > 0);
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = (uint16_t)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

/* ------------------------------------------------------------- the value */

static void test_decode_plain(void **state)
{
    char out[NEXTBOOT_MAX];

    (void)state;
    /* With the terminator, which is what the writers put there. */
    assert_int_equal(nextboot_decode(WIN11_PATH, sizeof WIN11_PATH, out,
                                     sizeof out), NEXTBOOT_OK);
    assert_string_equal(out, WIN11_PATH);

    /* And without it, which is what a variable editor is likely to write. */
    assert_int_equal(nextboot_decode(WIN11_PATH, strlen(WIN11_PATH), out,
                                     sizeof out), NEXTBOOT_OK);
    assert_string_equal(out, WIN11_PATH);
}

static void test_decode_empty(void **state)
{
    char out[NEXTBOOT_MAX];

    (void)state;
    assert_int_equal(nextboot_decode("", 0, out, sizeof out),
                     NEXTBOOT_ERR_VALUE);
    assert_string_equal(out, "");
    /* A lone terminator is an empty value, not a path to anything. */
    assert_int_equal(nextboot_decode("", 1, out, sizeof out),
                     NEXTBOOT_ERR_VALUE);
    assert_string_equal(out, "");
}

static void test_decode_oversized(void **state)
{
    char big[NEXTBOOT_MAX + 64];
    char out[NEXTBOOT_MAX];

    (void)state;
    memset(big, 'a', sizeof big);
    assert_int_equal(nextboot_decode(big, sizeof big, out, sizeof out),
                     NEXTBOOT_ERR_VALUE);
    assert_string_equal(out, "");
    /* Exactly one character past what fits, terminator included. */
    assert_int_equal(nextboot_decode(big, NEXTBOOT_MAX, out, sizeof out),
                     NEXTBOOT_ERR_VALUE);
    /* And the longest value that does fit. */
    assert_int_equal(nextboot_decode(big, NEXTBOOT_MAX - 1u, out,
                                     sizeof out), NEXTBOOT_OK);
    assert_int_equal(strlen(out), NEXTBOOT_MAX - 1u);
}

/* Bytes outside printable ASCII cannot name a record: manifest_parse drops
 * any record carrying one, so a value with one has nothing to match. */
static void test_decode_non_ascii(void **state)
{
    static const unsigned char utf8[] = { 'W', 'i', 'n', 0xC3, 0xA9, '.',
                                          'i', 's', 'o', 0 };
    static const unsigned char ctrl[] = { 'a', '\n', 'b', 0 };
    static const unsigned char tab[] = { 'a', '\t', 'b', 0 };
    char out[NEXTBOOT_MAX];

    (void)state;
    assert_int_equal(nextboot_decode(utf8, sizeof utf8, out, sizeof out),
                     NEXTBOOT_ERR_VALUE);
    assert_int_equal(nextboot_decode(ctrl, sizeof ctrl, out, sizeof out),
                     NEXTBOOT_ERR_VALUE);
    assert_int_equal(nextboot_decode(tab, sizeof tab, out, sizeof out),
                     NEXTBOOT_ERR_VALUE);
    assert_string_equal(out, "");
}

/* An embedded NUL is refused rather than taken as a terminator, so no two
 * readers of the same value can disagree about where it ends. */
static void test_decode_embedded_nul(void **state)
{
    static const char split[] = "a.iso\0b.iso";
    char out[NEXTBOOT_MAX];

    (void)state;
    assert_int_equal(nextboot_decode(split, sizeof split - 1u, out,
                                     sizeof out), NEXTBOOT_ERR_VALUE);
    assert_string_equal(out, "");
}

static void test_decode_null_args(void **state)
{
    char out[NEXTBOOT_MAX];

    (void)state;
    assert_int_equal(nextboot_decode(NULL, 4, out, sizeof out),
                     NEXTBOOT_ERR_VALUE);
    assert_int_equal(nextboot_decode("x", 1, NULL, sizeof out),
                     NEXTBOOT_ERR_BAD_ARG);
    assert_int_equal(nextboot_decode("x", 1, out, 0), NEXTBOOT_ERR_BAD_ARG);
}

/* The value is only ever compared against a path the survey found, and the
 * comparison is the manifest's own: case-insensitive, either separator. */
static void test_names(void **state)
{
    uint16_t wide[512];

    (void)state;
    widen(wide, sizeof(wide) / sizeof(wide[0]), WIN11_PATH);
    assert_true(nextboot_names(wide, WIN11_PATH));
    assert_true(nextboot_names(wide, "windows/win11_25h2_x64.iso"));
    assert_true(nextboot_names(wide, "/Windows/Win11_25H2_x64.iso"));
    assert_false(nextboot_names(wide, "Windows/Win10.iso"));
    assert_false(nextboot_names(wide, ""));
    assert_false(nextboot_names(wide, NULL));
    assert_false(nextboot_names(NULL, WIN11_PATH));

    /* An exFAT path arrives backslash-separated on a stick written by
     * Windows; it names the same file. */
    widen(wide, sizeof(wide) / sizeof(wide[0]),
          "Windows\\Win11_25H2_x64.iso");
    assert_true(nextboot_names(wide, WIN11_PATH));
}

/* A path no record could carry cannot be named by a marker either, and
 * answering that without searching is what keeps the comparison total. */
static void test_names_non_ascii_path(void **state)
{
    uint16_t wide[8];

    (void)state;
    wide[0] = (uint16_t)'a';
    wide[1] = 0x00E9u;   /* e-acute: no manifest record may carry it */
    wide[2] = 0;
    assert_false(nextboot_names(wide, "a?"));
}

/* The marker names an entry; booting it is then the menu's own decision, so
 * the rule here is only whether the decoded value names the entry in hand. */
static int names_entry(const char *marker, const char *path)
{
    char decoded[NEXTBOOT_MAX];
    uint16_t wide[512];

    if (nextboot_decode(marker, strlen(marker) + 1u, decoded,
                        sizeof decoded) != NEXTBOOT_OK)
        return 0;
    widen(wide, sizeof(wide) / sizeof(wide[0]), path);
    return nextboot_names(wide, decoded);
}

static void test_rule(void **state)
{
    (void)state;
    assert_true(names_entry(WIN11_PATH, WIN11_PATH));
    assert_true(names_entry("Linux/arch.iso", "Linux/arch.iso"));
    assert_false(names_entry("Windows/Win10.iso", WIN11_PATH));
    assert_false(names_entry("", WIN11_PATH));
    assert_false(names_entry("../../etc/shadow", WIN11_PATH));
}

/* The reserved namespace. ':' is the load-bearing character: exFAT forbids it
 * in a filename, so no path the survey builds can ever land in here. */
static void test_reserved_personal(void **state)
{
    (void)state;
    assert_true(nextboot_is_reserved(NEXTBOOT_PERSONAL));
    assert_true(nextboot_is_personal(NEXTBOOT_PERSONAL));
    assert_true(nextboot_is_reserved(":whatever"));
    assert_false(nextboot_is_personal(":whatever"));
}

/* Ordinary paths are not reserved, and neither is the '@personal' spelling
 * that was considered and rejected: a file may legally be called that. */
static void test_reserved_rejects_paths(void **state)
{
    (void)state;
    assert_false(nextboot_is_reserved(WIN11_PATH));
    assert_false(nextboot_is_reserved("Linux/arch.iso"));
    assert_false(nextboot_is_reserved("@personal"));
    assert_false(nextboot_is_personal("@personal"));
    assert_false(nextboot_is_reserved(""));
    assert_false(nextboot_is_reserved(NULL));
    assert_false(nextboot_is_personal(NULL));
}

/* Exact match, not a prefix, so the namespace can grow without silently
 * rebinding older markers. */
static void test_personal_is_exact(void **state)
{
    (void)state;
    assert_false(nextboot_is_personal(":personalish"));
    assert_false(nextboot_is_personal(":persona"));
    assert_false(nextboot_is_personal(":"));
}

/* ------------------------------------------------------- the BIOS sector */

/* A sector as archusb-boot-next writes it: printf's text, zeros to 512. */
static void sector_from_text(uint8_t *sec, const char *text)
{
    memset(sec, 0, NEXTBOOT_SECTOR_BYTES);
    memcpy(sec, text, strlen(text));
}

static void test_sector_round_trip(void **state)
{
    uint8_t sec[NEXTBOOT_SECTOR_BYTES];
    uint8_t want[NEXTBOOT_SECTOR_BYTES];
    char out[NEXTBOOT_MAX];
    uint64_t stamp = 0;

    (void)state;
    assert_int_equal(nextboot_sector_write(sec, sizeof sec, 1790000000u,
                                           WIN11_PATH), NEXTBOOT_OK);
    /* Byte for byte what the shell writer produces. */
    sector_from_text(want, "falcon-next-boot 1\n1790000000\n" WIN11_PATH "\n");
    assert_memory_equal(sec, want, sizeof sec);

    assert_true(nextboot_sector_is_marker(sec, sizeof sec));
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                           sizeof out),
                     NEXTBOOT_SECTOR_VALUE);
    assert_int_equal(stamp, 1790000000u);
    assert_string_equal(out, WIN11_PATH);
}

static void test_sector_personal_and_zero_stamp(void **state)
{
    uint8_t sec[NEXTBOOT_SECTOR_BYTES];
    char out[NEXTBOOT_MAX];
    uint64_t stamp = 99;

    (void)state;
    assert_int_equal(nextboot_sector_write(sec, sizeof sec, 0u,
                                           NEXTBOOT_PERSONAL), NEXTBOOT_OK);
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                           sizeof out),
                     NEXTBOOT_SECTOR_VALUE);
    assert_int_equal(stamp, 0u);
    assert_string_equal(out, NEXTBOOT_PERSONAL);
}

/* The prefix is the one thing that makes a sector the loader's to write: a
 * zeroed gap, a stage, or a stranger's loader is NONE and is left alone. */
static void test_sector_not_a_marker(void **state)
{
    uint8_t sec[NEXTBOOT_SECTOR_BYTES];
    char out[NEXTBOOT_MAX];
    uint64_t stamp = 0;

    (void)state;
    memset(sec, 0, sizeof sec);
    assert_false(nextboot_sector_is_marker(sec, sizeof sec));
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                           sizeof out), NEXTBOOT_SECTOR_NONE);

    sector_from_text(sec, "falcon-next-boo 1\n1\nx\n");
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                           sizeof out), NEXTBOOT_SECTOR_NONE);
    sector_from_text(sec, "FALCON-NEXT-BOOT 1\n1\nx\n");
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                           sizeof out), NEXTBOOT_SECTOR_NONE);
    memset(sec, 0xFF, sizeof sec);
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                           sizeof out), NEXTBOOT_SECTOR_NONE);

    /* The wrong length is not a sector at all. */
    sector_from_text(sec, "falcon-next-boot 1\n1\nx\n");
    assert_int_equal(nextboot_sector_parse(sec, 256u, &stamp, out,
                                           sizeof out), NEXTBOOT_SECTOR_NONE);
    assert_int_equal(nextboot_sector_parse(NULL, sizeof sec, &stamp, out,
                                           sizeof out), NEXTBOOT_SECTOR_NONE);
}

/* A marker by its magic and wrong everywhere else: BAD, which the loader
 * clears and does not act on. */
static void test_sector_bad_shapes(void **state)
{
    static const char *const bad[] = {
        "falcon-next-boot 2\n1\nx\n",             /* a later version */
        "falcon-next-boot 1 \n1\nx\n",            /* not exactly the version */
        "falcon-next-boot 1\n\nx\n",              /* no stamp */
        "falcon-next-boot 1\n12a\nx\n",           /* not digits */
        "falcon-next-boot 1\n-5\nx\n",            /* a sign */
        "falcon-next-boot 1\n123456789012345678901\nx\n", /* 21 digits */
        "falcon-next-boot 1\n99999999999999999999\nx\n",  /* overflows */
        "falcon-next-boot 1\n1\n\n",              /* empty value */
        "falcon-next-boot 1\n1\nx",               /* no final newline */
        "falcon-next-boot 1\n1",                  /* no value line */
        "falcon-next-boot 1\n1\na\tb\n",          /* not printable */
        "falcon-next-boot 1\n1\nx\ny\n",          /* a line after the value */
        "falcon-next-boot 1\n1\nx\n\n",           /* even an empty one */
    };
    uint8_t sec[NEXTBOOT_SECTOR_BYTES];
    char out[NEXTBOOT_MAX];
    uint64_t stamp = 7;
    size_t i;

    (void)state;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        sector_from_text(sec, bad[i]);
        assert_true(nextboot_sector_is_marker(sec, sizeof sec));
        assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                               sizeof out),
                         NEXTBOOT_SECTOR_BAD);
        assert_string_equal(out, "");
        assert_int_equal(stamp, 7u);
    }

    /* A stray byte at the very end of the sector, which a torn or careless
     * write could leave. */
    sector_from_text(sec, "falcon-next-boot 1\n1\nx\n");
    sec[NEXTBOOT_SECTOR_BYTES - 1u] = 0x01u;
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                           sizeof out), NEXTBOOT_SECTOR_BAD);
    assert_string_equal(out, "");
}

/* The largest stamp that fits and a value at the path cap, the longest
 * record the format allows, still parse. */
static void test_sector_limits(void **state)
{
    uint8_t sec[NEXTBOOT_SECTOR_BYTES];
    char value[NEXTBOOT_MAX];
    char out[NEXTBOOT_MAX];
    uint64_t stamp = 0;

    (void)state;
    memset(value, 'a', sizeof value - 1u);
    value[sizeof value - 1u] = '\0';
    assert_int_equal(nextboot_sector_write(sec, sizeof sec, ~(uint64_t)0,
                                           value), NEXTBOOT_OK);
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                           sizeof out),
                     NEXTBOOT_SECTOR_VALUE);
    assert_true(stamp == ~(uint64_t)0);
    assert_string_equal(out, value);

    /* A caller buffer too small for the value is a refusal, not a
     * truncation. */
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out, 8u),
                     NEXTBOOT_SECTOR_BAD);
}

static void test_sector_write_refusals(void **state)
{
    uint8_t sec[NEXTBOOT_SECTOR_BYTES];

    (void)state;
    assert_int_equal(nextboot_sector_write(sec, sizeof sec, 1u, ""),
                     NEXTBOOT_ERR_VALUE);
    assert_int_equal(nextboot_sector_write(sec, sizeof sec, 1u, "a\nb"),
                     NEXTBOOT_ERR_VALUE);
    assert_int_equal(nextboot_sector_write(sec, sizeof sec, 1u, NULL),
                     NEXTBOOT_ERR_BAD_ARG);
    assert_int_equal(nextboot_sector_write(NULL, sizeof sec, 1u, "x"),
                     NEXTBOOT_ERR_BAD_ARG);
    assert_int_equal(nextboot_sector_write(sec, 511u, 1u, "x"),
                     NEXTBOOT_ERR_BAD_ARG);
}

/* A 4Kn disk's marker: one whole 4096-byte sector, the text in its first
 * 512 bytes and zeros to its end, which reads as the same record. */
static void test_sector_4k(void **state)
{
    static uint8_t sec[NEXTBOOT_SECTOR_MAX];
    static uint8_t small[NEXTBOOT_SECTOR_BYTES];
    char out[NEXTBOOT_MAX];
    uint64_t stamp = 0;
    size_t i;

    (void)state;
    memset(sec, 0xEE, sizeof sec);
    assert_int_equal(nextboot_sector_write(sec, sizeof sec, 1790000000u,
                                           WIN11_PATH), NEXTBOOT_OK);
    assert_int_equal(nextboot_sector_write(small, sizeof small, 1790000000u,
                                           WIN11_PATH), NEXTBOOT_OK);
    assert_memory_equal(sec, small, sizeof small);
    for (i = sizeof small; i < sizeof sec; i++)
        assert_int_equal(sec[i], 0);
    assert_true(nextboot_sector_is_marker(sec, sizeof sec));
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                           sizeof out),
                     NEXTBOOT_SECTOR_VALUE);
    assert_int_equal(stamp, 1790000000u);
    assert_string_equal(out, WIN11_PATH);

    /* The sector's other sizes between the two read the same. */
    assert_int_equal(nextboot_sector_parse(sec, 1024u, &stamp, out,
                                           sizeof out),
                     NEXTBOOT_SECTOR_VALUE);
    assert_int_equal(nextboot_sector_parse(sec, 2048u, &stamp, out,
                                           sizeof out),
                     NEXTBOOT_SECTOR_VALUE);

    /* A byte anywhere past the text, to the sector's last, is not a marker
     * this format wrote. */
    sec[NEXTBOOT_SECTOR_MAX - 1u] = 0x01u;
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                           sizeof out), NEXTBOOT_SECTOR_BAD);
    assert_string_equal(out, "");
    /* ... though read as a 512-byte sector the text alone is whole. */
    assert_int_equal(nextboot_sector_parse(sec, NEXTBOOT_SECTOR_BYTES, &stamp,
                                           out, sizeof out),
                     NEXTBOOT_SECTOR_VALUE);
    sec[NEXTBOOT_SECTOR_MAX - 1u] = 0u;
    sec[NEXTBOOT_SECTOR_BYTES] = (uint8_t)'x';
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                           sizeof out), NEXTBOOT_SECTOR_BAD);

    /* A value line that runs past the text area is refused even where the
     * sector is long enough to hold it. */
    memset(sec, 0, sizeof sec);
    memcpy(sec, "falcon-next-boot 1\n1\n", 21);
    memset(sec + 21, 'a', NEXTBOOT_SECTOR_BYTES - 21u);
    sec[NEXTBOOT_SECTOR_BYTES + 3u] = (uint8_t)'\n';
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                           sizeof out), NEXTBOOT_SECTOR_BAD);
    /* And so is a stamp line that does. */
    memset(sec, 0, sizeof sec);
    memcpy(sec, "falcon-next-boot 1\n", 19);
    memset(sec + 19, '1', NEXTBOOT_SECTOR_BYTES - 19u);
    assert_int_equal(nextboot_sector_parse(sec, sizeof sec, &stamp, out,
                                           sizeof out), NEXTBOOT_SECTOR_BAD);
}

static void test_sector_sizes(void **state)
{
    static uint8_t sec[2u * NEXTBOOT_SECTOR_MAX];
    char out[NEXTBOOT_MAX];
    uint64_t stamp = 0;

    (void)state;
    assert_true(nextboot_sector_size_ok(512u));
    assert_true(nextboot_sector_size_ok(1024u));
    assert_true(nextboot_sector_size_ok(2048u));
    assert_true(nextboot_sector_size_ok(4096u));
    assert_false(nextboot_sector_size_ok(0u));
    assert_false(nextboot_sector_size_ok(256u));
    assert_false(nextboot_sector_size_ok(511u));
    assert_false(nextboot_sector_size_ok(1536u));
    assert_false(nextboot_sector_size_ok(4095u));
    assert_false(nextboot_sector_size_ok(8192u));

    assert_int_equal(nextboot_sector_write(sec, 1536u, 1u, "x"),
                     NEXTBOOT_ERR_BAD_ARG);
    assert_int_equal(nextboot_sector_write(sec, 8192u, 1u, "x"),
                     NEXTBOOT_ERR_BAD_ARG);
    assert_int_equal(nextboot_sector_write(sec, 4096u, 1u, "x"), NEXTBOOT_OK);
    assert_int_equal(nextboot_sector_parse(sec, 8192u, &stamp, out,
                                           sizeof out), NEXTBOOT_SECTOR_NONE);
    assert_int_equal(nextboot_sector_parse(sec, 4000u, &stamp, out,
                                           sizeof out), NEXTBOOT_SECTOR_NONE);
}

/* ------------------------------------------------------------ dating it */

static void test_age(void **state)
{
    (void)state;
    assert_int_equal(nextboot_age_of(1000u, 1000u, 600u, 120u),
                     NEXTBOOT_AGE_FRESH);
    assert_int_equal(nextboot_age_of(1000u, 1600u, 600u, 120u),
                     NEXTBOOT_AGE_FRESH);
    assert_int_equal(nextboot_age_of(1000u, 1601u, 600u, 120u),
                     NEXTBOOT_AGE_STALE);
    assert_int_equal(nextboot_age_of(0u, ~(uint64_t)0, 600u, 120u),
                     NEXTBOOT_AGE_STALE);
    /* A clock corrected back a little between setting and booting: the rig
     * saw four seconds. */
    assert_int_equal(nextboot_age_of(1004u, 1000u, 600u, 120u),
                     NEXTBOOT_AGE_FRESH);
    assert_int_equal(nextboot_age_of(1120u, 1000u, 600u, 120u),
                     NEXTBOOT_AGE_FRESH);
    assert_int_equal(nextboot_age_of(1121u, 1000u, 600u, 120u),
                     NEXTBOOT_AGE_FUTURE);
    assert_int_equal(nextboot_age_of(1001u, 1000u, 600u, 0u),
                     NEXTBOOT_AGE_FUTURE);
    assert_int_equal(nextboot_age_of(~(uint64_t)0, 0u, 600u, 120u),
                     NEXTBOOT_AGE_FUTURE);
}

/* Checked against `date -u -d @N`: the number Linux's since_epoch gives for
 * the same digits. */
static void test_rtc_seconds(void **state)
{
    static const struct {
        uint8_t bcd[7];
        uint64_t want;
    } cases[] = {
        { { 0x19, 0x70, 0x01, 0x01, 0x00, 0x00, 0x00 }, 0u },
        { { 0x20, 0x00, 0x02, 0x29, 0x12, 0x34, 0x56 }, 951827696u },
        { { 0x20, 0x26, 0x09, 0x24, 0x23, 0x59, 0x59 }, 1790294399u },
        { { 0x20, 0x26, 0x12, 0x31, 0x00, 0x00, 0x00 }, 1798675200u },
        { { 0x21, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00 }, 4107542400u },
    };
    uint64_t got;
    size_t i;

    (void)state;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        got = 0;
        assert_int_equal(nextboot_rtc_seconds(cases[i].bcd, &got),
                         NEXTBOOT_OK);
        assert_true(got == cases[i].want);
    }
}

static void test_rtc_refusals(void **state)
{
    static const uint8_t bad[][7] = {
        { 0x19, 0x69, 0x12, 0x31, 0x23, 0x59, 0x59 },   /* before 1970 */
        { 0x20, 0x26, 0x13, 0x01, 0x00, 0x00, 0x00 },   /* month 13 */
        { 0x20, 0x26, 0x00, 0x01, 0x00, 0x00, 0x00 },   /* month 0 */
        { 0x20, 0x26, 0x04, 0x31, 0x00, 0x00, 0x00 },   /* April 31st */
        { 0x20, 0x26, 0x01, 0x00, 0x00, 0x00, 0x00 },   /* day 0 */
        { 0x20, 0x26, 0x01, 0x01, 0x24, 0x00, 0x00 },   /* hour 24 */
        { 0x20, 0x26, 0x01, 0x01, 0x81, 0x00, 0x00 },   /* 12-hour PM flag */
        { 0x20, 0x26, 0x01, 0x01, 0x00, 0x60, 0x00 },   /* minute 60 */
        { 0x20, 0x26, 0x01, 0x01, 0x00, 0x00, 0x60 },   /* second 60 */
        { 0x20, 0x2A, 0x01, 0x01, 0x00, 0x00, 0x00 },   /* not BCD */
        { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },   /* a dead clock */
    };
    uint64_t got = 5;
    size_t i;

    (void)state;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++)
        assert_int_equal(nextboot_rtc_seconds(bad[i], &got),
                         NEXTBOOT_ERR_VALUE);
    assert_true(got == 5u);
    assert_int_equal(nextboot_rtc_seconds(NULL, &got), NEXTBOOT_ERR_BAD_ARG);
    assert_int_equal(nextboot_rtc_seconds(bad[0], NULL),
                     NEXTBOOT_ERR_BAD_ARG);
}

int run_nextboot_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_decode_plain),
        cmocka_unit_test(test_decode_empty),
        cmocka_unit_test(test_decode_oversized),
        cmocka_unit_test(test_decode_non_ascii),
        cmocka_unit_test(test_decode_embedded_nul),
        cmocka_unit_test(test_decode_null_args),
        cmocka_unit_test(test_names),
        cmocka_unit_test(test_names_non_ascii_path),
        cmocka_unit_test(test_rule),
        cmocka_unit_test(test_reserved_personal),
        cmocka_unit_test(test_reserved_rejects_paths),
        cmocka_unit_test(test_personal_is_exact),
        cmocka_unit_test(test_sector_round_trip),
        cmocka_unit_test(test_sector_personal_and_zero_stamp),
        cmocka_unit_test(test_sector_not_a_marker),
        cmocka_unit_test(test_sector_bad_shapes),
        cmocka_unit_test(test_sector_limits),
        cmocka_unit_test(test_sector_write_refusals),
        cmocka_unit_test(test_sector_4k),
        cmocka_unit_test(test_sector_sizes),
        cmocka_unit_test(test_age),
        cmocka_unit_test(test_rtc_seconds),
        cmocka_unit_test(test_rtc_refusals),
    };

    return cmocka_run_group_tests_name("nextboot", tests, NULL, NULL);
}
