/* PE/COFF header layout (src/pe/pe.c). Synthetic images bend one field at a
 * time; the real loader binary checks the answers against a file a real
 * linker wrote. osrel_test.c covers the same section walk through
 * osrel_find, which is now a wrapper over this module. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include <pe/pe.h>
#include "tests.h"

#define PE_AT   0x80u
#define IMG_MAX 2048u

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* A header-only image: optional header of `magic` with `ndirs` directory
 * entries, then `nsect` sections named from `names`. Returns the offset of
 * the section table. */
static size_t build(uint8_t *img, uint16_t magic, uint32_t ndirs,
                    uint16_t nsect, const char *const *names)
{
    size_t dirs = (magic == PE_MAGIC_PE32P) ? 112u : 96u;
    size_t optsize = dirs + (size_t)ndirs * 8u;
    size_t opt = PE_AT + 24u;
    size_t table = opt + optsize;
    uint16_t i;

    memset(img, 0, IMG_MAX);
    img[0] = 'M'; img[1] = 'Z';
    put32(img + 0x3c, PE_AT);
    memcpy(img + PE_AT, "PE\0\0", 4);
    put16(img + PE_AT + 6, nsect);
    put16(img + PE_AT + 20, (uint16_t)optsize);
    put16(img + opt, magic);
    put32(img + opt + 60, 0x400);                   /* SizeOfHeaders */
    put32(img + opt + ((magic == PE_MAGIC_PE32P) ? 108u : 92u), ndirs);
    for (i = 0; i < nsect; i++) {
        uint8_t *s = img + table + (size_t)i * 40u;

        memcpy(s, names[i], strlen(names[i]) > 8 ? 8 : strlen(names[i]));
        put32(s + 16, 0x100u);                      /* SizeOfRawData */
        put32(s + 20, 0x400u + 0x100u * i);         /* PointerToRawData */
    }
    return table;
}

static void test_parse_refusals(void **state)
{
    uint8_t img[IMG_MAX];
    pe_hdr h;
    static const char *const one[] = { ".text" };

    (void)state;
    assert_int_equal(pe_parse(NULL, 0, &h), PE_ERR_BAD_ARG);
    assert_int_equal(pe_parse(img, 0, NULL), PE_ERR_BAD_ARG);

    build(img, PE_MAGIC_PE32P, 16, 1, one);
    assert_int_equal(pe_parse(img, 0x3f, &h), PE_ERR_NOT_PE);
    img[0] = 'X';
    assert_int_equal(pe_parse(img, IMG_MAX, &h), PE_ERR_NOT_PE);
    img[0] = 'M';
    put32(img + 0x3c, IMG_MAX);                     /* lfanew past the window */
    assert_int_equal(pe_parse(img, IMG_MAX, &h), PE_ERR_NOT_PE);
    put32(img + 0x3c, PE_AT);
    img[PE_AT + 2] = 'X';
    assert_int_equal(pe_parse(img, IMG_MAX, &h), PE_ERR_NOT_PE);
    img[PE_AT + 2] = 0;
    assert_int_equal(pe_parse(img, PE_AT + 23u, &h), PE_ERR_NOT_PE);
    assert_int_equal(pe_parse(img, IMG_MAX, &h), PE_OK);
    assert_int_equal(h.magic, PE_MAGIC_PE32P);
    assert_int_equal(h.nsect, 1);
}

