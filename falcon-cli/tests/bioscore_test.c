/* Tests for falcon-cli/src/mkstick/bioscore.c: where falcon-bios' stages go
 * on a GPT disk.
 *
 * The interesting cases are all the ones that must FAIL. A stage written one
 * sector low lands on the GPT header; one written long runs into the first
 * partition; a stage 1 that grew past 440 bytes loses its tail to the
 * protective MBR it has to share LBA 0 with. None of those three produces a
 * diagnostic on the machine it breaks, so each of them is tested here.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <mkstick/bioscore.h>

/* The real stick's numbers: a 128-entry array, so 34 usable, and partitions
 * aligned at 2048. */
#define REAL_FIRST_USABLE 34u
#define REAL_FIRST_PART   2048u

static void fill_stage1(uint8_t *s)
{
    memset(s, 0, BIOS_SECTOR_BYTES);
    memset(s, 0x90, 286);              /* about what stage 1 measures today */
    s[BIOS_MBR_SIG_OFF] = 0x55;
    s[BIOS_MBR_SIG_OFF + 1u] = 0xAA;
}

/* An LBA 0 with a protective MBR in it: one 0xEE record at 446 and nothing
 * else that matters. */
static void fill_disk_mbr(uint8_t *m)
{
    memset(m, 0, BIOS_SECTOR_BYTES);
    memset(m, 0xE9, 64);               /* somebody else's boot code */
    m[446] = 0x00;
    m[450] = 0xEE;
    m[BIOS_MBR_SIG_OFF] = 0x55;
    m[BIOS_MBR_SIG_OFF + 1u] = 0xAA;
}

static void test_plan(void **state)
{
    bios_plan p;

    (void)state;
    assert_int_equal(bioscore_plan(34, 64, 1024, &p), BIOSCORE_OK);
    assert_int_equal(p.stage2_lba, 34);
    assert_int_equal(p.stage3_lba, 98);
    assert_int_equal(p.end_lba, 1122);

    /* The whole plan has to fit between the entry array and LBA 2048, and
     * with these constants it does, twice over. */
    assert_true(p.end_lba <= REAL_FIRST_PART);

    /* LBA 1 is the GPT header, 2..33 the entry array. A loader built to read
     * stage 2 from any of them works on a raw image and destroys a stick. */
    assert_int_equal(bioscore_plan(1, 64, 1024, &p), BIOSCORE_ERR_GPT);
    assert_int_equal(bioscore_plan(33, 64, 1024, &p), BIOSCORE_ERR_GPT);

    assert_int_equal(bioscore_plan(34, 0, 1024, &p), BIOSCORE_ERR_ARG);
    assert_int_equal(bioscore_plan(34, 64, 0, &p), BIOSCORE_ERR_ARG);
    assert_int_equal(bioscore_plan(34, 64, 1024, NULL), BIOSCORE_ERR_ARG);
}

static void test_check_images(void **state)
{
    uint8_t s1[BIOS_SECTOR_BYTES];
    bios_plan p;

    (void)state;
    fill_stage1(s1);
    assert_int_equal(bioscore_plan(34, 64, 1024, &p), BIOSCORE_OK);

    assert_int_equal(bioscore_check_images(s1, sizeof s1, 5416, 211000, &p),
                     BIOSCORE_OK);
    /* Exactly the reserved size is a fit. */
    assert_int_equal(bioscore_check_images(s1, sizeof s1, 64u * 512u,
                                           1024u * 512u, &p), BIOSCORE_OK);
    assert_int_equal(bioscore_check_images(s1, sizeof s1, 64u * 512u + 1u,
                                           211000, &p), BIOSCORE_ERR_STAGE2);
    assert_int_equal(bioscore_check_images(s1, sizeof s1, 5416,
                                           1024u * 512u + 1u, &p),
                     BIOSCORE_ERR_STAGE3);
    assert_int_equal(bioscore_check_images(s1, sizeof s1, 0, 211000, &p),
                     BIOSCORE_ERR_STAGE2);

    /* Not one sector, or no signature. */
    assert_int_equal(bioscore_check_images(s1, 511, 5416, 211000, &p),
                     BIOSCORE_ERR_STAGE1);
    s1[BIOS_MBR_SIG_OFF] = 0;
    assert_int_equal(bioscore_check_images(s1, sizeof s1, 5416, 211000, &p),
                     BIOSCORE_ERR_STAGE1);

    /* Past 440 bytes: the splice would drop it, so it is refused instead. */
    fill_stage1(s1);
    s1[BIOS_MBR_CODE_LEN] = 0x01;
    assert_int_equal(bioscore_check_images(s1, sizeof s1, 5416, 211000, &p),
                     BIOSCORE_ERR_SPLICE);
    fill_stage1(s1);
    s1[BIOS_MBR_SIG_OFF - 1u] = 0x01;
    assert_int_equal(bioscore_check_images(s1, sizeof s1, 5416, 211000, &p),
                     BIOSCORE_ERR_SPLICE);
}

