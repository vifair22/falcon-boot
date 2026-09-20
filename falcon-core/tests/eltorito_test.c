/* Tests for falcon-core/src/media/eltorito.c: the boot catalog reader
 * (docs/notes/iso-reader-route.md).
 *
 * The contracts that matter are the two the corpus sweep of 2026-09-19 turned
 * up, because both are places a reasonable implementation would go wrong:
 * a zero sector count is normal and must not be rejected, and a section
 * header's platform ID is whatever the medium says and must be reported
 * rather than filtered on. OPNsense 25.1's real header is reproduced below
 * for the second, next to FreeBSD 15.1's, which differs in that one byte.
 *
 * Everything else is about not trusting medium data: a section count that
 * runs off the end of the sector, more entries than fit, and a descriptor
 * that names a sector the medium does not have.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>

#include <media/eltorito.h>

#define SEC     2048u
#define SECTORS 32u
#define CAT_LBA 20u

static uint8_t g_iso[SECTORS * SEC];
static int g_fail_at = -1;      /* sector whose read fails, or -1 */

static int iso_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    if (g_fail_at >= 0 && off / SEC == (uint64_t)g_fail_at)
        return -1;
    if (off + len > sizeof g_iso)
        return -1;
    memcpy(buf, g_iso + off, (size_t)len);
    return 0;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* A boot record volume descriptor at sector 17 pointing at CAT_LBA. */
static void build_brvd(uint32_t catalog_lba)
{
    uint8_t *d = g_iso + 17u * SEC;

    memset(d, 0, SEC);
    d[0] = 0;
    memcpy(d + 1, "CD001", 5);
    d[6] = 1;
    memcpy(d + 7, "EL TORITO SPECIFICATION", 23);
    put_le32(d + 0x47, catalog_lba);
}

static uint8_t *cat(void)
{
    return g_iso + (size_t)CAT_LBA * SEC;
}

/* The validation entry: header id 1, a platform, and the 0x55 0xAA key. */
static void build_validation(uint8_t platform)
{
    uint8_t *v = cat();

    memset(v, 0, SEC);
    v[0] = 1;
    v[1] = platform;
    v[30] = 0x55;
    v[31] = 0xAA;
}

static void put_entry(unsigned index, uint8_t indicator, uint8_t media,
                      uint16_t sectors, uint32_t lba)
{
    uint8_t *e = cat() + (size_t)index * 32u;

    memset(e, 0, 32);
    e[0] = indicator;
    e[1] = media;
    put_le16(e + 6, sectors);
    put_le32(e + 8, lba);
}

static void put_header(unsigned index, uint8_t id, uint8_t platform,
                       uint16_t count)
{
    uint8_t *h = cat() + (size_t)index * 32u;

    memset(h, 0, 32);
    h[0] = id;
    h[1] = platform;
    put_le16(h + 2, count);
}

static void reset(void)
{
    memset(g_iso, 0, sizeof g_iso);
    g_fail_at = -1;
}

/* The shape every corpus medium has: an x86 default entry and one final
 * section header carrying a single UEFI entry. */
static void build_typical(void)
{
    reset();
    build_brvd(CAT_LBA);
    build_validation(ELT_PLATFORM_X86);
    put_entry(1, 0x88, 0, 4, 1044);
    put_header(2, 0x91, ELT_PLATFORM_UEFI, 1);
    put_entry(3, 0x88, 0, 4096, 20);
}

static void test_elt_typical(void **state)
{
    elt_catalog c;

    (void)state;
    build_typical();
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso), ELT_OK);
    assert_int_equal(c.n, 2);
    assert_int_equal(c.catalog_lba, CAT_LBA);
    assert_int_equal(c.validation_platform, ELT_PLATFORM_X86);
    assert_int_equal(c.truncated, 0);

    /* The default entry takes its platform from the validation entry. */
    assert_int_equal(c.entry[0].is_default, 1);
    assert_int_equal(c.entry[0].platform, ELT_PLATFORM_X86);
    assert_int_equal(c.entry[0].media, ELT_MEDIA_NONE);
    assert_int_equal(c.entry[0].lba, 1044);
    assert_int_equal(c.entry[0].sectors, 4);

    assert_int_equal(c.entry[1].is_default, 0);
    assert_int_equal(c.entry[1].platform, ELT_PLATFORM_UEFI);
    assert_int_equal(c.entry[1].lba, 20);
    assert_int_equal(c.entry[1].sectors, 4096);
}

/* Arch and Void record a zero sector count on their UEFI entry, and Windows
 * and ESXi record 1. Both are entries the firmware boots. Rejecting either,
 * or treating the field as a length, loses real media. */
