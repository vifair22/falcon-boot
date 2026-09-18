/* .osrel label extraction (src/pos/osrel.c). The parser walks a length-prefixed PE
 * structure, so most of what is worth testing here is what it does with a
 * malformed one: every failure must leave the caller's buffer empty and
 * return an error the caller turns into "use the filename". */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <pos/osrel.h>
#include "tests.h"

/* A minimal PE32+ with one named section, built by hand so the tests can bend
 * individual fields. Layout: DOS stub, PE signature at 0x80, COFF header, an
 * optional header of the declared size, then the section table. */
#define PE_AT     0x80u
#define OPTSIZE   0xF0u
#define SECT_AT   (PE_AT + 24u + OPTSIZE)
#define BODY_AT   (SECT_AT + 40u * 2u)
#define IMG_MAX   1024u

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Builds an image whose .osrel section holds body. Returns the total length. */
static size_t build_pe(uint8_t *img, const char *sect, const char *body)
{
    size_t blen = strlen(body);

    memset(img, 0, IMG_MAX);
    img[0] = 'M'; img[1] = 'Z';
    put32(img + 0x3C, PE_AT);
    img[PE_AT] = 'P'; img[PE_AT + 1] = 'E';
    put16(img + PE_AT + 6, 2);          /* NumberOfSections */
    put16(img + PE_AT + 20, OPTSIZE);   /* SizeOfOptionalHeader */

    /* A decoy first, so a parser that only ever looks at section zero fails. */
    memcpy(img + SECT_AT, ".text", 5);
    put32(img + SECT_AT + 16, 4);
    put32(img + SECT_AT + 20, BODY_AT);

    memcpy(img + SECT_AT + 40, sect, strlen(sect));
    put32(img + SECT_AT + 40 + 16, (uint32_t)blen);
    put32(img + SECT_AT + 40 + 20, BODY_AT);
    memcpy(img + BODY_AT, body, blen);
    return BODY_AT + blen;
}

static void test_pretty_name_wins(void **state)
{
    uint8_t img[IMG_MAX];
    char out[OSREL_LABEL_MAX];
    size_t n = build_pe(img, ".osrel",
                        "NAME=\"Arch Linux\"\nPRETTY_NAME=\"Arch Linux\"\nID=arch\n");

    assert_int_equal(osrel_label(img, n, out, sizeof out), OSREL_OK);
    assert_string_equal(out, "Arch Linux");
    (void)state;
}

/* The appliance is the reason this matters: its filename says nothing useful,
 * and its osrel can say exactly what it is. */
static void test_appliance_label(void **state)
{
    uint8_t img[IMG_MAX];
    char out[OSREL_LABEL_MAX];
    size_t n = build_pe(img, ".osrel",
                        "PRETTY_NAME=\"falcon-boot signing appliance\"\n");

    assert_int_equal(osrel_label(img, n, out, sizeof out), OSREL_OK);
    assert_string_equal(out, "falcon-boot signing appliance");
    (void)state;
}

static void test_name_and_version_fallback(void **state)
{
    uint8_t img[IMG_MAX];
    char out[OSREL_LABEL_MAX];
    size_t n = build_pe(img, ".osrel", "NAME=\"Arch Linux\"\nVERSION_ID=20260101\n");

    assert_int_equal(osrel_label(img, n, out, sizeof out), OSREL_OK);
    assert_string_equal(out, "Arch Linux 20260101");
    (void)state;
}

/* Fedora's shape: NAME already carries the version, so appending it again
 * would read "Fedora Linux 41 41". */
static void test_version_not_duplicated(void **state)
{
    uint8_t img[IMG_MAX];
    char out[OSREL_LABEL_MAX];
    size_t n = build_pe(img, ".osrel", "NAME=\"Fedora Linux 41\"\nVERSION_ID=41\n");

    assert_int_equal(osrel_label(img, n, out, sizeof out), OSREL_OK);
    assert_string_equal(out, "Fedora Linux 41");
    (void)state;
}

static void test_unquoted_and_comments(void **state)
{
    char out[OSREL_LABEL_MAX];
    const char *t = "# PRETTY_NAME=Decoy\nID=arch\nPRETTY_NAME=Arch\n";

    assert_int_equal(osrel_value(t, strlen(t), "PRETTY_NAME", out, sizeof out),
                     OSREL_OK);
    assert_string_equal(out, "Arch");
    (void)state;
}

/* A key that merely starts the same must not match: NAME must not be answered
 * by NAME_SOMETHING, and PRETTY_NAME must not be answered by NAME. */
static void test_key_is_not_a_prefix(void **state)
{
    char out[OSREL_LABEL_MAX];
    const char *t = "NAME_LONG=wrong\nNAME=right\n";

    assert_int_equal(osrel_value(t, strlen(t), "NAME", out, sizeof out),
                     OSREL_OK);
    assert_string_equal(out, "right");
    (void)state;
}

/* A control byte would be drawn straight to a console that treats some of them
 * as commands, so it is refused rather than printed. */
