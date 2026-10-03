/* Tests for falcon-bios/src/power/acpicore.c: the S5 values the idle
 * power-off writes (DESIGN Section 7.9), from tables written here into a
 * megabyte of pretend physical memory: the RSDP where a BIOS puts it, the
 * RSDT or XSDT, the FADT and a DSDT with \_S5, each checksummed. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <power/acpicore.h>

#define MEM_LEN 0x100000u
#define RSDT    0x80000u
#define FADT    0x81000u
#define DSDT    0x82000u

static uint8_t g_mem[MEM_LEN];

static int mem_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    if (addr >= MEM_LEN || len > MEM_LEN - addr)
        return -1;
    memcpy(buf, g_mem + addr, len);
    return 0;
}

static void put32(uint32_t at, uint32_t v)
{
    g_mem[at] = (uint8_t)v;
    g_mem[at + 1] = (uint8_t)(v >> 8);
    g_mem[at + 2] = (uint8_t)(v >> 16);
    g_mem[at + 3] = (uint8_t)(v >> 24);
}

/* The byte at `fix` made whatever brings [at, at + len) to a zero sum. */
static void checksum(uint32_t at, uint32_t len, uint32_t fix)
{
    uint8_t s = 0;
    uint32_t i;

    g_mem[fix] = 0;
    for (i = 0; i < len; i++)
        s = (uint8_t)(s + g_mem[at + i]);
    g_mem[fix] = (uint8_t)(0u - s);
}

static void table(uint32_t at, const char *sig, uint32_t len)
{
    memcpy(g_mem + at, sig, 4);
    put32(at + 4, len);
    g_mem[at + 8] = 1;
}

/* A machine: RSDP at rsdp (revision rev), the system table, a FADT with
 * PM1a at 0x604 and SMI at 0xB2, and a DSDT whose \_S5 sits at `s5_at`
 * into it. */
static void machine(uint32_t rsdp, int rev, uint32_t s5_at,
                    const uint8_t *s5, size_t s5_len)
{
    uint32_t dlen = s5_at + (uint32_t)s5_len + 64u;

    memset(g_mem, 0, sizeof g_mem);
    memcpy(g_mem + rsdp, "RSD PTR ", 8);
    g_mem[rsdp + 15] = (uint8_t)rev;
    if (rev >= 2) {
        put32(rsdp + 20, 36);
        put32(rsdp + 24, RSDT);          /* an XSDT at the RSDT's place */
        table(RSDT, "XSDT", 36 + 8);
        put32(RSDT + 36, FADT);
        checksum(RSDT, 36 + 8, RSDT + 9);
        checksum(rsdp, 20, rsdp + 8);
        checksum(rsdp, 36, rsdp + 32);
    } else {
        put32(rsdp + 16, RSDT);
        table(RSDT, "RSDT", 36 + 8);
        put32(RSDT + 36, 0x90000u);      /* another table first */
        put32(RSDT + 40, FADT);
        checksum(RSDT, 36 + 8, RSDT + 9);
        checksum(rsdp, 20, rsdp + 8);
        table(0x90000u, "APIC", 44);
    }
    table(FADT, "FACP", 116);
    put32(FADT + 40, DSDT);
    put32(FADT + 48, 0xB2);
    g_mem[FADT + 52] = 0xF0;
    put32(FADT + 64, 0x604);
    checksum(FADT, 116, FADT + 9);

    table(DSDT, "DSDT", dlen);
    memcpy(g_mem + DSDT + s5_at, s5, s5_len);
    checksum(DSDT, dlen, DSDT + 9);
}

static const uint8_t k_s5_byte[] = {
    0x08, '_', 'S', '5', '_', 0x12, 0x08, 0x04, 0x0A, 0x05, 0x0A, 0x06,
    0x00, 0x00
};

static void test_find(void **state)
{
    acpi_s5 s;

    (void)state;
    machine(0xF0000u, 0, 100, k_s5_byte, sizeof k_s5_byte);
    assert_int_equal(acpi_find_s5(mem_read, NULL, 0, &s), ACPI_OK);
    assert_int_equal(s.pm1a_cnt, 0x604);
    assert_int_equal(s.pm1b_cnt, 0);
    assert_int_equal(s.smi_cmd, 0xB2);
    assert_int_equal(s.acpi_enable, 0xF0);
    assert_int_equal(s.slp_typa, 5);
    assert_int_equal(s.slp_typb, 6);
}

/* The name across the edge of the window the DSDT is read in. */
static void test_window_edge(void **state)
{
    acpi_s5 s;
    uint32_t at;

    (void)state;
    for (at = 36 + 470; at < 36 + 520; at++) {
        machine(0xE0010u, 0, at, k_s5_byte, sizeof k_s5_byte);
        assert_int_equal(acpi_find_s5(mem_read, NULL, 0, &s), ACPI_OK);
        assert_int_equal(s.slp_typa, 5);
        assert_int_equal(s.slp_typb, 6);
    }
}

/* The EBDA first, and an ACPI 2.0 RSDP with an XSDT. */
static void test_ebda_xsdt(void **state)
{
    acpi_s5 s;

    (void)state;
    machine(0x9FC00u, 2, 40, k_s5_byte, sizeof k_s5_byte);
    assert_int_equal(acpi_find_s5(mem_read, NULL, 0x9FC00u, &s), ACPI_OK);
    assert_int_equal(s.slp_typa, 5);
    /* Not looked for there without the EBDA's address. */
    assert_int_equal(acpi_find_s5(mem_read, NULL, 0, &s), ACPI_NO_RSDP);
}

