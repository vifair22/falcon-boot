/* Tests for falcon-cli/src/mkstick/bioscore.c: where falcon-bios' stages go
 * on a disk (docs/DESIGN.md Section 3.3).
 *
 * The interesting cases are the ones that must FAIL. Stage 2 one sector low
 * lands on the GPT's entry array; one too long runs into the first
 * partition; stage 3 past the reserved area lands in the FAT; a record whose
 * marker is FAT's FSInfo sector would have the loader zero it. None of those
 * produces a diagnostic on the machine it breaks, so each is tested here.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <mkstick/bioscore.h>

/* The contract's numbers (falcon-bios/src/start/biosdefs.h), written here
 * because a host test does not see that header; bios.c passes the real
 * ones. */
#define S2_GPT      34u
#define S2_MBR      1u
#define S2_MAX      64u
#define LBA_OFF     0x1B2u
#define SECTORS_OFF 0x1B6u
#define S3_SECTOR   32u
#define MARKER      8u

static void fill_stage1(uint8_t *s)
{
    memset(s, 0, BIOS_SECTOR_BYTES);
    memset(s, 0x90, 0x1B0);            /* code up to just before the fields */
    s[BIOS_MBR_SIG_OFF] = 0x55;
    s[BIOS_MBR_SIG_OFF + 1u] = 0xAA;
}

/* A FAT32 boot sector as mkfs.fat -F 32 -R <reserved> writes it. */
static void fill_fat32(uint8_t *bs, uint16_t reserved)
{
    memset(bs, 0, BIOS_SECTOR_BYTES);
    bs[0] = 0xEB;
    bs[1] = 0x58;
    bs[2] = 0x90;
    bs[0x0B] = 0x00;                   /* 512 bytes per sector */
    bs[0x0C] = 0x02;
    bs[0x0D] = 1;                      /* sectors per cluster */
    bs[0x0E] = (uint8_t)(reserved & 0xFFu);
    bs[0x0F] = (uint8_t)(reserved >> 8);
    bs[0x10] = 2;                      /* FATs */
    bs[0x30] = 1;                      /* FSInfo */
    bs[0x32] = 6;                      /* backup boot sector */
    bs[510] = 0x55;
    bs[511] = 0xAA;
}

/* The stick: GPT, first partition (the ESP) at 2048, 64 MiB of it, formatted
 * with room for stage 3. */
static void stick(bios_disk *d)
{
    memset(d, 0, sizeof *d);
    d->sector_bytes = 512;
    d->kind = PART_KIND_GPT;
    d->first_usable = 34;
    d->first_part_lba = 2048;
    d->boot_start = 2048;
    d->boot_sectors = 131072;
    d->fat.sector = 512;
    d->fat.reserved = 2080;
    d->fat.fsinfo = 1;
    d->fat.backup = 6;
    d->fat.fat32 = 1;
    d->stage3_sector = S3_SECTOR;
    d->marker_sector = MARKER;
}

/* The A21m's shape: MBR, the first partition at 63, so 62 sectors of gap. */
static void a21m(bios_disk *d)
{
    stick(d);
    d->kind = PART_KIND_MBR;
    d->first_usable = 1;
    d->first_part_lba = 63;
    d->boot_start = 4096;
}

static void test_plan_gpt(void **state)
{
    bios_disk d;
    bios_plan p;

    (void)state;
    stick(&d);
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 14100, 201215,
                                   &p), BIOSCORE_OK);
    assert_int_equal(p.stage2_lba, 34);
    assert_int_equal(p.stage2_sectors, 28);     /* the real length */
    assert_int_equal(p.stage3_lba, 2048 + 32);
    assert_int_equal(p.stage3_sectors, 393);
    assert_int_equal(p.marker_lba, 2048 + 8);

    /* Exactly one sector: a stage that ends on a sector boundary. */
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512, 512, &p),
                     BIOSCORE_OK);
    assert_int_equal(p.stage2_sectors, 1);
    assert_int_equal(p.stage3_sectors, 1);

    /* No marker in the record: none planned. */
    d.marker_sector = 0;
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512, 512, &p),
                     BIOSCORE_OK);
    assert_int_equal(p.marker_lba, 0);
}

