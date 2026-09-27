/* Tests for falcon-cli/src/mkstick/records.c and nextcore.c: the identity
 * records as mkstick reads and writes them, and the Linux half of the
 * next-boot marker.
 *
 * records_read and records_write_boot are run over a small image written
 * here: an MBR disk with a FAT32 partition (the boot partition) and a
 * partition that is not FAT, so the boot record's write, its FAT32 backup,
 * and the reader's narrowing by type are all checked against real bytes.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ident/ident.h>
#include <media/nextboot.h>
#include <mkstick/bioscore.h>
#include <mkstick/nextcore.h>
#include <mkstick/records.h>
#include <part/part.h>
#include <part/table.h>

#define IMG_SECTORS 8192u
#define P1_START    2048u
#define P1_SECTORS  4096u
#define P2_START    6144u
#define P2_SECTORS  2048u
#define RESERVED    2080u

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void mbr_entry(uint8_t *mbr, unsigned slot, uint8_t type,
                      uint32_t start, uint32_t count)
{
    uint8_t *e = mbr + 446 + slot * 16u;

    e[4] = type;
    put_le32(e + 8, start);
    put_le32(e + 12, count);
}

/* An image file: MBR, p1 a FAT32 volume (type 0x0C) as far as its boot
 * sector goes, p2 type 0x83 with nothing in it. Returns its path. */
static char *make_image(void)
{
    static char path[64];
    uint8_t sec[512];
    int fd;

    (void)snprintf(path, sizeof path, "/tmp/falcon-records-test.XXXXXX");
    fd = mkstemp(path);
    assert_true(fd >= 0);
    assert_int_equal(ftruncate(fd, (off_t)IMG_SECTORS * 512), 0);

    memset(sec, 0, sizeof sec);
    mbr_entry(sec, 0, 0x0C, P1_START, P1_SECTORS);
    mbr_entry(sec, 1, 0x83, P2_START, P2_SECTORS);
    sec[440] = 0x78;                       /* a disk signature */
    sec[510] = 0x55;
    sec[511] = 0xAA;
    assert_int_equal(pwrite(fd, sec, sizeof sec, 0), 512);

    memset(sec, 0, sizeof sec);
    sec[0] = 0xEB;
    sec[1] = 0x58;
    sec[2] = 0x90;
    sec[0x0C] = 0x02;
    sec[0x0D] = 1;
    sec[0x0E] = (uint8_t)(RESERVED & 0xFFu);
    sec[0x0F] = (uint8_t)(RESERVED >> 8);
    sec[0x10] = 2;
    sec[0x30] = 1;
    sec[0x32] = 6;
    sec[510] = 0x55;
    sec[511] = 0xAA;
    assert_int_equal(pwrite(fd, sec, sizeof sec, (off_t)P1_START * 512), 512);
    assert_int_equal(pwrite(fd, sec, sizeof sec, (off_t)(P1_START + 6) * 512),
                     512);
    /* The same boot sector on p2: a FAT the loaders never read for a boot
     * record, because the MBR type says it is not FAT. */
    assert_int_equal(pwrite(fd, sec, sizeof sec, (off_t)P2_START * 512), 512);
    assert_int_equal(pwrite(fd, sec, sizeof sec, (off_t)(P2_START + 6) * 512),
                     512);
    (void)close(fd);
    return path;
}

static void boot_rec(ident_rec *r, uint8_t fill)
{
    memset(r, 0, sizeof *r);
    r->version = IDENT_VERSION;
    r->role = IDENT_ROLE_BOOT;
    r->stage3_sector = 32;
    r->marker_sector = 8;
    memset(r->install_id, fill, sizeof r->install_id);
}

