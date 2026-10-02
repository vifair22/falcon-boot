/* Tests for falcon-bios/src/hw/ata/atacore.c: the ATAPI command blocks
 * falcon jump's ATA.MOD sends and the answers it reads (DESIGN Section 13.4).
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
    (void)scsi_cdb_read10(cdb, 0x01234567u, 32u);
    assert_memory_equal(cdb, read10, sizeof cdb);

    memset(cdb, 0xAA, sizeof cdb);
    (void)scsi_cdb_read_capacity(cdb);
    assert_int_equal(cdb[0], SCSI_READ_CAPACITY);
    assert_int_equal(cdb[11], 0);

    (void)scsi_cdb_request_sense(cdb, 18u);
    assert_int_equal(cdb[0], SCSI_REQUEST_SENSE);
    assert_int_equal(cdb[4], 18);

    memset(cdb, 0xAA, sizeof cdb);
    (void)scsi_cdb_test_unit_ready(cdb);
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
    assert_int_equal(scsi_parse_sense(s, sizeof s), SENSE_UNIT_ATTENTION);
    s[0] = 0xF0;                        /* the valid bit set */
    s[2] = 0x20 | SENSE_NOT_READY;      /* ILI, and the key */
    assert_int_equal(scsi_parse_sense(s, sizeof s), SENSE_NOT_READY);
    s[0] = 0x72;                        /* descriptor format */
    assert_int_equal(scsi_parse_sense(s, sizeof s), -1);
    assert_int_equal(scsi_parse_sense(s, 2u), -1);
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

/* A CompactFlash card's IDENTIFY: 0x848A, LBA, 1 GB. */
static void test_identify_disk(void **state)
{
    uint16_t w[256];
    uint64_t n = 0;
    int big = 1;
    char model[41];

    (void)state;
    memset(w, 0, sizeof w);
    w[0] = 0x848Au;
    w[49] = 1u << 9;
    w[60] = 0x0000u;
    w[61] = 0x0020u;                    /* 0x200000 sectors */
    w[27] = (uint16_t)(('C' << 8) | 'F');
    assert_int_equal(ata_parse_identify_disk(w, &n, &big, model, sizeof model), 0);
    assert_int_equal(n, 0x200000u);
    assert_int_equal(big, 0);
    assert_string_equal(model, "CF");

    w[0] = 0x0040u;                     /* an ordinary fixed disk */
    assert_int_equal(ata_parse_identify_disk(w, &n, &big, model, sizeof model), 0);
    w[0] = 0x85C0u;                     /* a CD drive */
    assert_int_equal(ata_parse_identify_disk(w, &n, &big, model, sizeof model), -1);
    w[0] = 0x0040u;
    w[49] = 0;                          /* CHS only */
    assert_int_equal(ata_parse_identify_disk(w, &n, &big, model, sizeof model), -1);
    w[49] = 1u << 9;
    w[60] = 0;
    w[61] = 0;
    assert_int_equal(ata_parse_identify_disk(w, &n, &big, model, sizeof model), -1);
}

/* A disk past 128 GiB: words 60-61 stop at 0x0FFFFFFF and the 48-bit
 * count is in words 100-103, used only when words 83 and 86 say the
 * feature set is there and on. */
static void test_identify_lba48(void **state)
{
    uint16_t w[256];
    uint64_t n = 0;
    int big = 0;

    (void)state;
    memset(w, 0, sizeof w);
    w[0] = 0x0040u;
    w[49] = 1u << 9;
    w[60] = 0xFFFFu;
    w[61] = 0x0FFFu;
    w[83] = 0x4400u;                    /* valid, 48-bit supported */
    w[86] = 0x0400u;                    /* 48-bit enabled */
    w[100] = 0x0000u;
    w[101] = 0x1020u;                   /* 0x10200000: 129 GiB */
    assert_int_equal(ata_parse_identify_disk(w, &n, &big, NULL, 0), 0);
    assert_int_equal(n, 0x10200000u);
    assert_int_equal(big, 1);

    /* Words above 2^32 sectors. */
    w[102] = 0x0001u;
    assert_int_equal(ata_parse_identify_disk(w, &n, &big, NULL, 0), 0);
    assert_true(n == 0x110200000ull);
    w[102] = 0;

    /* Supported but not enabled: the 28-bit count, no 48-bit commands. */
    w[86] = 0;
    assert_int_equal(ata_parse_identify_disk(w, &n, &big, NULL, 0), 0);
    assert_int_equal(n, 0x0FFFFFFFu);
    assert_int_equal(big, 0);

    /* Word 83 not valid (bits 15-14 not 01b): its bit 10 means nothing. */
    w[86] = 0x0400u;
    w[83] = 0xC400u;
    assert_int_equal(ata_parse_identify_disk(w, &n, &big, NULL, 0), 0);
    assert_int_equal(big, 0);
    w[83] = 0x0400u;
    assert_int_equal(ata_parse_identify_disk(w, &n, &big, NULL, 0), 0);
    assert_int_equal(big, 0);

    /* 48-bit, but no 48-bit count: taken at its 28-bit one. */
    w[83] = 0x4400u;
    w[100] = w[101] = 0;
    assert_int_equal(ata_parse_identify_disk(w, &n, &big, NULL, 0), 0);
    assert_int_equal(n, 0x0FFFFFFFu);
    assert_int_equal(big, 1);

    assert_int_equal(ata_parse_identify_disk(w, &n, NULL, NULL, 0), -1);
}