static void test_check_gap(void **state)
{
    bios_plan p;

    (void)state;
    assert_int_equal(bioscore_plan(34, 64, 1024, &p), BIOSCORE_OK);
    assert_int_equal(bioscore_check_gap(&p, REAL_FIRST_USABLE,
                                        REAL_FIRST_PART), BIOSCORE_OK);
    /* No partitions: nothing below to protect. */
    assert_int_equal(bioscore_check_gap(&p, REAL_FIRST_USABLE, 0),
                     BIOSCORE_OK);
    /* Touching is not overlapping: end_lba is one past the last sector. */
    assert_int_equal(bioscore_check_gap(&p, REAL_FIRST_USABLE, p.end_lba),
                     BIOSCORE_OK);
    assert_int_equal(bioscore_check_gap(&p, REAL_FIRST_USABLE, p.end_lba - 1u),
                     BIOSCORE_ERR_GAP);

    /* A disk with a bigger entry array declares a later first usable, and
     * the plan built from the loader's constants no longer fits it. */
    assert_int_equal(bioscore_check_gap(&p, 40, REAL_FIRST_PART),
                     BIOSCORE_ERR_GPT);
    /* A header claiming a first usable below the array it must follow. */
    assert_int_equal(bioscore_check_gap(&p, 2, REAL_FIRST_PART),
                     BIOSCORE_ERR_GPT);
    assert_int_equal(bioscore_check_gap(NULL, 34, 2048), BIOSCORE_ERR_ARG);

    /* A stick whose first partition starts at 128 instead of 2048: the
     * stages do not fit and the install has to say so rather than write. */
    assert_int_equal(bioscore_check_gap(&p, REAL_FIRST_USABLE, 128),
                     BIOSCORE_ERR_GAP);
}

static void test_splice(void **state)
{
    uint8_t s1[BIOS_SECTOR_BYTES];
    uint8_t disk[BIOS_SECTOR_BYTES];
    uint8_t out[BIOS_SECTOR_BYTES];

    (void)state;
    fill_stage1(s1);
    fill_disk_mbr(disk);

    assert_int_equal(bioscore_splice_mbr(disk, s1, out), BIOSCORE_OK);
    /* The code area is stage 1's ... */
    assert_memory_equal(out, s1, BIOS_MBR_CODE_LEN);
    /* ... and everything from the disk signature onwards is the disk's,
     * which is what keeps the protective partition record and the 0xAA55. */
    assert_memory_equal(out + BIOS_MBR_CODE_LEN, disk + BIOS_MBR_CODE_LEN,
                        BIOS_SECTOR_BYTES - BIOS_MBR_CODE_LEN);
    assert_int_equal(out[450], 0xEE);
    assert_int_equal(out[BIOS_MBR_SIG_OFF], 0x55);
    assert_int_equal(out[BIOS_MBR_SIG_OFF + 1u], 0xAA);

    /* In place, which is what the command does. */
    fill_disk_mbr(disk);
    assert_int_equal(bioscore_splice_mbr(disk, s1, disk), BIOSCORE_OK);
    assert_memory_equal(disk, s1, BIOS_MBR_CODE_LEN);
    assert_int_equal(disk[450], 0xEE);

    assert_true(bioscore_is_installed(disk, s1));
    disk[0] ^= 0xFFu;
    assert_false(bioscore_is_installed(disk, s1));
    assert_false(bioscore_is_installed(NULL, s1));

    /* A sector with no signature is not a boot sector, and splicing into it
     * would produce one that claims to be. */
    fill_disk_mbr(disk);
    disk[BIOS_MBR_SIG_OFF + 1u] = 0;
    assert_int_equal(bioscore_splice_mbr(disk, s1, out), BIOSCORE_ERR_MBR);
    assert_int_equal(bioscore_splice_mbr(NULL, s1, out), BIOSCORE_ERR_ARG);
}

static void test_backup_header(void **state)
{
    uint8_t hdr[BIOS_BACKUP_HDR];
    uint32_t sector = 0;
    uint64_t lba = 0, sectors = 0;

    (void)state;
    assert_int_equal(bioscore_backup_header(hdr, 512, 34, 1088), BIOSCORE_OK);
    assert_int_equal(bioscore_backup_parse(hdr, sizeof hdr, &sector, &lba,
                                           &sectors), BIOSCORE_OK);
    assert_int_equal(sector, 512);
    assert_int_equal(lba, 34);
    assert_int_equal(sectors, 1088);
    assert_int_equal(bioscore_backup_size(512, 1088),
                     BIOS_BACKUP_HDR + 512u + 512u * 1088u);

    /* Anything else handed to a restore: a truncated file, a file that is
     * not ours, a version we do not write. */
    assert_int_equal(bioscore_backup_parse(hdr, 8, &sector, &lba, &sectors),
                     BIOSCORE_ERR_BACKUP);
    hdr[0] = 'X';
    assert_int_equal(bioscore_backup_parse(hdr, sizeof hdr, &sector, &lba,
                                           &sectors), BIOSCORE_ERR_BACKUP);
    assert_int_equal(bioscore_backup_header(hdr, 512, 34, 1088), BIOSCORE_OK);
    hdr[16] = 9;
    assert_int_equal(bioscore_backup_parse(hdr, sizeof hdr, &sector, &lba,
                                           &sectors), BIOSCORE_ERR_BACKUP);

    assert_int_equal(bioscore_backup_header(hdr, 0, 34, 1088),
                     BIOSCORE_ERR_ARG);
    assert_int_equal(bioscore_backup_header(hdr, 512, 34, 0),
                     BIOSCORE_ERR_ARG);
}

static void test_strerror(void **state)
{
    (void)state;
    assert_string_equal(bioscore_strerror(BIOSCORE_OK), "ok");
    assert_non_null(bioscore_strerror(BIOSCORE_ERR_GAP));
    assert_string_equal(bioscore_strerror((bioscore_status)-99), "unknown");
}

int run_bioscore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_plan),
        cmocka_unit_test(test_check_images),
        cmocka_unit_test(test_check_gap),
        cmocka_unit_test(test_splice),
        cmocka_unit_test(test_backup_header),
        cmocka_unit_test(test_strerror),
    };

    return cmocka_run_group_tests_name("mkstick bioscore", tests, NULL, NULL);
}