static void test_plan_mbr(void **state)
{
    bios_disk d;
    bios_plan p;

    (void)state;
    a21m(&d);
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 14100, 201215,
                                   &p), BIOSCORE_OK);
    assert_int_equal(p.stage2_lba, 1);
    assert_int_equal(p.stage2_sectors, 28);

    /* 62 sectors hold stage 2 and nothing more: one byte past that reaches
     * the Windows partition's boot sector. */
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, 64, 62 * 512, 512, &p),
                     BIOSCORE_OK);
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, 64, 62 * 512 + 1, 512,
                                   &p), BIOSCORE_ERR_GAP);
}

/* A 4Kn stick as mkstick create makes one on a 4096-byte-sector disk: the
 * GPT's array ends at LBA 5, the ESP starts at 1 MiB (LBA 256), and every
 * count is of 4096-byte sectors, the record's included. */
static void test_plan_4kn(void **state)
{
    bios_disk d;
    bios_plan p;

    (void)state;
    stick(&d);
    d.sector_bytes = 4096;
    d.first_usable = 6;
    d.first_part_lba = 256;
    d.boot_start = 256;
    d.boot_sectors = 76800;
    d.fat.sector = 4096;
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 14100, 201215,
                                   &p), BIOSCORE_OK);
    assert_int_equal(p.stage2_lba, 34);
    assert_int_equal(p.stage2_sectors, 4);
    assert_int_equal(p.stage3_lba, 256 + 32);
    assert_int_equal(p.stage3_sectors, 50);
    assert_int_equal(p.marker_lba, 256 + 8);

    /* Stage 1 reads whole sectors to STAGE2_LOAD: stage 2 rounded up to
     * 4096 bytes must still fit the reservation. */
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, S2_MAX * 512,
                                   512, &p), BIOSCORE_OK);
    assert_int_equal(p.stage2_sectors, S2_MAX * 512 / 4096);
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX,
                                   S2_MAX * 512 + 1, 512, &p),
                     BIOSCORE_ERR_STAGE2);
}

static void test_plan_refusals(void **state)
{
    bios_disk d;
    bios_plan p;

    (void)state;
    stick(&d);

    /* Below the header's first usable LBA is the entry array. */
    d.first_usable = 40;
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512, 512, &p),
                     BIOSCORE_ERR_GPT);
    stick(&d);
    assert_int_equal(bioscore_plan(&d, 1, S2_MBR, S2_MAX, 512, 512, &p),
                     BIOSCORE_ERR_GPT);

    /* A partition inside the gap. */
    d.first_part_lba = 60;
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 14100, 512, &p),
                     BIOSCORE_ERR_GAP);
    d.first_part_lba = 0;
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512, 512, &p),
                     BIOSCORE_ERR_GAP);

    /* Stage 2 past the reservation stage 1 may read. */
    stick(&d);
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX,
                                   S2_MAX * 512 + 1, 512, &p),
                     BIOSCORE_ERR_STAGE2);

    /* Stage 3 exactly filling the reserved area fits; a byte more does not. */
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512,
                                   (2080 - 32) * 512, &p), BIOSCORE_OK);
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512,
                                   (2080 - 32) * 512 + 1, &p),
                     BIOSCORE_ERR_RESERVED);

    /* mkfs.fat's default 32 reserved sectors: no room at all. */
    d.fat.reserved = 32;
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512, 512, &p),
                     BIOSCORE_ERR_RECORD);

    /* Records naming FAT's own sectors. */
    stick(&d);
    d.marker_sector = 1;               /* FSInfo */
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512, 512, &p),
                     BIOSCORE_ERR_RECORD);
    d.marker_sector = 7;               /* the backup FSInfo */
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512, 512, &p),
                     BIOSCORE_ERR_RECORD);
    stick(&d);
    d.stage3_sector = 5;               /* a run crossing the backup at 6 */
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512, 2048, &p),
                     BIOSCORE_ERR_RECORD);
    stick(&d);
    d.stage3_sector = 8;               /* on the marker */
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512, 512, &p),
                     BIOSCORE_ERR_RECORD);
    stick(&d);
    d.stage3_sector = 0;
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512, 512, &p),
                     BIOSCORE_ERR_RECORD);

    /* Sectors of neither 512 nor 4096 bytes, and a FAT whose sectors are
     * not the disk's. */
    stick(&d);
    d.sector_bytes = 2048;
    d.fat.sector = 2048;
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512, 512, &p),
                     BIOSCORE_ERR_SECTOR);
    stick(&d);
    d.sector_bytes = 4096;
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 512, 512, &p),
                     BIOSCORE_ERR_FAT);

    stick(&d);
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 0, 512, &p),
                     BIOSCORE_ERR_ARG);
    assert_int_equal(bioscore_plan(NULL, S2_GPT, S2_MBR, S2_MAX, 1, 1, &p),
                     BIOSCORE_ERR_ARG);
    d.kind = (part_kind)0;
    assert_int_equal(bioscore_plan(&d, S2_GPT, S2_MBR, S2_MAX, 1, 1, &p),
                     BIOSCORE_ERR_ARG);
}