static void test_write_and_read(void **state)
{
    static mks_records r;
    char *img = make_image();
    uint8_t sec[512];
    ident_rec rec;
    ident_rec got;
    int fd;

    (void)state;
    assert_int_equal(records_read(&r, img, 512, (uint64_t)IMG_SECTORS * 512),
                     CLI_OK);
    assert_int_equal(r.rc, PART_OK);
    assert_int_equal(r.table.count, 2);
    assert_int_equal(records_boot(&r), -1);
    assert_int_equal(r.recs[0].state, REC_NONE);

    boot_rec(&rec, 0xA5);
    assert_int_equal(records_write_boot(img, (uint64_t)P1_START * 512, &rec),
                     CLI_OK);
    assert_int_equal(records_write_boot(img, (uint64_t)P2_START * 512, &rec),
                     CLI_OK);

    /* Both copies carry it, and the rest of the boot sector is untouched. */
    fd = open(img, O_RDONLY);
    assert_true(fd >= 0);
    assert_int_equal(pread(fd, sec, sizeof sec, (off_t)(P1_START + 6) * 512),
                     512);
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_OK);
    assert_true(ident_same_install(&rec, &got));
    assert_int_equal(pread(fd, sec, sizeof sec, (off_t)P1_START * 512), 512);
    assert_int_equal(sec[0x0E], (uint8_t)(RESERVED & 0xFFu));
    assert_int_equal(sec[510], 0x55);
    (void)close(fd);

    assert_int_equal(records_read(&r, img, 512, (uint64_t)IMG_SECTORS * 512),
                     CLI_OK);
    assert_int_equal(records_boot(&r), 0);
    assert_int_equal(r.recs[0].state, REC_OK);
    assert_string_equal(r.recs[0].where, "boot sector");
    assert_int_equal(r.recs[0].rec.stage3_sector, 32);
    assert_int_equal(r.recs[0].rec.marker_sector, 8);
    /* p2 carries a boot record too, but its type rules it out. */
    assert_int_equal(r.recs[1].state, REC_OK);
    assert_int_equal(records_find(&r, IDENT_ROLE_BOOT, 1), 1);
    assert_int_equal(records_find(&r, IDENT_ROLE_SHELF, 0), -1);
    assert_true(records_boot_candidate(&r.table, &r.parts[0]));
    assert_false(records_boot_candidate(&r.table, &r.parts[1]));

    /* A corrupted record reads as bad, not as absent. */
    fd = open(img, O_RDWR);
    assert_true(fd >= 0);
    assert_int_equal(pread(fd, sec, sizeof sec, (off_t)P1_START * 512), 512);
    sec[IDENT_BIN_OFF + 20u] ^= 0xFFu;
    assert_int_equal(pwrite(fd, sec, sizeof sec, (off_t)P1_START * 512), 512);
    (void)close(fd);
    assert_int_equal(records_read(&r, img, 512, (uint64_t)IMG_SECTORS * 512),
                     CLI_OK);
    assert_int_equal(r.recs[0].state, REC_BAD);
    assert_int_equal(records_boot(&r), -1);

    (void)unlink(img);
}

static void test_write_refusals(void **state)
{
    char *img = make_image();
    ident_rec rec;

    (void)state;
    boot_rec(&rec, 1);

    /* LBA 0 is an MBR, not a FAT boot sector. */
    assert_int_not_equal(records_write_boot(img, 0, &rec), CLI_OK);

    /* A record whose stage 3 is past the reserved area. */
    rec.stage3_sector = RESERVED;
    assert_int_not_equal(records_write_boot(img, (uint64_t)P1_START * 512,
                                            &rec), CLI_OK);

    /* Only boot records are binary. */
    boot_rec(&rec, 1);
    rec.role = IDENT_ROLE_LIBRARY;
    assert_int_not_equal(records_write_boot(img, (uint64_t)P1_START * 512,
                                            &rec), CLI_OK);
    assert_int_not_equal(records_write_boot("/nonexistent/falcon", 0, &rec),
                         CLI_OK);
    (void)unlink(img);
}

