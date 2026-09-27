/* Tests for falcon-bios/src/disk/bootpart.c: which partition stage 2 takes
 * stage 3 from (docs/DESIGN.md Section 3.3).
 *
 * Every disk is built in memory, byte by byte, and its records written by
 * falcon-core's own encoder. What is checked is the choice: only a
 * FAT-typed partition's boot sector is asked, only a boot record counts,
 * the first in table order wins, a disk with no table is one volume from
 * LBA 0, and the reserved sectors bound stage 3.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>

#include <disk/bootpart.h>

#define BS 512u
#define BLOCKS 4096u

typedef struct {
    uint8_t buf[BLOCKS * BS];
    int     fail_lba;        /* -1 never; else a read starting at this block */
    int     reads;
} tdisk;

static int tdisk_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    tdisk *d = ctx;

    d->reads++;
    if (d->fail_lba >= 0 && off == (uint64_t)d->fail_lba * BS)
        return -1;
    if (off % BS != 0 || len % BS != 0 || off + len > sizeof d->buf)
        return -1;
    memcpy(buf, d->buf + off, (size_t)len);
    return 0;
}

static tdisk *tdisk_new(void)
{
    tdisk *d = calloc(1, sizeof *d);

    assert_non_null(d);
    d->fail_lba = -1;
    return d;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t ref_crc32(const uint8_t *p, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    unsigned b;

    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

static void mbr_entry(tdisk *d, unsigned slot, uint8_t type, uint32_t start,
                      uint32_t count)
{
    uint8_t *e = d->buf + 446 + slot * 16u;

    e[4] = type;
    put_le32(e + 8, start);
    put_le32(e + 12, count);
    d->buf[510] = 0x55;
    d->buf[511] = 0xAA;
}

/* A FAT boot sector at `lba`: the reserved count, 0x55AA, and a record of
 * `role` with install ID byte `id0`. role 0 writes no record. */
static void fat_boot(tdisk *d, uint32_t lba, uint16_t reserved,
                     ident_role role, uint8_t id0)
{
    uint8_t *s = d->buf + (size_t)lba * BS;
    ident_rec r;

    s[0] = 0xEB;
    s[1] = 0x58;
    s[2] = 0x90;
    s[0x0B] = 0x00;
    s[0x0C] = 0x02;
    s[0x0E] = (uint8_t)reserved;
    s[0x0F] = (uint8_t)(reserved >> 8);
    s[510] = 0x55;
    s[511] = 0xAA;
    if (role == 0)
        return;
    memset(&r, 0, sizeof r);
    r.version = IDENT_VERSION;
    r.role = role;
    if (role == IDENT_ROLE_BOOT) {
        r.stage3_sector = 32;
        r.marker_sector = 8;
    }
    memset(r.install_id, 0x5A, sizeof r.install_id);
    r.install_id[0] = id0;
    ident_encode_bin(&r, s);
}

/* A protective MBR and a primary GPT whose entries have the given type
 * GUIDs, each 64 blocks long from 2048 in 64-block steps. */
static void gpt_disk(tdisk *d, const uint8_t *const *types, unsigned n)
{
    uint8_t *hdr = d->buf + BS;
    uint8_t *arr = d->buf + 2u * BS;
    unsigned i, k;

    mbr_entry(d, 0, 0xEE, 1, BLOCKS - 1u);
    for (i = 0; i < n; i++) {
        uint8_t *e = arr + i * 128u;

        memcpy(e, types[i], 16);
        for (k = 0; k < 16; k++)
            e[16 + k] = (uint8_t)(0x10u * i + k + 1u);
        put_le64(e + 32, 2048u + 64u * i);
        put_le64(e + 40, 2048u + 64u * i + 63u);
    }
    memcpy(hdr, "EFI PART", 8);
    put_le32(hdr + 8, 0x00010000u);
    put_le32(hdr + 12, 92);
    put_le64(hdr + 24, 1);
    put_le64(hdr + 32, BLOCKS - 1u);
    put_le64(hdr + 40, 34);
    put_le64(hdr + 48, BLOCKS - 34u);
    hdr[56] = 0x42;
    put_le64(hdr + 72, 2);
    put_le32(hdr + 80, 128);
    put_le32(hdr + 84, 128);
    put_le32(hdr + 88, ref_crc32(arr, 128u * 128u));
    put_le32(hdr + 16, ref_crc32(hdr, 92));
}

/* A boot record whose stage 3 sector is `stage3`: 0 is an ESP's on a
 * station installed for UEFI as well (DESIGN Section 13.2). */
static void fat_boot_s3(tdisk *d, uint32_t lba, uint16_t reserved,
                        uint8_t id0, uint16_t stage3)
{
    uint8_t *s = d->buf + (size_t)lba * BS;
    ident_rec r;

    fat_boot(d, lba, reserved, 0, 0);
    memset(&r, 0, sizeof r);
    r.version = IDENT_VERSION;
    r.role = IDENT_ROLE_BOOT;
    r.stage3_sector = stage3;
    r.marker_sector = stage3 != 0 ? 8 : 0;
    memset(r.install_id, 0x5A, sizeof r.install_id);
    r.install_id[0] = id0;
    ident_encode_bin(&r, s);
}

static part_entry g_parts[16];
static uint8_t g_sec[BS];

static int find(tdisk *d, bootpart *bp)
{
    memset(bp, 0, sizeof *bp);
    return bootpart_find(tdisk_read, d, BS, BLOCKS, g_parts, 16, g_sec, bp);
}

static void test_candidates(void **state)
{
    static const uint8_t basic[16] = {
        0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
        0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7
    };
    static const uint8_t fat_types[] = {
        0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E, 0xEF,
        0x11, 0x14, 0x16, 0x1B, 0x1C, 0x1E
    };
    static const uint8_t other[] = { 0x05, 0x07, 0x0F, 0x17, 0x83, 0x85, 0xEE };
    part_entry p;
    size_t i;

    (void)state;
    memset(&p, 0, sizeof p);
    memcpy(p.type_guid, gpt_guid_esp, 16);
    assert_true(bootpart_candidate(&p));
    memcpy(p.type_guid, falcon_guid_boot, 16);
    assert_true(bootpart_candidate(&p));
    memcpy(p.type_guid, basic, 16);
    assert_false(bootpart_candidate(&p));

    memset(&p, 0, sizeof p);
    p.primary = 1;
    for (i = 0; i < sizeof fat_types; i++) {
        p.mbr_type = fat_types[i];
        assert_true(bootpart_candidate(&p));
    }
    for (i = 0; i < sizeof other; i++) {
        p.mbr_type = other[i];
        assert_false(bootpart_candidate(&p));
    }
    assert_false(bootpart_candidate(NULL));
}

/* The A21m's shape: one NTFS partition at 63, and the falcon partition
 * hidden FAT32 after it. A record on the NTFS boot sector is not read; a
 * plain FAT32 without one is passed over; the hidden one is taken. */
static void test_mbr(void **state)
{
    tdisk *d = tdisk_new();
    bootpart bp;

    (void)state;
    mbr_entry(d, 0, 0x07, 63, 1000);
    mbr_entry(d, 1, 0x0C, 1063, 100);
    mbr_entry(d, 2, 0x1C, 1163, 200);
    fat_boot(d, 63, 32, IDENT_ROLE_BOOT, 1);
    fat_boot(d, 1063, 32, 0, 0);
    fat_boot(d, 1163, 40, IDENT_ROLE_BOOT, 2);

    assert_int_equal(find(d, &bp), BOOTPART_OK);
    assert_int_equal(bp.start_lba, 1163);
    assert_int_equal(bp.number, 3);
    assert_int_equal(bp.rec.role, IDENT_ROLE_BOOT);
    assert_int_equal(bp.rec.install_id[0], 2);
    assert_int_equal(bp.rec.stage3_sector, 32);
    assert_int_equal(bp.rec.marker_sector, 8);
    /* The boot sector found is left in the buffer, for the BPB. */
    assert_int_equal(bootpart_stage3_room(g_sec, &bp.rec), 8);
    free(d);
}

/* A record of another role, or one whose CRC fails, is no boot record. The
 * first boot record in table order wins over a later one. */
static void test_mbr_roles_and_order(void **state)
{
    tdisk *d = tdisk_new();
    bootpart bp;

    (void)state;
    mbr_entry(d, 0, 0x0C, 100, 100);
    mbr_entry(d, 1, 0x0C, 200, 100);
    mbr_entry(d, 2, 0x0C, 300, 100);
    mbr_entry(d, 3, 0x0C, 400, 100);
    fat_boot(d, 100, 64, IDENT_ROLE_LIBRARY, 1);
    fat_boot(d, 200, 64, IDENT_ROLE_BOOT, 2);
    d->buf[200u * BS + IDENT_BIN_OFF + 12] ^= 0xFF;   /* the ID: CRC fails */
    fat_boot(d, 300, 64, IDENT_ROLE_BOOT, 3);
    fat_boot(d, 400, 64, IDENT_ROLE_BOOT, 4);

    assert_int_equal(find(d, &bp), BOOTPART_OK);
    assert_int_equal(bp.start_lba, 300);
    assert_int_equal(bp.number, 3);
    assert_int_equal(bp.rec.install_id[0], 3);

    /* No boot record anywhere. */
    fat_boot(d, 300, 64, IDENT_ROLE_SHELF, 3);
    fat_boot(d, 400, 64, 0, 0);
    memset(d->buf + 400u * BS + IDENT_BIN_OFF, 0, IDENT_BIN_LEN);
    assert_int_equal(find(d, &bp), BOOTPART_ERR_NONE);
    free(d);
}

/* A logical FAT partition inside an extended one is a candidate; the
 * container is not. */
static void test_mbr_logical(void **state)
{
    tdisk *d = tdisk_new();
    uint8_t *ebr;
    bootpart bp;

    (void)state;
    mbr_entry(d, 0, 0x0F, 1000, 2000);
    ebr = d->buf + 1000u * BS;
    ebr[446 + 4] = 0x0C;
    put_le32(ebr + 446 + 8, 63);
    put_le32(ebr + 446 + 12, 500);
    ebr[510] = 0x55;
    ebr[511] = 0xAA;
    fat_boot(d, 1063, 40, IDENT_ROLE_BOOT, 7);

    assert_int_equal(find(d, &bp), BOOTPART_OK);
    assert_int_equal(bp.start_lba, 1063);
    assert_int_equal(bp.number, 5);
    free(d);
}

/* jump's floppy: no table, the volume at LBA 0. Its record's magic lands in
 * the fourth entry's status byte, which is what keeps part_read_table from
 * reading the boot sector as an MBR. */
static void test_no_table(void **state)
{
    tdisk *d = tdisk_new();
    bootpart bp;

    (void)state;
    fat_boot(d, 0, 120, IDENT_ROLE_BOOT, 9);
    d->buf[IDENT_BIN_OFF + 6] = 98;      /* stage 3 at 98, as jump's */
    d->buf[IDENT_BIN_OFF + 7] = 0;
    d->buf[IDENT_BIN_OFF + 8] = 0;       /* no marker */
    put_le32(d->buf + IDENT_BIN_OFF + 28,
             ref_crc32(d->buf + IDENT_BIN_OFF, 28));

    assert_int_equal(find(d, &bp), BOOTPART_OK);
    assert_int_equal(bp.start_lba, 0);
    assert_int_equal(bp.number, 0);
    assert_int_equal(bp.rec.stage3_sector, 98);
    assert_int_equal(bp.rec.marker_sector, 0);
    assert_int_equal(bootpart_stage3_room(g_sec, &bp.rec), 22);

    /* A superfloppy with no record is not a boot volume. */
    memset(d->buf + IDENT_BIN_OFF, 0, IDENT_BIN_LEN);
    assert_int_equal(find(d, &bp), BOOTPART_ERR_NONE);
    free(d);
}

/* A stick: the ESP carries the record; a basic-data partition's record is
 * never read, and the falcon boot type counts as the ESP does. */
static void test_gpt(void **state)
{
    static const uint8_t basic[16] = {
        0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
        0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7
    };
    const uint8_t *types[3];
    tdisk *d = tdisk_new();
    bootpart bp;

    (void)state;
    types[0] = basic;
    types[1] = gpt_guid_esp;
    types[2] = falcon_guid_boot;
    gpt_disk(d, types, 3);
    fat_boot(d, 2048, 40, IDENT_ROLE_BOOT, 1);
    fat_boot(d, 2112, 40, 0, 0);
    fat_boot(d, 2176, 48, IDENT_ROLE_BOOT, 3);

    assert_int_equal(find(d, &bp), BOOTPART_OK);
    assert_int_equal(bp.start_lba, 2176);
    assert_int_equal(bp.number, 3);
    assert_int_equal(bootpart_stage3_room(g_sec, &bp.rec), 16);

    fat_boot(d, 2112, 40, IDENT_ROLE_BOOT, 2);
    assert_int_equal(find(d, &bp), BOOTPART_OK);
    assert_int_equal(bp.start_lba, 2112);
    assert_int_equal(bp.number, 2);
    free(d);
}

/* A GPT station installed for UEFI and BIOS both: the ESP first, its boot
 * record naming no stage 3, then the station partition with the real one.
 * Stage 2 takes the second; with the station partition gone there is no
 * stage 3 to take, whatever the ESP says. */
static void test_gpt_esp_without_stage3(void **state)
{
    const uint8_t *types[2];
    tdisk *d = tdisk_new();
    bootpart bp;

    (void)state;
    types[0] = gpt_guid_esp;
    types[1] = falcon_guid_boot;
    gpt_disk(d, types, 2);
    fat_boot_s3(d, 2048, 40, 7, 0);
    fat_boot_s3(d, 2112, 48, 7, 32);

    assert_int_equal(find(d, &bp), BOOTPART_OK);
    assert_int_equal(bp.start_lba, 2112);
    assert_int_equal(bp.number, 2);
    assert_int_equal(bp.rec.stage3_sector, 32);
    assert_int_equal(bootpart_stage3_room(g_sec, &bp.rec), 16);

    memset(d->buf + 2112u * BS, 0, BS);
    fat_boot(d, 2112, 48, 0, 0);
    assert_int_equal(find(d, &bp), BOOTPART_ERR_NONE);
    free(d);
}

static void test_errors(void **state)
{
    tdisk *d = tdisk_new();
    bootpart bp;

    (void)state;
    mbr_entry(d, 0, 0x0C, 100, 100);
    fat_boot(d, 100, 64, IDENT_ROLE_BOOT, 1);

    d->fail_lba = 100;
    assert_int_equal(find(d, &bp), BOOTPART_ERR_IO);
    d->fail_lba = 0;
    assert_int_equal(find(d, &bp), BOOTPART_ERR_IO);
    d->fail_lba = -1;

    /* Two entries sharing blocks: a table, and not a sane one. */
    mbr_entry(d, 1, 0x0C, 150, 100);
    assert_int_equal(find(d, &bp), BOOTPART_ERR_TABLE);

    assert_int_equal(bootpart_find(NULL, d, BS, 0, g_parts, 16, g_sec, &bp),
                     BOOTPART_ERR_ARG);
    assert_int_equal(bootpart_find(tdisk_read, d, BS, 0, g_parts, 0, g_sec,
                                   &bp), BOOTPART_ERR_ARG);
    assert_int_equal(bootpart_find(tdisk_read, d, 3, 0, g_parts, 16, g_sec,
                                   &bp), BOOTPART_ERR_ARG);
    free(d);
}

static void test_room(void **state)
{
    uint8_t sec[BS];
    ident_rec r;

    (void)state;
    memset(sec, 0, sizeof sec);
    memset(&r, 0, sizeof r);
    sec[0x0E] = 0x00;
    sec[0x0F] = 0x02;                    /* 512 reserved */
    r.stage3_sector = 32;
    assert_int_equal(bootpart_stage3_room(sec, &r), 480);
    r.stage3_sector = 512;
    assert_int_equal(bootpart_stage3_room(sec, &r), 0);
    r.stage3_sector = 0;
    assert_int_equal(bootpart_stage3_room(sec, &r), 0);
    assert_int_equal(bootpart_stage3_room(NULL, &r), 0);
    assert_int_equal(bootpart_stage3_room(sec, NULL), 0);
}

int run_bootpart_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_candidates),
        cmocka_unit_test(test_mbr),
        cmocka_unit_test(test_mbr_roles_and_order),
        cmocka_unit_test(test_mbr_logical),
        cmocka_unit_test(test_no_table),
        cmocka_unit_test(test_gpt),
        cmocka_unit_test(test_gpt_esp_without_stage3),
        cmocka_unit_test(test_errors),
        cmocka_unit_test(test_room),
    };

    return cmocka_run_group_tests_name("bootpart", tests, NULL, NULL);
}