static void test_fat_bpb(void **state)
{
    uint8_t bs[BIOS_SECTOR_BYTES];
    bios_fat f;

    (void)state;
    fill_fat32(bs, 2080);
    assert_int_equal(bioscore_fat_bpb(bs, &f), BIOSCORE_OK);
    assert_int_equal(f.reserved, 2080);
    assert_int_equal(f.fsinfo, 1);
    assert_int_equal(f.backup, 6);
    assert_true(f.fat32);

    /* FAT16: a 16-bit FAT size, no FSInfo, no backup. */
    bs[0x16] = 0x20;
    assert_int_equal(bioscore_fat_bpb(bs, &f), BIOSCORE_OK);
    assert_false(f.fat32);
    assert_int_equal(f.backup, 0);

    assert_int_equal(f.sector, 512);

    /* A 4Kn disk's FAT: taken, with its sector size (mkstick bios refuses
     * it, records_write_boot does not). */
    fill_fat32(bs, 2080);
    bs[0x0C] = 0x10;                   /* 4096-byte sectors */
    assert_int_equal(bioscore_fat_bpb(bs, &f), BIOSCORE_OK);
    assert_int_equal(f.sector, 4096);
    bs[0x0C] = 0x06;                   /* 1536: not a power of two */
    assert_int_equal(bioscore_fat_bpb(bs, &f), BIOSCORE_ERR_FAT);
    bs[0x0B] = 0x00;
    bs[0x0C] = 0x01;                   /* 256 */
    assert_int_equal(bioscore_fat_bpb(bs, &f), BIOSCORE_ERR_FAT);
    fill_fat32(bs, 2080);
    bs[510] = 0;
    assert_int_equal(bioscore_fat_bpb(bs, &f), BIOSCORE_ERR_FAT);
    fill_fat32(bs, 2080);
    bs[0] = 0;
    assert_int_equal(bioscore_fat_bpb(bs, &f), BIOSCORE_ERR_FAT);
    fill_fat32(bs, 0);
    assert_int_equal(bioscore_fat_bpb(bs, &f), BIOSCORE_ERR_FAT);
    fill_fat32(bs, 2080);
    bs[0x10] = 0;
    assert_int_equal(bioscore_fat_bpb(bs, &f), BIOSCORE_ERR_FAT);
    assert_int_equal(bioscore_fat_bpb(NULL, &f), BIOSCORE_ERR_ARG);
}

static void test_reserved_for(void **state)
{
    (void)state;
    /* Today's stick stage 3 (393 sectors): the 1 MiB floor. */
    assert_int_equal(bioscore_reserved_for(S3_SECTOR, 201215), 32 + 2048);
    /* No stage 3 at all: the floor too. */
    assert_int_equal(bioscore_reserved_for(S3_SECTOR, 0), 32 + 2048);
    /* Twice a stage 3 over half a MiB rounds up to the next MiB. */
    assert_int_equal(bioscore_reserved_for(S3_SECTOR, 1025 * 512),
                     32 + 4096);
    /* Past the BPB's 16-bit field. */
    assert_int_equal(bioscore_reserved_for(S3_SECTOR, 40u * 1024 * 1024), 0);
    assert_int_equal(bioscore_reserved_for(0, 512), 0);
}