static void test_no_table(void **state)
{
    static mks_records r;
    char path[] = "/tmp/falcon-records-empty.XXXXXX";
    int fd = mkstemp(path);

    (void)state;
    assert_true(fd >= 0);
    assert_int_equal(ftruncate(fd, 1024 * 1024), 0);
    (void)close(fd);
    assert_int_equal(records_read(&r, path, 512, 1024 * 1024), CLI_OK);
    assert_int_equal(r.rc, PART_ERR_NO_TABLE);
    assert_int_equal(r.table.count, 0);
    assert_int_not_equal(records_read(&r, "/nonexistent/falcon", 512, 512),
                         CLI_OK);
    (void)unlink(path);
}

static void test_id_text(void **state)
{
    uint8_t id[IDENT_ID_LEN];
    uint8_t back[IDENT_ID_LEN];
    uint8_t other[IDENT_ID_LEN];
    char hex[33];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof id; i++)
        id[i] = (uint8_t)(i * 17u);
    records_id_hex(id, hex);
    assert_string_equal(hex, "00112233445566778899aabbccddeeff");
    assert_true(records_id_parse(hex, back));
    assert_memory_equal(id, back, sizeof id);
    assert_true(records_id_parse("00112233445566778899AABBCCDDEEFF", back));
    assert_memory_equal(id, back, sizeof id);

    assert_false(records_id_parse("0011", back));
    assert_false(records_id_parse("g0112233445566778899aabbccddeeff", back));
    assert_false(records_id_parse(NULL, back));

    /* Two draws differ (1 in 2^128 that they do not). */
    assert_int_equal(records_new_id(id), CLI_OK);
    assert_int_equal(records_new_id(other), CLI_OK);
    assert_memory_not_equal(id, other, sizeof id);
}

static void test_boot_candidate_gpt(void **state)
{
    part_table t;
    part_entry e;

    (void)state;
    memset(&t, 0, sizeof t);
    memset(&e, 0, sizeof e);
    t.id.kind = PART_KIND_GPT;
    memcpy(e.type_guid, gpt_guid_esp, GPT_GUID_LEN);
    assert_true(records_boot_candidate(&t, &e));
    memcpy(e.type_guid, falcon_guid_boot, GPT_GUID_LEN);
    assert_true(records_boot_candidate(&t, &e));
    e.type_guid[0] ^= 1u;
    assert_false(records_boot_candidate(&t, &e));

    /* MBR: FAT bytes and their hidden forms only. */
    t.id.kind = PART_KIND_MBR;
    e.mbr_type = 0x1C;
    assert_true(records_boot_candidate(&t, &e));
    e.mbr_type = 0x07;
    assert_false(records_boot_candidate(&t, &e));
    assert_false(records_boot_candidate(NULL, &e));
}

/* --write-boot-at's words: what jump's floppy recipe passes, and what a
 * slip of the recipe would pass instead. */
static void test_boot_args(void **state)
{
    const char *why = NULL;
    uint64_t off = 1;
    ident_rec r;

    (void)state;
    assert_true(records_boot_args("0", "98", "0", &off, &r, &why));
    assert_int_equal(off, 0);
    assert_int_equal(r.role, IDENT_ROLE_BOOT);
    assert_int_equal(r.version, IDENT_VERSION);
    assert_int_equal(r.stage3_sector, 98);
    assert_int_equal(r.marker_sector, 0);
    assert_true(records_boot_args("0x100000", "0x20", "8", &off, &r, &why));
    assert_int_equal(off, 1048576u);
    assert_int_equal(r.stage3_sector, 32);
    assert_int_equal(r.marker_sector, 8);

    assert_false(records_boot_args("513", "98", "0", &off, &r, &why));
    assert_non_null(why);
    assert_false(records_boot_args("-512", "98", "0", &off, &r, &why));
    assert_false(records_boot_args("", "98", "0", &off, &r, &why));
    assert_false(records_boot_args("0", "0", "0", &off, &r, &why));
    assert_false(records_boot_args("0", "65536", "0", &off, &r, &why));
    assert_false(records_boot_args("0", "98x", "0", &off, &r, &why));
    assert_false(records_boot_args("0", "98", "+1", &off, &r, &why));
    assert_false(records_boot_args("0", "98", NULL, &off, &r, &why));
    assert_false(records_boot_args("0", "98", "0", NULL, &r, &why));
}

