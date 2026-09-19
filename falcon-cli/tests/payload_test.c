/* Tests for falcon-cli/src/sign/payload.c, the bare-EFI-payload bless of
 * docs/DESIGN.md Section 7.2.
 *
 * What is worth testing here is not the signing - that is sbsign's, and
 * running it would test sbsign - but the three decisions around it whose
 * wrong answer is SILENT:
 *
 *   shape      which path a file takes, taken from its extension. A .iso that
 *              took the payload path would be signed as a bootloader and
 *              still refuse to boot.
 *   refusal    which families may be blessed as a bare payload at all. Saying
 *              yes to a Linux image would sign its loader, report success, and
 *              leave the kernel for the platform to refuse at boot with
 *              nothing on screen tying the two together.
 *   ownership  "is this already signed with your key", which decides whether
 *              a payload is touched at all. This one is tested hardest,
 *              because the obvious implementation of it is wrong: sbverify
 *              0.9.5 answers "Signature verification OK" for a
 *              Microsoft-signed binary against an unrelated certificate, so
 *              the check reads the PE's certificate table itself and looks
 *              for the certificate's DER. A false no here would append a
 *              second copy of our own certificate on every bless.
 *
 * The fourth thing a bless must get right, that the signatures already on a
 * third-party payload SURVIVE it, is checked against the real memtest image
 * rather than here: it is a property of what sbsign did, not of a decision
 * this file makes. What is here is the reading of a certificate table, which
 * is the input that check is made from.
 *
 * The PE here is synthetic and minimal: a DOS stub, a PE signature, an
 * optional header with a data directory, and a blob where the certificate
 * table would be. That is exactly the structure the check walks, and building
 * it by hand is what makes the malformed cases (a short directory array, a
 * table pointing past the end of the file) constructible at all.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sign/payload.h>
#include <media/family.h>

/* ------------------------------------------------------------ temp files */

static void temp_path(char *out, size_t cap, const char *tag)
{
    (void)snprintf(out, cap, "/tmp/falcon-payload-%ld-%s", (long)getpid(),
                   tag);
}

static void write_file(const char *path, const void *buf, size_t len)
{
    FILE *f = fopen(path, "wb");

    assert_non_null(f);
    if (len != 0)
        assert_int_equal(fwrite(buf, 1, len, f), len);
    assert_int_equal(fclose(f), 0);
}

/* ------------------------------------------------------------ a fake PE */

/* Offsets are the PE32+ ones the check uses: optional header at PE+24, the
 * directory count 108 bytes into it and the array 112. */
#define PE_OFF     0x80u
#define OPT_OFF    (PE_OFF + 24u)
#define NRVA_OFF   (OPT_OFF + 108u)
#define DIR_OFF    (OPT_OFF + 112u)
#define SEC_ENTRY  (DIR_OFF + 4u * 8u)
#define TABLE_OFF  0x400u
#define PE_MAX     4096u

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Builds a PE32+ whose certificate table is `table`, and writes it to path.
 * nrva is the NumberOfRvaAndSizes field, so a caller can build the "this
 * image has no directory 4 at all" case; table_off and table_len are what the
 * directory entry claims, so a caller can also point it past the end. */
static size_t build_pe(uint8_t *buf, uint32_t nrva, uint32_t table_off,
                       uint32_t table_len, const void *table, size_t table_n)
{
    size_t end = TABLE_OFF + table_n;

    memset(buf, 0, PE_MAX);
    buf[0] = 'M';
    buf[1] = 'Z';
    put_le32(buf + 0x3c, PE_OFF);
    memcpy(buf + PE_OFF, "PE\0\0", 4);
    buf[OPT_OFF] = 0x0b;          /* PE32+ magic, little-endian 0x20b */
    buf[OPT_OFF + 1u] = 0x02;
    put_le32(buf + NRVA_OFF, nrva);
    put_le32(buf + SEC_ENTRY, table_off);
    put_le32(buf + SEC_ENTRY + 4u, table_len);
    if (table_n != 0)
        memcpy(buf + TABLE_OFF, table, table_n);
    return end > PE_MAX ? PE_MAX : end;
}

/* ------------------------------------------------------------ shape */

static void test_payload_shape_by_extension(void **state)
{
    (void)state;
    assert_int_equal(sgn_payload_shape_of("memtest86.efi"), SGN_PAYLOAD_EFI);
    assert_int_equal(sgn_payload_shape_of("/mnt/p2/Other/Shell.EFI"),
                     SGN_PAYLOAD_EFI);
    assert_int_equal(sgn_payload_shape_of("memtest86-usb.img"),
                     SGN_PAYLOAD_IMG);
    assert_int_equal(sgn_payload_shape_of("/a/b/DISK.Img"), SGN_PAYLOAD_IMG);

    /* An ISO keeps the wrap path it has always had. */
    assert_int_equal(sgn_payload_shape_of("archlinux.iso"), SGN_PAYLOAD_NONE);
    /* The LAST extension decides, so a name that merely contains one is not
     * a payload. */
    assert_int_equal(sgn_payload_shape_of("shell.efi.iso"), SGN_PAYLOAD_NONE);
    assert_int_equal(sgn_payload_shape_of("notanextension"),
                     SGN_PAYLOAD_NONE);
    /* A dotfile called exactly ".efi" has no stem, so it is a file named
     * after the extension rather than a payload. */
    assert_int_equal(sgn_payload_shape_of(".efi"), SGN_PAYLOAD_NONE);
    assert_int_equal(sgn_payload_shape_of("/some/dir/.img"),
                     SGN_PAYLOAD_NONE);
    assert_int_equal(sgn_payload_shape_of(""), SGN_PAYLOAD_NONE);
    assert_int_equal(sgn_payload_shape_of(NULL), SGN_PAYLOAD_NONE);
}