static void test_addressing(void **state)
{
    (void)state;
    /* Below 128 GiB, LBA28 whether or not the disk has 48-bit. */
    assert_int_equal(ata_addressing(0, 0, 1), ATA_ADDR_LBA28);
    assert_int_equal(ata_addressing(1, 0, 256), ATA_ADDR_LBA28);
    assert_int_equal(ata_addressing(1, 0x0FFFFF00u, 255), ATA_ADDR_LBA28);
    /* The last sector LBA28 reaches is 0x0FFFFFFE. */
    assert_int_equal(ata_addressing(0, 0x0FFFFFFEu, 1), ATA_ADDR_LBA28);
    assert_int_equal(ata_addressing(0, 0x0FFFFFFFu, 1), ATA_ADDR_NONE);
    assert_int_equal(ata_addressing(1, 0x0FFFFFFFu, 1), ATA_ADDR_LBA48);
    /* A transfer that crosses the line, or starts past it. */
    assert_int_equal(ata_addressing(1, 0x0FFFFF80u, 256), ATA_ADDR_LBA48);
    assert_int_equal(ata_addressing(0, 0x0FFFFF80u, 256), ATA_ADDR_NONE);
    assert_int_equal(ata_addressing(1, 0x10200000u, 8), ATA_ADDR_LBA48);
    assert_int_equal(ata_addressing(0, 0x10200000u, 8), ATA_ADDR_NONE);
    /* More than 256 sectors needs 48-bit's count. */
    assert_int_equal(ata_addressing(1, 0, 257), ATA_ADDR_LBA48);
    assert_int_equal(ata_addressing(0, 0, 257), ATA_ADDR_NONE);
    assert_int_equal(ata_addressing(1, 0, 65536), ATA_ADDR_LBA48);
    assert_int_equal(ata_addressing(1, 0, 65537), ATA_ADDR_NONE);
    /* 48 bits is the end. */
    assert_int_equal(ata_addressing(1, ((uint64_t)1 << 48) - 1u, 1),
                     ATA_ADDR_LBA48);
    assert_int_equal(ata_addressing(1, ((uint64_t)1 << 48) - 1u, 2),
                     ATA_ADDR_NONE);
    assert_int_equal(ata_addressing(1, 0, 0), ATA_ADDR_NONE);
}