static void test_refusals(void **state)
{
    acpi_s5 s;

    (void)state;
    machine(0xF0000u, 0, 100, k_s5_byte, sizeof k_s5_byte);
    g_mem[0xF0000u + 8] ^= 1u;          /* the RSDP's checksum */
    assert_int_equal(acpi_find_s5(mem_read, NULL, 0, &s), ACPI_NO_RSDP);

    machine(0xF0000u, 0, 100, k_s5_byte, sizeof k_s5_byte);
    g_mem[DSDT + 50] ^= 1u;             /* the DSDT's */
    assert_int_equal(acpi_find_s5(mem_read, NULL, 0, &s), ACPI_BAD_TABLE);

    machine(0xF0000u, 0, 100, k_s5_byte, sizeof k_s5_byte);
    put32(FADT + 64, 0);                /* no PM1a */
    checksum(FADT, 116, FADT + 9);
    assert_int_equal(acpi_find_s5(mem_read, NULL, 0, &s), ACPI_NO_FADT);

    machine(0xF0000u, 0, 100, (const uint8_t *)"\x08_S4_", 5);
    assert_int_equal(acpi_find_s5(mem_read, NULL, 0, &s), ACPI_NO_S5);
    assert_int_equal(acpi_find_s5(NULL, NULL, 0, &s), ACPI_NO_RSDP);
}

/* The package's integers as firmware writes them. */
static void test_parse(void **state)
{
    static const uint8_t zero_one[] = {
        0x08, 0x5C, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x00, 0x01
    };
    static const uint8_t word[] = {
        0x10, 0x08, '_', 'S', '5', '_', 0x12, 0x4A, 0x00, 0x04, 0x0B, 0x07,
        0x00, 0x0C, 0x03, 0x00, 0x00, 0x00
    };
    static const uint8_t one_int[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x03, 0x01, 0x0A, 0x06
    };
    static const uint8_t cut[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x0A, 0x06
    };
    static const uint8_t no_name[] = {
        0x14, '_', 'S', '5', '_', 0x12, 0x03, 0x01, 0x0A, 0x06
    };
    static const uint8_t no_pkg[] = {
        0x08, '_', 'S', '5', '_', 0x0A, 0x05
    };
    uint8_t a = 9, b = 9;

    (void)state;
    assert_int_equal(acpi_parse_s5(zero_one, sizeof zero_one, &a, &b),
                     ACPI_OK);
    assert_int_equal(a, 0);
    assert_int_equal(b, 1);
    /* A two-byte PkgLength, then WordPrefix and DWordPrefix. */
    assert_int_equal(acpi_parse_s5(word, sizeof word, &a, &b), ACPI_OK);
    assert_int_equal(a, 7);
    assert_int_equal(b, 3);
    assert_int_equal(acpi_parse_s5(one_int, sizeof one_int, &a, &b),
                     ACPI_OK);
    assert_int_equal(a, 6);
    assert_int_equal(b, 6);
    assert_int_equal(acpi_parse_s5(cut, sizeof cut, &a, &b), ACPI_NO_S5);
    assert_int_equal(acpi_parse_s5(no_name, sizeof no_name, &a, &b),
                     ACPI_NO_S5);
    assert_int_equal(acpi_parse_s5(no_pkg, sizeof no_pkg, &a, &b),
                     ACPI_NO_S5);
    assert_int_equal(acpi_parse_s5(NULL, 0, &a, &b), ACPI_NO_S5);

    assert_int_equal(acpi_s5_word(5), 0x3400);
    assert_int_equal(acpi_s5_word(0), 0x2000);
    assert_int_equal(acpi_s5_word(0xFF), 0x3C00);
}

/* Any table by signature: the MADT mp/mp.c asks for, the FADT, one that
 * is not there, and one whose checksum does not hold. */
static void test_find_table(void **state)
{
    uint32_t at = 0, len = 0;

    (void)state;
    machine(0xF0000u, 0, 100, k_s5_byte, sizeof k_s5_byte);
    checksum(0x90000u, 44, 0x90009u);
    assert_int_equal(acpi_find_table(mem_read, NULL, 0, "APIC", &at, &len),
                     ACPI_OK);
    assert_int_equal(at, 0x90000u);
    assert_int_equal(len, 44);
    assert_int_equal(acpi_find_table(mem_read, NULL, 0, "FACP", &at, &len),
                     ACPI_OK);
    assert_int_equal(at, FADT);
    assert_int_equal(len, 116);
    assert_int_equal(acpi_find_table(mem_read, NULL, 0, "HPET", &at, &len),
                     ACPI_NO_FADT);
    g_mem[0x90000u + 20] ^= 1;
    assert_int_equal(acpi_find_table(mem_read, NULL, 0, "APIC", &at, &len),
                     ACPI_BAD_TABLE);
    assert_int_equal(acpi_find_table(mem_read, NULL, 0, NULL, &at, &len),
                     ACPI_NO_RSDP);
    memset(g_mem, 0, sizeof g_mem);
    assert_int_equal(acpi_find_table(mem_read, NULL, 0, "APIC", &at, &len),
                     ACPI_NO_RSDP);
}

int run_acpicore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_find),
        cmocka_unit_test(test_window_edge),
        cmocka_unit_test(test_ebda_xsdt),
        cmocka_unit_test(test_refusals),
        cmocka_unit_test(test_parse),
        cmocka_unit_test(test_find_table),
    };

    return cmocka_run_group_tests_name("acpicore", tests, NULL, NULL);
}