static void test_elt_sector_count_is_not_a_size(void **state)
{
    elt_catalog c;

    (void)state;
    build_typical();
    put_entry(3, 0x88, 0, 0, 622776);          /* arch-2026.01.01 */
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso), ELT_OK);
    assert_int_equal(c.n, 2);
    assert_int_equal(c.entry[1].sectors, 0);
    assert_int_equal(c.entry[1].lba, 622776);

    put_entry(3, 0x88, 0, 1, 536);             /* windows-11-25h2 */
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso), ELT_OK);
    assert_int_equal(c.n, 2);
    assert_int_equal(c.entry[1].sectors, 1);
    assert_int_equal(c.entry[1].lba, 536);
}

/* OPNsense 25.1 against FreeBSD 15.1. The two media differ in exactly one
 * byte of the section header and are otherwise identical, and OPNsense's
 * entry is a real, bootable UEFI FAT image. The parser reports the byte; it
 * must not filter on it. */
static void test_elt_platform_is_reported_not_filtered(void **state)
{
    elt_catalog c;

    (void)state;
    build_typical();
    put_header(2, 0x91, ELT_PLATFORM_UEFI, 1); /* 91 ef 01 00, FreeBSD */
    put_entry(3, 0x88, 0, 4096, 20);
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso), ELT_OK);
    assert_int_equal(c.n, 2);
    assert_int_equal(c.entry[1].platform, ELT_PLATFORM_UEFI);
    assert_int_equal(c.entry[1].lba, 20);

    put_header(2, 0x91, ELT_PLATFORM_X86, 1);  /* 91 00 01 00, OPNsense */
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso), ELT_OK);
    assert_int_equal(c.n, 2);
    assert_int_equal(c.entry[1].platform, ELT_PLATFORM_X86);
    assert_int_equal(c.entry[1].lba, 20);
    assert_int_equal(c.entry[1].is_default, 0);
}

/* A non-bootable entry is skipped, not reported, and a catalog of nothing but
 * those succeeds with no entries rather than failing. */
static void test_elt_non_bootable_entries(void **state)
{
    elt_catalog c;

    (void)state;
    build_typical();
    put_entry(1, 0x00, 0, 4, 1044);
    put_entry(3, 0x00, 0, 4096, 20);
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso), ELT_OK);
    assert_int_equal(c.n, 0);
    assert_int_equal(c.truncated, 0);
}

/* A section entry extension carries more of the previous entry's data. It is
 * not an entry and must not consume one of the section's declared slots. */
static void test_elt_section_extension_is_skipped(void **state)
{
    elt_catalog c;

    (void)state;
    reset();
    build_brvd(CAT_LBA);
    build_validation(ELT_PLATFORM_X86);
    put_entry(1, 0x88, 0, 4, 100);
    put_header(2, 0x91, ELT_PLATFORM_UEFI, 2);
    put_entry(3, 0x88, 0, 0, 200);
    put_entry(4, 0x44, 0, 0, 0);               /* the extension */
    put_entry(5, 0x88, 0, 0, 300);
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso), ELT_OK);
    assert_int_equal(c.n, 3);
    assert_int_equal(c.entry[1].lba, 200);
    assert_int_equal(c.entry[2].lba, 300);
    assert_int_equal(c.truncated, 0);
}

/* Two headers, the first 0x90 and the second 0x91. Parsing stops at the
 * final one even though the sector has room for more. */
static void test_elt_multiple_sections(void **state)
{
    elt_catalog c;

    (void)state;
    reset();
    build_brvd(CAT_LBA);
    build_validation(ELT_PLATFORM_X86);
    put_entry(1, 0x88, 0, 4, 100);
    put_header(2, 0x90, ELT_PLATFORM_PPC, 1);
    put_entry(3, 0x88, 0, 0, 200);
    put_header(4, 0x91, ELT_PLATFORM_UEFI, 1);
    put_entry(5, 0x88, 0, 0, 300);
    put_header(6, 0x90, ELT_PLATFORM_MAC, 1);  /* past the final header */
    put_entry(7, 0x88, 0, 0, 400);
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso), ELT_OK);
    assert_int_equal(c.n, 3);
    assert_int_equal(c.entry[1].platform, ELT_PLATFORM_PPC);
    assert_int_equal(c.entry[2].platform, ELT_PLATFORM_UEFI);
}

/* A header count is medium data. One that runs past the end of the sector is
 * a prefix, reported as truncated, never a read past the buffer. */
static void test_elt_section_count_overruns_sector(void **state)
{
    elt_catalog c;

    (void)state;
    reset();
    build_brvd(CAT_LBA);
    build_validation(ELT_PLATFORM_X86);
    put_entry(1, 0x88, 0, 4, 100);
    put_header(2, 0x91, ELT_PLATFORM_UEFI, 0xFFFFu);
    put_entry(3, 0x88, 0, 0, 200);
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso), ELT_OK);
    assert_int_equal(c.truncated, 1);
    /* Entry 3 is real and is kept; the rest of the sector is zeroes, which
     * are non-bootable entries, so nothing else is reported. */
    assert_int_equal(c.n, 2);
    assert_int_equal(c.entry[1].lba, 200);
}