/* A floppy-shaped image: a FAT12 volume at LBA 0 with no table around it,
 * its record written at byte 0 with jump's own sectors, as the recipe in
 * apps/jump/app.mk writes it. */
static void test_write_raw_volume(void **state)
{
    static mks_records r;
    char path[] = "/tmp/falcon-records-fd.XXXXXX";
    const char *why = NULL;
    uint8_t sec[512];
    uint64_t off = 0;
    ident_rec rec;
    ident_rec got;
    int fd = mkstemp(path);

    (void)state;
    assert_true(fd >= 0);
    assert_int_equal(ftruncate(fd, 2880 * 512), 0);
    memset(sec, 0, sizeof sec);
    sec[0] = 0xEB;
    sec[1] = 0x3C;
    sec[2] = 0x90;
    sec[0x0C] = 0x02;
    sec[0x0D] = 1;
    sec[0x0E] = 144;          /* reserved: stage 3 at 98 and its sectors */
    sec[0x10] = 2;
    sec[0x16] = 9;            /* a 16-bit FAT size: not FAT32, no backup */
    sec[0x1D9] = 0xC3;        /* the last byte of the floppy's stage 1 */
    sec[510] = 0x55;
    sec[511] = 0xAA;
    assert_int_equal(pwrite(fd, sec, sizeof sec, 0), 512);
    (void)close(fd);

    assert_true(records_boot_args("0", "98", "0", &off, &rec, &why));
    memset(rec.install_id, 0x5A, sizeof rec.install_id);
    assert_int_equal(records_write_boot(path, off, &rec), CLI_OK);

    fd = open(path, O_RDONLY);
    assert_true(fd >= 0);
    assert_int_equal(pread(fd, sec, sizeof sec, 0), 512);
    (void)close(fd);
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_OK);
    assert_true(ident_same_install(&rec, &got));
    assert_int_equal(got.stage3_sector, 98);
    assert_int_equal(got.marker_sector, 0);
    /* The code before the record and the BPB are left alone. */
    assert_int_equal(sec[0x1D9], 0xC3);
    assert_int_equal(sec[0x0E], 144);
    /* The volume is not a partitioned disk, and mkstick does not say it is. */
    assert_int_equal(records_read(&r, path, 512, 2880 * 512), CLI_OK);
    assert_int_not_equal(r.rc, PART_OK);

    /* Stage 3 past the reserved area is refused, as on a partition. */
    assert_true(records_boot_args("0", "144", "0", &off, &rec, &why));
    memset(rec.install_id, 0x5A, sizeof rec.install_id);
    assert_int_not_equal(records_write_boot(path, off, &rec), CLI_OK);
    (void)unlink(path);
}

/* A 4Kn stick's ESP: FAT32 of 4096-byte sectors, the backup boot sector
 * six of them in. The record goes in both copies, the backup found by the
 * FAT's own sector size. */