static void test_fields_both_magics(void **state)
{
    uint8_t img[IMG_MAX];
    static const uint16_t magics[] = { PE_MAGIC_PE32, PE_MAGIC_PE32P };
    size_t m;

    (void)state;
    for (m = 0; m < 2; m++) {
        pe_hdr h;
        size_t off, entry;
        uint32_t v, rva, size;
        size_t opt = PE_AT + 24u;
        size_t dirs = (magics[m] == PE_MAGIC_PE32P) ? 112u : 96u;

        build(img, magics[m], 16, 0, NULL);
        put32(img + opt + dirs + 4u * 8u, 0x1234u);     /* security rva */
        put32(img + opt + dirs + 4u * 8u + 4u, 0x88u);  /* security size */
        assert_int_equal(pe_parse(img, IMG_MAX, &h), PE_OK);
        assert_int_equal(pe_checksum_off(&h, &off), PE_OK);
        assert_int_equal(off, opt + 64u);
        assert_int_equal(pe_size_of_headers(&h, img, &v), PE_OK);
        assert_int_equal(v, 0x400u);
        assert_int_equal(pe_dir_count(&h, img, &v), PE_OK);
        assert_int_equal(v, 16u);
        assert_int_equal(pe_dir(&h, img, PE_DIR_SECURITY, &entry, &rva, &size),
                         PE_OK);
        assert_int_equal(entry, opt + dirs + 32u);
        assert_int_equal(rva, 0x1234u);
        assert_int_equal(size, 0x88u);
        /* The directory array stops at 16. */
        assert_int_equal(pe_dir(&h, img, 16, &entry, &rva, &size),
                         PE_ERR_ABSENT);
    }
}

static void test_dir_edges(void **state)
{
    uint8_t img[IMG_MAX];
    pe_hdr h;
    size_t entry, off;
    uint32_t rva, size, v;

    (void)state;
    /* A short directory array has no security entry: an answer. */
    build(img, PE_MAGIC_PE32P, 4, 0, NULL);
    assert_int_equal(pe_parse(img, IMG_MAX, &h), PE_OK);
    assert_int_equal(pe_dir(&h, img, PE_DIR_SECURITY, &entry, &rva, &size),
                     PE_ERR_ABSENT);

    /* A window that stops inside the array is "read more". */
    build(img, PE_MAGIC_PE32P, 16, 0, NULL);
    assert_int_equal(pe_parse(img, PE_AT + 24u + 112u + 8u, &h), PE_OK);
    assert_int_equal(pe_dir(&h, img, PE_DIR_SECURITY, &entry, &rva, &size),
                     PE_ERR_SHORT);

    /* An unfamiliar magic parses (sections do not care) but has no fields. */
    build(img, 0x1234u, 16, 0, NULL);
    assert_int_equal(pe_parse(img, IMG_MAX, &h), PE_OK);
    assert_int_equal(pe_checksum_off(&h, &off), PE_ERR_NOT_PE);
    assert_int_equal(pe_dir_count(&h, img, &v), PE_ERR_NOT_PE);
    assert_int_equal(pe_dir(&h, img, 0, &entry, &rva, &size), PE_ERR_NOT_PE);

    /* A directory entry claimed past SizeOfOptionalHeader is refused. */
    build(img, PE_MAGIC_PE32P, 16, 0, NULL);
    put32(img + PE_AT + 24u + 108u, 40u);
    assert_int_equal(pe_parse(img, IMG_MAX, &h), PE_OK);
    assert_int_equal(pe_dir(&h, img, 20, &entry, &rva, &size), PE_ERR_NOT_PE);

    assert_int_equal(pe_checksum_off(NULL, &off), PE_ERR_BAD_ARG);
    assert_int_equal(pe_size_of_headers(&h, NULL, &v), PE_ERR_BAD_ARG);
    assert_int_equal(pe_dir_count(&h, img, NULL), PE_ERR_BAD_ARG);
    assert_int_equal(pe_dir(&h, img, 0, NULL, &rva, &size), PE_ERR_BAD_ARG);
}