/* ------------------------------------------------------------ refusal */

static void test_payload_refusal_allows_only_self_contained(void **state)
{
    size_t n = 0;
    const family_desc *tab = family_table(&n);
    size_t i;

    (void)state;
    /* The two that end at the payload. */
    assert_null(sgn_payload_refusal(FAMILY_MEMTEST));
    assert_null(sgn_payload_refusal(FAMILY_UNKNOWN));

    /* Every Linux family runs a kernel after the loader, so every one of them
     * is refused. Enumerated from the registry rather than listed, so a family
     * added later is refused by default rather than silently allowed. */
    for (i = 0; i < n; i++) {
        if (tab[i].id == FAMILY_MEMTEST || tab[i].id == FAMILY_UNKNOWN)
            continue;
        assert_non_null(sgn_payload_refusal(tab[i].id));
    }

    /* Windows is refused for its own reason and says so: its chain is
     * Microsoft-signed the whole way down, and it boots as it is. */
    assert_non_null(strstr(sgn_payload_refusal(FAMILY_WINDOWS),
                           "boots as it is"));
}

/* ------------------------------------------------------------ the cert */

static void test_payload_cert_der_decodes_pem(void **state)
{
    static const char pem[] =
        "some leading noise\n"
        "-----BEGIN CERTIFICATE-----\n"
        "AAECAwQF\n"
        "Bgc=\n"
        "-----END CERTIFICATE-----\n"
        "trailing noise\n";
    static const uint8_t want[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    char path[256];
    uint8_t *der = NULL;
    size_t len = 0;

    (void)state;
    temp_path(path, sizeof(path), "cert.pem");
    write_file(path, pem, sizeof(pem) - 1u);

    assert_int_equal(sgn_payload_cert_der(path, &der, &len), 0);
    assert_int_equal(len, sizeof(want));
    assert_memory_equal(der, want, sizeof(want));
    free(der);
    (void)unlink(path);
}

static void test_payload_cert_der_rejects_non_pem(void **state)
{
    static const char none[] = "not a certificate at all\n";
    static const char unterminated[] =
        "-----BEGIN CERTIFICATE-----\nAAEC\n";
    static const char empty[] =
        "-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----\n";
    char path[256];
    uint8_t *der = (uint8_t *)1;
    size_t len = 1;

    (void)state;
    temp_path(path, sizeof(path), "bad.pem");

    write_file(path, none, sizeof(none) - 1u);
    assert_int_not_equal(sgn_payload_cert_der(path, &der, &len), 0);
    assert_null(der);
    assert_int_equal(len, 0);

    write_file(path, unterminated, sizeof(unterminated) - 1u);
    assert_int_not_equal(sgn_payload_cert_der(path, &der, &len), 0);

    write_file(path, empty, sizeof(empty) - 1u);
    assert_int_not_equal(sgn_payload_cert_der(path, &der, &len), 0);

    (void)unlink(path);
    /* A path that is not there answers the same way, which is what lets the
     * caller report one missing-certificate error rather than two. */
    assert_int_not_equal(sgn_payload_cert_der(path, &der, &len), 0);
}

/* ------------------------------------------------------------ ownership */

static void test_payload_signed_with_finds_the_certificate(void **state)
{
    static const uint8_t der[] = { 0x30, 0x82, 0xAB, 0xCD, 0xEF, 0x01 };
    uint8_t table[64];
    uint8_t pe[PE_MAX];
    char path[256];
    size_t n;

    (void)state;
    temp_path(path, sizeof(path), "signed.efi");

    /* The DER sits inside the PKCS#7 blob rather than at its start, which is
     * why the check is a search and not a compare. */
    memset(table, 0x5A, sizeof(table));
    memcpy(table + 17u, der, sizeof(der));
    n = build_pe(pe, 16u, TABLE_OFF, (uint32_t)sizeof(table), table,
                 sizeof(table));
    write_file(path, pe, n);
    assert_int_equal(sgn_payload_signed_with(path, der, sizeof(der)), 1);

    /* Same image, a certificate it does not carry. */
    {
        static const uint8_t other[] = { 0x30, 0x82, 0x11, 0x22, 0x33, 0x44 };

        assert_int_equal(sgn_payload_signed_with(path, other, sizeof(other)),
                         0);
    }
    (void)unlink(path);
}

static void test_payload_signed_with_says_no_when_unsure(void **state)
{
    static const uint8_t der[] = { 0x30, 0x82, 0xAB, 0xCD };
    uint8_t table[32];
    uint8_t pe[PE_MAX];
    char path[256];
    size_t n;

    (void)state;
    temp_path(path, sizeof(path), "unsure.efi");
    memcpy(table, der, sizeof(der));
    memset(table + sizeof(der), 0, sizeof(table) - sizeof(der));

    /* An unsigned image: the directory entry is there and empty. */
    n = build_pe(pe, 16u, 0u, 0u, table, sizeof(table));
    write_file(path, pe, n);
    assert_int_equal(sgn_payload_signed_with(path, der, sizeof(der)), 0);

    /* An image whose directory array is too short to have a security entry
     * at all, which is an answer rather than a parse failure. */
    n = build_pe(pe, 4u, TABLE_OFF, (uint32_t)sizeof(table), table,
                 sizeof(table));
    write_file(path, pe, n);
    assert_int_equal(sgn_payload_signed_with(path, der, sizeof(der)), 0);

    /* A table the headers point past the end of the file: hostile or
     * corrupt, and either way not a yes. */
    n = build_pe(pe, 16u, 0x100000u, (uint32_t)sizeof(table), table,
                 sizeof(table));
    write_file(path, pe, n);
    assert_int_equal(sgn_payload_signed_with(path, der, sizeof(der)), 0);

    /* Not a PE. */
    write_file(path, "this is not a PE image", 22u);
    assert_int_equal(sgn_payload_signed_with(path, der, sizeof(der)), 0);

    /* Not a file. */
    (void)unlink(path);
    assert_int_equal(sgn_payload_signed_with(path, der, sizeof(der)), 0);

    /* No certificate to look for. */
    assert_int_equal(sgn_payload_signed_with(path, der, 0), 0);
}

/* A table longer than the check will read is refused rather than truncated:
 * a partial search could answer "not signed with your key" about an image
 * that is, and the caller would then sign it a second time. */
static void test_payload_signed_with_refuses_an_absurd_table(void **state)
{
    static const uint8_t der[] = { 0x30, 0x82, 0x00, 0x01 };
    uint8_t pe[PE_MAX];
    char path[256];
    size_t n;

    (void)state;
    temp_path(path, sizeof(path), "absurd.efi");
    n = build_pe(pe, 16u, TABLE_OFF, 0xFFFFFFFu, der, sizeof(der));
    write_file(path, pe, n);
    assert_int_equal(sgn_payload_signed_with(path, der, sizeof(der)), 0);
    (void)unlink(path);
}

/* The table is read back byte for byte, because the bless keeps the one it
 * found before signing and looks for it inside the one it produced: a read
 * that was off by a header, or that stopped short, would turn "the vendor's
 * signature survived" into a check that cannot pass. */
static void test_payload_cert_table_is_read_exactly(void **state)
{
    uint8_t table[96];
    uint8_t pe[PE_MAX];
    uint8_t *got = (uint8_t *)1;
    size_t got_len = 1;
    char path[256];
    size_t n;
    size_t i;

    (void)state;
    temp_path(path, sizeof(path), "table.efi");
    for (i = 0; i < sizeof(table); i++)
        table[i] = (uint8_t)(i * 7u + 3u);

    n = build_pe(pe, 16u, TABLE_OFF, (uint32_t)sizeof(table), table,
                 sizeof(table));
    write_file(path, pe, n);
    assert_int_equal(sgn_payload_cert_table(path, &got, &got_len), 0);
    assert_non_null(got);
    assert_int_equal(got_len, sizeof(table));
    assert_memory_equal(got, table, sizeof(table));
    free(got);

    /* An unsigned image: no table is an answer, not an error, so the caller
     * has nothing to preserve and no failure to report. */
    n = build_pe(pe, 16u, 0u, 0u, table, sizeof(table));
    write_file(path, pe, n);
    got = (uint8_t *)1;
    got_len = 1;
    assert_int_equal(sgn_payload_cert_table(path, &got, &got_len), 0);
    assert_null(got);
    assert_int_equal(got_len, 0);

    /* A table that does not fit in the file is refused rather than returned
     * short, because a short read would compare equal to nothing. */
    n = build_pe(pe, 16u, TABLE_OFF, 0x1000u, table, sizeof(table));
    write_file(path, pe, n);
    assert_int_not_equal(sgn_payload_cert_table(path, &got, &got_len), 0);

    (void)unlink(path);
}

/* ------------------------------------------------------------------ run */

int run_payload_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_payload_shape_by_extension),
        cmocka_unit_test(test_payload_refusal_allows_only_self_contained),
        cmocka_unit_test(test_payload_cert_der_decodes_pem),
        cmocka_unit_test(test_payload_cert_der_rejects_non_pem),
        cmocka_unit_test(test_payload_signed_with_finds_the_certificate),
        cmocka_unit_test(test_payload_signed_with_says_no_when_unsure),
        cmocka_unit_test(test_payload_signed_with_refuses_an_absurd_table),
        cmocka_unit_test(test_payload_cert_table_is_read_exactly),
    };

    return cmocka_run_group_tests_name("bare EFI payloads", tests, NULL,
                                       NULL);
}
