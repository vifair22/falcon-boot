/* Tests for falcon-bios/src/hw/ata/atacore.c: the ATAPI command blocks
 * falcon jump's ATA.MOD sends and the answers it reads (DESIGN Section 14.4).
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <hw/ata/atacore.h>

static void test_cdbs(void **state)
{
    uint8_t cdb[ATAPI_CDB_LEN];
    static const uint8_t read10[ATAPI_CDB_LEN] = {
        0x28, 0, 0x01, 0x23, 0x45, 0x67, 0, 0x00, 0x20, 0, 0, 0
    };

    (void)state;
    memset(cdb, 0xAA, sizeof cdb);
    atapi_cdb_read10(cdb, 0x01234567u, 32u);
    assert_memory_equal(cdb, read10, sizeof cdb);

    memset(cdb, 0xAA, sizeof cdb);
    atapi_cdb_read_capacity(cdb);
    assert_int_equal(cdb[0], SCSI_READ_CAPACITY);
    assert_int_equal(cdb[11], 0);

    atapi_cdb_request_sense(cdb, 18u);
    assert_int_equal(cdb[0], SCSI_REQUEST_SENSE);
    assert_int_equal(cdb[4], 18);

    memset(cdb, 0xAA, sizeof cdb);
    atapi_cdb_test_unit_ready(cdb);
    assert_int_equal(cdb[0], 0);
    assert_int_equal(cdb[5], 0);
}

static void test_capacity(void **state)
{
    /* A 650 MB disc: last block 333,000 - 1, 2048 bytes each. */
    static const uint8_t cd[8] = { 0, 0x05, 0x14, 0xC7, 0, 0, 0x08, 0 };
    static const uint8_t disk[8] = { 0, 0, 0x10, 0, 0, 0, 0x02, 0 };
    static const uint8_t none[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0x08, 0 };
    uint64_t n = 0;

    (void)state;
    assert_int_equal(atapi_parse_capacity(cd, &n), 0);
    assert_int_equal(n, 0x514C8u);
    assert_int_equal(atapi_parse_capacity(disk, &n), -1);
    assert_int_equal(atapi_parse_capacity(none, &n), -1);
    assert_int_equal(atapi_parse_capacity(cd, NULL), -1);
}

static void test_sense(void **state)
{
    uint8_t s[18];

    (void)state;
    memset(s, 0, sizeof s);
    s[0] = 0x70;
    s[2] = SENSE_UNIT_ATTENTION;
    assert_int_equal(atapi_parse_sense(s, sizeof s), SENSE_UNIT_ATTENTION);
    s[0] = 0xF0;                        /* the valid bit set */
    s[2] = 0x20 | SENSE_NOT_READY;      /* ILI, and the key */
    assert_int_equal(atapi_parse_sense(s, sizeof s), SENSE_NOT_READY);
    s[0] = 0x72;                        /* descriptor format */
    assert_int_equal(atapi_parse_sense(s, sizeof s), -1);
    assert_int_equal(atapi_parse_sense(s, 2u), -1);
}

/* QEMU's ide-cd, as IDENTIFY PACKET DEVICE words describe it. */
static void test_identify(void **state)
{
    uint16_t w[256];
    char model[41];
    const char *name = "QEMU DVD-ROM";
    size_t i;

    (void)state;
    memset(w, 0, sizeof w);
    w[0] = 0x85C0u;                     /* ATAPI, CD-ROM, removable */
    for (i = 0; i < 40u; i += 2u) {
        char a = i < strlen(name) ? name[i] : ' ';
        char b = i + 1u < strlen(name) ? name[i + 1u] : ' ';

        w[27u + i / 2u] = (uint16_t)(((uint16_t)(uint8_t)a << 8) |
                                     (uint8_t)b);
    }
    assert_true(atapi_parse_identify(w, model, sizeof model));
    assert_string_equal(model, "QEMU DVD-ROM");

    /* A packet device that is not a CD (a ZIP drive, type 0). */
    w[0] = 0x8080u;
    assert_false(atapi_parse_identify(w, model, sizeof model));
    /* An ATA disk's word 0. */
    w[0] = 0x0040u;
    assert_false(atapi_parse_identify(w, model, sizeof model));

    /* A short buffer truncates rather than overruns. */
    w[0] = 0x85C0u;
    assert_true(atapi_parse_identify(w, model, 5u));
    assert_string_equal(model, "QEMU");
}

int run_atacore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_cdbs),
        cmocka_unit_test(test_capacity),
        cmocka_unit_test(test_sense),
        cmocka_unit_test(test_identify),
    };

    return cmocka_run_group_tests_name("atacore", tests, NULL, NULL);
}