static void test_stage1(void **state)
{
    uint8_t s1[BIOS_SECTOR_BYTES];
    uint8_t disk[BIOS_SECTOR_BYTES];
    uint8_t out[BIOS_SECTOR_BYTES];
    uint64_t lba = 0, n = 0;

    (void)state;
    fill_stage1(s1);
    assert_int_equal(bioscore_check_stage1(s1, sizeof s1), BIOSCORE_OK);
    assert_int_equal(bioscore_check_stage1(s1, 511), BIOSCORE_ERR_STAGE1);
    s1[445] = 1;
    assert_int_equal(bioscore_check_stage1(s1, sizeof s1),
                     BIOSCORE_ERR_SPLICE);
    fill_stage1(s1);
    s1[511] = 0;
    assert_int_equal(bioscore_check_stage1(s1, sizeof s1),
                     BIOSCORE_ERR_STAGE1);
    assert_int_equal(bioscore_check_stage1(NULL, 512), BIOSCORE_ERR_ARG);

    /* A disk's LBA 0: somebody's code, a signature and one MBR entry. */
    fill_stage1(s1);
    memset(disk, 0xE9, sizeof disk);
    disk[440] = 0x12;
    disk[446] = 0x80;
    disk[450] = 0x07;
    disk[510] = 0x55;
    disk[511] = 0xAA;
    assert_false(bioscore_stage1_fields(disk, LBA_OFF, SECTORS_OFF, &lba,
                                        &n) && lba == 34);

    assert_int_equal(bioscore_splice_mbr(disk, s1, LBA_OFF, SECTORS_OFF, 34,
                                         28, out), BIOSCORE_OK);
    assert_memory_equal(out, s1, LBA_OFF);
    assert_int_equal(out[440], 0x12);  /* the disk signature stays */
    assert_int_equal(out[446], 0x80);  /* and the table */
    assert_int_equal(out[450], 0x07);
    assert_true(bioscore_stage1_fields(out, LBA_OFF, SECTORS_OFF, &lba, &n));
    assert_int_equal(lba, 34);
    assert_int_equal(n, 28);
    assert_true(bioscore_is_installed(out, s1, LBA_OFF, SECTORS_OFF));
    assert_false(bioscore_is_installed(disk, s1, LBA_OFF, SECTORS_OFF));

    /* In place, as bios.c does it. */
    assert_int_equal(bioscore_splice_mbr(disk, s1, LBA_OFF, SECTORS_OFF, 1, 5,
                                         disk), BIOSCORE_OK);
    assert_true(bioscore_stage1_fields(disk, LBA_OFF, SECTORS_OFF, &lba, &n));
    assert_int_equal(lba, 1);
    assert_int_equal(n, 5);

    /* Refusals: a disk with no signature, fields outside the code or on top
     * of each other, a run that does not fit its fields. */
    disk[511] = 0;
    assert_int_equal(bioscore_splice_mbr(disk, s1, LBA_OFF, SECTORS_OFF, 34,
                                         28, out), BIOSCORE_ERR_MBR);
    disk[511] = 0xAA;
    assert_int_equal(bioscore_splice_mbr(disk, s1, 438, SECTORS_OFF, 34, 28,
                                         out), BIOSCORE_ERR_ARG);
    assert_int_equal(bioscore_splice_mbr(disk, s1, LBA_OFF, LBA_OFF + 2u, 34,
                                         28, out), BIOSCORE_ERR_ARG);
    assert_int_equal(bioscore_splice_mbr(disk, s1, LBA_OFF, SECTORS_OFF, 34,
                                         0x10000, out), BIOSCORE_ERR_ARG);
    assert_int_equal(bioscore_splice_mbr(disk, s1, LBA_OFF, SECTORS_OFF, 0, 28,
                                         out), BIOSCORE_ERR_ARG);
    assert_false(bioscore_stage1_fields(NULL, LBA_OFF, SECTORS_OFF, &lba, &n));
    assert_false(bioscore_is_installed(NULL, s1, LBA_OFF, SECTORS_OFF));
}