static void test_control_bytes_refused(void **state)
{
    char out[OSREL_LABEL_MAX];
    const char *t = "PRETTY_NAME=\"Arch\033[2JLinux\"\n";

    assert_int_not_equal(osrel_value(t, strlen(t), "PRETTY_NAME", out,
                                     sizeof out), OSREL_OK);
    assert_string_equal(out, "");
    (void)state;
}

/* A long value is clipped, not refused: a clipped name still beats a filename. */
static void test_long_value_truncates(void **state)
{
    char out[8];
    const char *t = "PRETTY_NAME=\"Extremely Long Distribution Name\"\n";

    assert_int_equal(osrel_value(t, strlen(t), "PRETTY_NAME", out, sizeof out),
                     OSREL_OK);
    assert_int_equal(strlen(out), 7);
    (void)state;
}

static void test_no_osrel_section(void **state)
{
    uint8_t img[IMG_MAX];
    char out[OSREL_LABEL_MAX];
    size_t n = build_pe(img, ".sbat", "PRETTY_NAME=\"Nope\"\n");

    assert_int_equal(osrel_label(img, n, out, sizeof out), OSREL_ERR_NO_SECTION);
    assert_string_equal(out, "");
    (void)state;
}

static void test_not_a_pe(void **state)
{
    char out[OSREL_LABEL_MAX];
    static const uint8_t junk[64] = { 'n', 'o', 't', 'p', 'e' };

    assert_int_equal(osrel_label(junk, sizeof junk, out, sizeof out),
                     OSREL_ERR_NOT_PE);
    assert_string_equal(out, "");
    (void)state;
}

/* The bounds checks, which are the whole reason this parser is allowed to run
 * before ExitBootServices. Each of these is a field pointing outside the
 * buffer; none may read past it. */
static void test_lfanew_past_end(void **state)
{
    uint8_t img[IMG_MAX];
    char out[OSREL_LABEL_MAX];
    size_t n = build_pe(img, ".osrel", "PRETTY_NAME=\"Arch\"\n");

    put32(img + 0x3C, 0xFFFFFF00u);
    assert_int_equal(osrel_label(img, n, out, sizeof out), OSREL_ERR_NOT_PE);
    (void)state;
}

static void test_section_count_absurd(void **state)
{
    uint8_t img[IMG_MAX];
    char out[OSREL_LABEL_MAX];
    size_t n = build_pe(img, ".osrel", "PRETTY_NAME=\"Arch\"\n");

    put16(img + PE_AT + 6, 0xFFFFu);
    assert_int_equal(osrel_label(img, n, out, sizeof out), OSREL_ERR_NOT_PE);
    (void)state;
}

static void test_raw_pointer_past_end(void **state)
{
    uint8_t img[IMG_MAX];
    char out[OSREL_LABEL_MAX];
    size_t n = build_pe(img, ".osrel", "PRETTY_NAME=\"Arch\"\n");

    put32(img + SECT_AT + 40 + 20, 0xFFFF0000u);
    assert_int_equal(osrel_label(img, n, out, sizeof out),
                     OSREL_ERR_NO_SECTION);
    (void)state;
}

static void test_raw_size_past_end(void **state)
{
    uint8_t img[IMG_MAX];
    char out[OSREL_LABEL_MAX];
    size_t n = build_pe(img, ".osrel", "PRETTY_NAME=\"Arch\"\n");

    put32(img + SECT_AT + 40 + 16, 0xFFFF0000u);
    assert_int_equal(osrel_label(img, n, out, sizeof out),
                     OSREL_ERR_NO_SECTION);
    (void)state;
}

static void test_truncated_image(void **state)
{
    uint8_t img[IMG_MAX];
    char out[OSREL_LABEL_MAX];

    (void)build_pe(img, ".osrel", "PRETTY_NAME=\"Arch\"\n");
    assert_int_equal(osrel_label(img, 8, out, sizeof out), OSREL_ERR_NOT_PE);
    (void)state;
}

static void test_null_args(void **state)
{
    char out[OSREL_LABEL_MAX];
    uint8_t img[IMG_MAX];
    size_t n = build_pe(img, ".osrel", "PRETTY_NAME=\"Arch\"\n");

    assert_int_equal(osrel_label(NULL, n, out, sizeof out), OSREL_ERR_BAD_ARG);
    assert_int_equal(osrel_label(img, n, NULL, sizeof out), OSREL_ERR_BAD_ARG);
    assert_int_equal(osrel_label(img, n, out, 0), OSREL_ERR_BAD_ARG);
    assert_int_equal(osrel_value(NULL, 5, "NAME", out, sizeof out),
                     OSREL_ERR_BAD_ARG);
    (void)state;
}