static void test_sections(void **state)
{
    uint8_t img[IMG_MAX];
    static const char *const names[] = { ".text", ".sbatx", ".sbat",
                                         "eightchr" };
    pe_hdr h;
    pe_sect s;
    size_t off, size, table;

    (void)state;
    table = build(img, PE_MAGIC_PE32P, 16, 4, names);
    assert_int_equal(pe_parse(img, IMG_MAX, &h), PE_OK);
    assert_int_equal(pe_section(&h, img, 3, &s), PE_OK);
    assert_string_equal(s.name, "eightchr");
    assert_int_equal(pe_section(&h, img, 4, &s), PE_ERR_ABSENT);

    /* ".sbat" is the third, not the ".sbatx" before it. */
    assert_int_equal(pe_find_section(&h, img, 0x10000, ".sbat", &off, &size),
                     PE_OK);
    assert_int_equal(off, 0x600u);
    assert_int_equal(size, 0x100u);
    assert_int_equal(pe_find_section(&h, img, 0x10000, "eightchr", &off,
                                     &size), PE_OK);
    assert_int_equal(pe_find_section(&h, img, 0x10000, ".osrel", &off, &size),
                     PE_ERR_ABSENT);

    /* Content past the FILE is a range error; past the window is fine. */
    assert_int_equal(pe_find_section(&h, img, 0x650, ".sbat", &off, &size),
                     PE_ERR_RANGE);
    /* Empty section: nothing to read. */
    put32(img + table + 2u * 40u + 16u, 0);
    assert_int_equal(pe_find_section(&h, img, 0x10000, ".sbat", &off, &size),
                     PE_ERR_ABSENT);

    /* A table that runs past the window is "read more", not "absent". */
    assert_int_equal(pe_parse(img, table + 40u, &h), PE_OK);
    assert_int_equal(pe_find_section(&h, img, 0x10000, ".sbat", &off, &size),
                     PE_ERR_SHORT);
    assert_int_equal(pe_section(&h, img, 2, &s), PE_ERR_SHORT);

    assert_int_equal(pe_find_section(&h, img, 0, NULL, &off, &size),
                     PE_ERR_BAD_ARG);
    assert_int_equal(pe_section(&h, NULL, 0, &s), PE_ERR_BAD_ARG);
}

/* The loader itself, as the linker wrote it. */
static void test_real_loader(void **state)
{
    static uint8_t buf[4096];
    char path[512];
    FILE *f;
    size_t got, off, size, entry;
    long total;
    uint32_t rva, sz, soh;
    pe_hdr h;

    (void)state;
    snprintf(path, sizeof(path), "%s/unsigned.efi", TRUST_DIR);
    f = fopen(path, "rb");
    assert_non_null(f);
    got = fread(buf, 1, sizeof(buf), f);
    fseek(f, 0, SEEK_END);
    total = ftell(f);
    fclose(f);

    assert_int_equal(pe_parse(buf, got, &h), PE_OK);
    assert_int_equal(h.magic, PE_MAGIC_PE32P);
    assert_int_equal(pe_size_of_headers(&h, buf, &soh), PE_OK);
    assert_true(soh <= got);
    assert_int_equal(pe_dir(&h, buf, PE_DIR_SECURITY, &entry, &rva, &sz),
                     PE_OK);
    assert_int_equal(sz, 0);            /* unsigned */
    assert_int_equal(pe_find_section(&h, buf, (size_t)total, ".sbat", &off,
                                     &size), PE_OK);
    assert_true(off + size <= (size_t)total);
}

int run_pe_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_parse_refusals),
        cmocka_unit_test(test_fields_both_magics),
        cmocka_unit_test(test_dir_edges),
        cmocka_unit_test(test_sections),
    };
    const struct CMUnitTest image_tests[] = {
        cmocka_unit_test(test_real_loader),
    };
    char path[512];
    int failed;
    FILE *f;

    failed = cmocka_run_group_tests_name("pe", tests, NULL, NULL);

    snprintf(path, sizeof(path), "%s/unsigned.efi", TRUST_DIR);
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("\n[  SKIPPED ] pe real image: %s not found.\n"
               "             Build it with: tools/mk-test-trust.sh "
               "(needs openssl and sbsign).\n", path);
        return failed + test_skip_rc();
    }
    fclose(f);
    return failed + cmocka_run_group_tests_name("pe (real image)",
                                                image_tests, NULL, NULL);
}