/* The channels a controller's programming interface and BARs name. */
static void test_channels(void **state)
{
    ata_ide_ctrl c[2];
    ata_chan ch[8];
    unsigned n;

    (void)state;
    /* No PCI IDE controller: the legacy pair. */
    n = ata_channel_list(NULL, 0, ch, 8u);
    assert_int_equal(n, 2);
    assert_int_equal(ch[0].base, 0x1F0);
    assert_int_equal(ch[0].ctrl, 0x3F6);
    assert_int_equal(ch[0].mode, ATA_CHAN_COMPAT);
    assert_int_equal(ch[0].ctrl_idx, ATA_CHAN_NO_CTRL);
    assert_int_equal(ch[0].channel, 0);
    assert_int_equal(ch[1].base, 0x170);
    assert_int_equal(ch[1].ctrl, 0x376);
    assert_int_equal(ch[1].channel, 1);
    assert_int_equal(ata_channel_list(NULL, 0, ch, 1u), 1);
    assert_int_equal(ata_channel_list(NULL, 0, NULL, 8u), 0);

    /* A PIIX3 as QEMU's pc machine carries it: both channels in
     * compatibility mode (0x80, bus master only), its BARs unused. */
    memset(c, 0, sizeof c);
    c[0].progif = 0x80;
    n = ata_channel_list(c, 1u, ch, 8u);
    assert_int_equal(n, 2);
    assert_int_equal(ch[0].mode, ATA_CHAN_COMPAT);
    assert_int_equal(ch[0].base, 0x1F0);
    assert_int_equal(ch[0].ctrl_idx, 0);
    assert_int_equal(ch[1].mode, ATA_CHAN_COMPAT);
    assert_int_equal(ch[1].base, 0x170);
    assert_int_equal(ch[1].ctrl, 0x376);

    /* A CMD646 (0x8F): both native, at the ports the BIOS put in its
     * BARs, the control register two past BAR1 and BAR3. */
    c[0].progif = 0x8F;
    c[0].bar[0] = 0xC001;
    c[0].bar[1] = 0xC011;
    c[0].bar[2] = 0xC021;
    c[0].bar[3] = 0xC031;
    n = ata_channel_list(c, 1u, ch, 8u);
    assert_int_equal(n, 2);
    assert_int_equal(ch[0].mode, ATA_CHAN_NATIVE);
    assert_int_equal(ch[0].base, 0xC000);
    assert_int_equal(ch[0].ctrl, 0xC012);
    assert_int_equal(ch[0].channel, 0);
    assert_int_equal(ch[1].mode, ATA_CHAN_NATIVE);
    assert_int_equal(ch[1].base, 0xC020);
    assert_int_equal(ch[1].ctrl, 0xC032);
    assert_int_equal(ch[1].channel, 1);

    /* Mixed: the primary native, the secondary compatibility (0x8B: bits 1
     * and 3 say only that a channel could switch). */
    c[0].progif = 0x8B;
    n = ata_channel_list(c, 1u, ch, 8u);
    assert_int_equal(n, 2);
    assert_int_equal(ch[0].mode, ATA_CHAN_NATIVE);
    assert_int_equal(ch[0].base, 0xC000);
    assert_int_equal(ch[1].mode, ATA_CHAN_COMPAT);
    assert_int_equal(ch[1].base, 0x170);
    c[0].progif = 0x84;
    n = ata_channel_list(c, 1u, ch, 8u);
    assert_int_equal(n, 2);
    assert_int_equal(ch[0].mode, ATA_CHAN_COMPAT);
    assert_int_equal(ch[0].base, 0x1F0);
    assert_int_equal(ch[1].mode, ATA_CHAN_NATIVE);
    assert_int_equal(ch[1].base, 0xC020);

    /* A native channel the BIOS assigned no port: listed unassigned, with
     * no ports, so it is said and not probed. A zero BAR, a memory BAR, a
     * control BAR missing, and a port past the 16-bit space. */
    c[0].progif = 0x8F;
    c[0].bar[0] = 0;
    n = ata_channel_list(c, 1u, ch, 8u);
    assert_int_equal(n, 2);
    assert_int_equal(ch[0].mode, ATA_CHAN_UNASSIGNED);
    assert_int_equal(ch[0].base, 0);
    assert_int_equal(ch[0].ctrl, 0);
    assert_int_equal(ch[0].ctrl_idx, 0);
    assert_int_equal(ch[0].channel, 0);
    assert_int_equal(ch[1].mode, ATA_CHAN_NATIVE);
    assert_int_equal(ch[1].base, 0xC020);
    c[0].bar[0] = 0xC000;               /* memory space */
    assert_int_equal(ata_channel_list(c, 1u, ch, 8u), 2);
    assert_int_equal(ch[0].mode, ATA_CHAN_UNASSIGNED);
    c[0].bar[0] = 0xC001;
    c[0].bar[1] = 0;
    assert_int_equal(ata_channel_list(c, 1u, ch, 8u), 2);
    assert_int_equal(ch[0].mode, ATA_CHAN_UNASSIGNED);
    c[0].bar[1] = 0x1C011;
    assert_int_equal(ata_channel_list(c, 1u, ch, 8u), 2);
    assert_int_equal(ch[0].mode, ATA_CHAN_UNASSIGNED);

    /* Two controllers both in compatibility mode: the legacy pair once. */
    memset(c, 0, sizeof c);
    c[0].progif = 0x80;
    c[1].progif = 0x00;
    n = ata_channel_list(c, 2u, ch, 8u);
    assert_int_equal(n, 2);
    assert_int_equal(ch[0].base, 0x1F0);
    assert_int_equal(ch[0].ctrl_idx, 0);
    assert_int_equal(ch[1].base, 0x170);
    assert_int_equal(ch[1].ctrl_idx, 0);

    /* One in compatibility mode and one native: four channels, the
     * native ones marked with their controller. */
    c[1].progif = 0x8F;
    c[1].bar[0] = 0xC001;
    c[1].bar[1] = 0xC011;
    c[1].bar[2] = 0xC021;
    c[1].bar[3] = 0xC031;
    n = ata_channel_list(c, 2u, ch, 8u);
    assert_int_equal(n, 4);
    assert_int_equal(ch[2].mode, ATA_CHAN_NATIVE);
    assert_int_equal(ch[2].base, 0xC000);
    assert_int_equal(ch[2].ctrl_idx, 1);
    assert_int_equal(ch[3].base, 0xC020);
    assert_int_equal(ch[3].ctrl_idx, 1);
    /* The list stops at cap. */
    assert_int_equal(ata_channel_list(c, 2u, ch, 3u), 3);
    /* Two unassigned channels are both listed: each is said. */
    c[0].progif = 0x8F;
    n = ata_channel_list(c, 2u, ch, 8u);
    assert_int_equal(n, 4);
    assert_int_equal(ch[0].mode, ATA_CHAN_UNASSIGNED);
    assert_int_equal(ch[1].mode, ATA_CHAN_UNASSIGNED);
    assert_int_equal(ch[2].mode, ATA_CHAN_NATIVE);
}

int run_atacore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_cdbs),
        cmocka_unit_test(test_capacity),
        cmocka_unit_test(test_sense),
        cmocka_unit_test(test_identify),
        cmocka_unit_test(test_identify_disk),
        cmocka_unit_test(test_identify_lba48),
        cmocka_unit_test(test_addressing),
        cmocka_unit_test(test_channels),
    };

    return cmocka_run_group_tests_name("atacore", tests, NULL, NULL);
}