/* THE regression test for the shape every real UKI has: the section table is
 * near the front and .osrel is far past any window a caller would read.
 *
 * The first cut validated the section's file range against the LENGTH OF THE
 * BUFFER rather than the length of the file, so it answered "no .osrel
 * section" for every real image and the loader showed filenames. It shipped,
 * because the crafted PEs in these tests were 6.7 KB - smaller than the
 * window - so their .osrel fell inside the buffer and the bug was invisible.
 * A fixture smaller than the thing it stands in for proves nothing about it.
 *
 * Numbers from a real mkinitcpio UKI: section table at 392, .osrel at 130048,
 * file 91 MB. */
static void test_osrel_beyond_the_window(void **state)
{
    uint8_t img[IMG_MAX];
    size_t off = 0, size = 0;
    const size_t window = 512;      /* stands in for the loader's 8 KB read */
    const size_t total = 130560;    /* stands in for the whole file */

    (void)build_pe(img, ".osrel", "PRETTY_NAME=\"Arch Linux\"\n");
    /* Point the section far outside the window, as ukify does. */
    put32(img + SECT_AT + 40 + 20, 130048u);
    put32(img + SECT_AT + 40 + 16, 512u);

    assert_int_equal(osrel_find(img, window, total, ".osrel", &off, &size), OSREL_OK);
    assert_int_equal(off, 130048u);
    assert_int_equal(size, 512u);
    (void)state;
}

/* The bound still has to bite against the FILE. A section claiming to start
 * past the end of the image is refused, window or no window. */
static void test_osrel_beyond_the_file(void **state)
{
    uint8_t img[IMG_MAX];
    size_t off = 0, size = 0;

    (void)build_pe(img, ".osrel", "PRETTY_NAME=\"Arch Linux\"\n");
    put32(img + SECT_AT + 40 + 20, 130048u);
    put32(img + SECT_AT + 40 + 16, 512u);

    assert_int_equal(osrel_find(img, 512u, 4096u, ".osrel", &off, &size),
                     OSREL_ERR_NO_SECTION);
    (void)state;
}

/* total < len is a caller that does not know its own buffer. */
static void test_osrel_total_below_len(void **state)
{
    uint8_t img[IMG_MAX];
    size_t off = 0, size = 0;
    size_t n = build_pe(img, ".osrel", "PRETTY_NAME=\"Arch\"\n");

    assert_int_equal(osrel_find(img, n, n / 2u, ".osrel", &off, &size),
                     OSREL_ERR_BAD_ARG);
    (void)state;
}

/* Kernel version order, as the chooser uses it to put the newest first. */
static void test_vercmp(void **state)
{
    (void)state;
    assert_true(osrel_vercmp("7.2.6-arch2-1", "6.18.52-1-lts") > 0);
    assert_true(osrel_vercmp("6.18.52-1-lts", "7.2.6-arch2-1") < 0);
    assert_true(osrel_vercmp("6.10.1", "6.9.12") > 0);
    assert_true(osrel_vercmp("6.9", "6.10") < 0);
    assert_true(osrel_vercmp("7.2.6-arch2-1", "7.2.6-arch10-1") < 0);
    assert_true(osrel_vercmp("6.18.2-arch2-1", "6.18.2-arch2-1") == 0);
    assert_true(osrel_vercmp("6.018", "6.18") == 0);
    assert_true(osrel_vercmp("6.18", "6.018") == 0);
    assert_true(osrel_vercmp("6.18", "6.18.1") < 0);
    assert_true(osrel_vercmp("6.18.1", "6.18") > 0);
    assert_true(osrel_vercmp("6.18-lts", "6.18-arch") > 0);
    assert_true(osrel_vercmp("99999999999999999999999", "1") > 0);
    assert_true(osrel_vercmp("", "") == 0);
    assert_true(osrel_vercmp(NULL, "1") < 0);
    assert_true(osrel_vercmp("1", NULL) > 0);
    assert_true(osrel_vercmp(NULL, NULL) == 0);
}

int run_osrel_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_vercmp),
        cmocka_unit_test(test_pretty_name_wins),
        cmocka_unit_test(test_appliance_label),
        cmocka_unit_test(test_name_and_version_fallback),
        cmocka_unit_test(test_version_not_duplicated),
        cmocka_unit_test(test_unquoted_and_comments),
        cmocka_unit_test(test_key_is_not_a_prefix),
        cmocka_unit_test(test_control_bytes_refused),
        cmocka_unit_test(test_long_value_truncates),
        cmocka_unit_test(test_no_osrel_section),
        cmocka_unit_test(test_not_a_pe),
        cmocka_unit_test(test_lfanew_past_end),
        cmocka_unit_test(test_section_count_absurd),
        cmocka_unit_test(test_raw_pointer_past_end),
        cmocka_unit_test(test_raw_size_past_end),
        cmocka_unit_test(test_truncated_image),
        cmocka_unit_test(test_null_args),
        cmocka_unit_test(test_osrel_beyond_the_window),
        cmocka_unit_test(test_osrel_beyond_the_file),
        cmocka_unit_test(test_osrel_total_below_len),
    };

    return cmocka_run_group_tests_name("osrel", tests, NULL, NULL);
}