static void test_backup_header(void **state)
{
    uint8_t hdr[BIOS_BACKUP_HDR];
    bios_run runs[4] = { { 0, 1 }, { 34, 28 }, { 2080, 393 }, { 2056, 1 } };
    bios_run back[BIOS_BACKUP_MAX_RUNS];
    uint32_t sb = 0;
    size_t n = 0;

    (void)state;
    assert_int_equal(bioscore_backup_header(hdr, 512, runs, 4), BIOSCORE_OK);
    assert_int_equal(bioscore_backup_parse(hdr, sizeof hdr, &sb, back,
                                           BIOS_BACKUP_MAX_RUNS, &n),
                     BIOSCORE_OK);
    assert_int_equal(sb, 512);
    assert_int_equal(n, 4);
    assert_int_equal(back[2].lba, 2080);
    assert_int_equal(back[2].sectors, 393);
    assert_int_equal(bioscore_backup_size(512, runs, 4),
                     BIOS_BACKUP_HDR + 512u * (1 + 28 + 393 + 1));

    /* Not ours, too short, the old version, too many runs for the caller. */
    assert_int_equal(bioscore_backup_parse(hdr, 16, &sb, back, 4, &n),
                     BIOSCORE_ERR_BACKUP);
    assert_int_equal(bioscore_backup_parse(hdr, sizeof hdr, &sb, back, 3, &n),
                     BIOSCORE_ERR_BACKUP);
    hdr[16] = 1;
    assert_int_equal(bioscore_backup_parse(hdr, sizeof hdr, &sb, back, 4, &n),
                     BIOSCORE_ERR_BACKUP);
    hdr[0] = 'X';
    assert_int_equal(bioscore_backup_parse(hdr, sizeof hdr, &sb, back, 4, &n),
                     BIOSCORE_ERR_BACKUP);

    /* A run no stage could ever be is a damaged header. */
    runs[1].sectors = 0x20000;
    assert_int_equal(bioscore_backup_header(hdr, 512, runs, 4), BIOSCORE_OK);
    assert_int_equal(bioscore_backup_parse(hdr, sizeof hdr, &sb, back, 4, &n),
                     BIOSCORE_ERR_BACKUP);

    runs[1].sectors = 0;
    assert_int_equal(bioscore_backup_header(hdr, 512, runs, 4),
                     BIOSCORE_ERR_ARG);
    assert_int_equal(bioscore_backup_header(hdr, 512, runs, 0),
                     BIOSCORE_ERR_ARG);
    assert_int_equal(bioscore_backup_header(hdr, 0, runs, 1),
                     BIOSCORE_ERR_ARG);
    assert_int_equal(bioscore_backup_parse(NULL, 0, &sb, back, 4, &n),
                     BIOSCORE_ERR_ARG);
    assert_int_equal(bioscore_backup_size(512, NULL, 0), BIOS_BACKUP_HDR);
}

static void test_is_reserve(void **state)
{
    part_table t;
    part_entry e;
    const char *name = BIOS_BOOT_NAME;
    size_t i;

    (void)state;
    memset(&t, 0, sizeof t);
    memset(&e, 0, sizeof e);
    t.id.kind = PART_KIND_GPT;
    e.number = BIOS_BOOT_SLOT;
    for (i = 0; name[i] != '\0'; i++)
        e.name[i] = (uint16_t)name[i];
    assert_true(bioscore_is_reserve(&t, &e));

    e.number = 1;
    assert_false(bioscore_is_reserve(&t, &e));
    e.number = BIOS_BOOT_SLOT;
    e.name[0] = 'X';
    assert_false(bioscore_is_reserve(&t, &e));
    e.name[0] = 'F';
    t.id.kind = PART_KIND_MBR;
    assert_false(bioscore_is_reserve(&t, &e));
    assert_false(bioscore_is_reserve(NULL, &e));
}

static void test_strerror(void **state)
{
    int s;

    (void)state;
    for (s = 0; s >= -13; s--)
        assert_non_null(bioscore_strerror((bioscore_status)s));
    assert_string_equal(bioscore_strerror((bioscore_status)-99), "unknown");
}

int run_bioscore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_plan_gpt),
        cmocka_unit_test(test_plan_mbr),
        cmocka_unit_test(test_plan_4kn),
        cmocka_unit_test(test_plan_refusals),
        cmocka_unit_test(test_fat_bpb),
        cmocka_unit_test(test_reserved_for),
        cmocka_unit_test(test_stage1),
        cmocka_unit_test(test_backup_header),
        cmocka_unit_test(test_is_reserve),
        cmocka_unit_test(test_strerror),
    };

    return cmocka_run_group_tests_name("bioscore", tests, NULL, NULL);
}