static void test_write_4k_fat(void **state)
{
    char path[] = "/tmp/falcon-records-4k.XXXXXX";
    uint8_t sec[512];
    ident_rec rec;
    ident_rec got;
    int fd = mkstemp(path);

    (void)state;
    assert_true(fd >= 0);
    assert_int_equal(ftruncate(fd, 64 * 4096), 0);
    memset(sec, 0, sizeof sec);
    sec[0] = 0xEB;
    sec[1] = 0x58;
    sec[2] = 0x90;
    sec[0x0C] = 0x10;         /* 4096 bytes per sector */
    sec[0x0D] = 1;
    sec[0x0E] = 40;
    sec[0x10] = 2;
    sec[0x30] = 1;
    sec[0x32] = 6;
    sec[510] = 0x55;
    sec[511] = 0xAA;
    assert_int_equal(pwrite(fd, sec, sizeof sec, 0), 512);
    assert_int_equal(pwrite(fd, sec, sizeof sec, 6 * 4096), 512);
    /* A decoy where a 512-byte reading of the backup's number would land. */
    assert_int_equal(pwrite(fd, sec, sizeof sec, 6 * 512), 512);
    (void)close(fd);

    boot_rec(&rec, 0x4B);
    assert_int_equal(records_write_boot(path, 0, &rec), CLI_OK);

    fd = open(path, O_RDONLY);
    assert_true(fd >= 0);
    assert_int_equal(pread(fd, sec, sizeof sec, 6 * 4096), 512);
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_OK);
    assert_true(ident_same_install(&rec, &got));
    assert_int_equal(pread(fd, sec, sizeof sec, 6 * 512), 512);
    assert_int_equal(ident_decode_bin(sec, &got), IDENT_ERR_NONE);
    (void)close(fd);
    (void)unlink(path);
}

static void test_nextcore(void **state)
{
    uint8_t buf[64];
    uint8_t sec[NEXTBOOT_SECTOR_BYTES];
    uint64_t v = 0;
    size_t n;

    (void)state;
    n = nextcore_efivar(NEXTBOOT_PERSONAL, buf, sizeof buf);
    assert_int_equal(n, 4 + 9 + 1);
    assert_int_equal(buf[0], 7);
    assert_int_equal(buf[1], 0);
    assert_memory_equal(buf + 4, ":personal", 9);
    assert_int_equal(buf[13], 0);
    assert_int_equal(nextcore_efivar("iso/arch.iso", buf, 8), 0);
    assert_int_equal(nextcore_efivar("", buf, sizeof buf), 0);
    assert_int_equal(nextcore_efivar("bad\x01value", buf, sizeof buf), 0);
    assert_int_equal(nextcore_efivar(NULL, buf, sizeof buf), 0);

    assert_int_equal(nextcore_parse_epoch("1790000000\n", 11, &v), 0);
    assert_int_equal(v, 1790000000u);
    assert_int_equal(nextcore_parse_epoch("12", 2, &v), 0);
    assert_int_equal(v, 12u);
    assert_int_equal(nextcore_parse_epoch("\n", 1, &v), -1);
    assert_int_equal(nextcore_parse_epoch("12a", 3, &v), -1);
    assert_int_equal(nextcore_parse_epoch("12345678901234567890", 20, &v),
                     -1);
    assert_int_equal(nextcore_parse_epoch(NULL, 0, &v), -1);

    memset(sec, 0, sizeof sec);
    assert_true(nextcore_sector_ours(sec, sizeof sec));
    assert_int_equal(nextboot_sector_write(sec, sizeof sec, 1790000000u,
                                           "arch.iso"), NEXTBOOT_OK);
    assert_true(nextcore_sector_ours(sec, sizeof sec));
    memset(sec, 0, sizeof sec);
    sec[300] = 1;
    assert_false(nextcore_sector_ours(sec, sizeof sec));
    assert_false(nextcore_sector_ours(sec, 511));
}

int run_records_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_write_and_read),
        cmocka_unit_test(test_write_refusals),
        cmocka_unit_test(test_no_table),
        cmocka_unit_test(test_id_text),
        cmocka_unit_test(test_boot_candidate_gpt),
        cmocka_unit_test(test_boot_args),
        cmocka_unit_test(test_write_raw_volume),
        cmocka_unit_test(test_write_4k_fat),
        cmocka_unit_test(test_nextcore),
    };

    return cmocka_run_group_tests_name("mkstick records", tests, NULL, NULL);
}