/* More bootable entries than the fixed array holds. The kept ones are a
 * prefix and truncated says so. */
static void test_elt_more_entries_than_fit(void **state)
{
    elt_catalog c;
    unsigned i;

    (void)state;
    reset();
    build_brvd(CAT_LBA);
    build_validation(ELT_PLATFORM_X86);
    put_entry(1, 0x88, 0, 4, 100);
    put_header(2, 0x91, ELT_PLATFORM_UEFI, 20);
    for (i = 0; i < 20u; i++)
        put_entry(3 + i, 0x88, 0, 0, 200 + i);
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso), ELT_OK);
    assert_int_equal(c.n, ELT_MAX_ENTRIES);
    assert_int_equal(c.truncated, 1);
    assert_int_equal(c.entry[0].lba, 100);
    assert_int_equal(c.entry[1].lba, 200);
}

static void test_elt_not_an_iso(void **state)
{
    elt_catalog c;

    (void)state;
    reset();
    /* No descriptor at all. */
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso),
                     ELT_ERR_NOT_ISO);
    assert_int_equal(c.n, 0);

    /* A descriptor with the right magic but the wrong type. */
    build_brvd(CAT_LBA);
    g_iso[17u * SEC] = 1;
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso),
                     ELT_ERR_NOT_ISO);
}

/* The boot record slot is generic; the system identifier is what makes it El
 * Torito's. Somebody else's boot record is "no catalog", not damage. */
static void test_elt_boot_record_is_not_el_torito(void **state)
{
    elt_catalog c;

    (void)state;
    build_typical();
    memcpy(g_iso + 17u * SEC + 7, "SOMEONE ELSES BOOT RECOR", 23);
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso),
                     ELT_ERR_NO_CATALOG);
    assert_int_equal(c.n, 0);
}

static void test_elt_bad_validation_entry(void **state)
{
    elt_catalog c;

    (void)state;
    build_typical();
    cat()[0] = 2;                              /* wrong header id */
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso),
                     ELT_ERR_CORRUPT);

    build_typical();
    cat()[31] = 0x00;                          /* key is not 55 AA */
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso),
                     ELT_ERR_CORRUPT);
    assert_int_equal(c.n, 0);
}

/* A descriptor naming a sector past the end of the medium, and a read that
 * fails outright. Both are a medium claiming a catalog it cannot produce. */
static void test_elt_catalog_unreadable(void **state)
{
    elt_catalog c;

    (void)state;
    build_typical();
    build_brvd(SECTORS + 100u);
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso),
                     ELT_ERR_CORRUPT);
    assert_int_equal(c.n, 0);

    build_typical();
    g_fail_at = (int)CAT_LBA;
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso),
                     ELT_ERR_CORRUPT);

    /* A descriptor that cannot be read at all is not an ISO as far as this
     * module can tell, which is a different answer. */
    build_typical();
    g_fail_at = 17;
    assert_int_equal(elt_parse(&c, iso_read, NULL, sizeof g_iso),
                     ELT_ERR_NOT_ISO);
}

/* size 0 means the caller could not say how long the medium is, and the
 * callback's own failure becomes the only bound. */
static void test_elt_unknown_size(void **state)
{
    elt_catalog c;

    (void)state;
    build_typical();
    assert_int_equal(elt_parse(&c, iso_read, NULL, 0), ELT_OK);
    assert_int_equal(c.n, 2);

    build_brvd(SECTORS + 100u);
    assert_int_equal(elt_parse(&c, iso_read, NULL, 0), ELT_ERR_CORRUPT);
}

static void test_elt_arg_errors(void **state)
{
    elt_catalog c;

    (void)state;
    build_typical();
    assert_int_equal(elt_parse(NULL, iso_read, NULL, sizeof g_iso),
                     ELT_ERR_ARG);
    assert_int_equal(elt_parse(&c, NULL, NULL, sizeof g_iso), ELT_ERR_ARG);
    assert_int_equal(c.n, 0);
}

int run_eltorito_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_elt_typical),
        cmocka_unit_test(test_elt_sector_count_is_not_a_size),
        cmocka_unit_test(test_elt_platform_is_reported_not_filtered),
        cmocka_unit_test(test_elt_non_bootable_entries),
        cmocka_unit_test(test_elt_section_extension_is_skipped),
        cmocka_unit_test(test_elt_multiple_sections),
        cmocka_unit_test(test_elt_section_count_overruns_sector),
        cmocka_unit_test(test_elt_more_entries_than_fit),
        cmocka_unit_test(test_elt_not_an_iso),
        cmocka_unit_test(test_elt_boot_record_is_not_el_torito),
        cmocka_unit_test(test_elt_bad_validation_entry),
        cmocka_unit_test(test_elt_catalog_unreadable),
        cmocka_unit_test(test_elt_unknown_size),
        cmocka_unit_test(test_elt_arg_errors),
    };

    return cmocka_run_group_tests_name("eltorito", tests, NULL, NULL);
}
