/* Tests for falcon-bios/src/mbr/vbrcore.c: what the station hands a
 * partition's boot sector (the entry marked active, the BPB made to agree).
 * Each of these, wrong, is a system that starts and then cannot find
 * itself, on a machine with no serial line. What the station calls a boot
 * sector is core's media/vbr.c, tested there. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <mbr/vbrcore.h>

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void sign(uint8_t *s)
{
    s[510] = 0x55;
    s[511] = 0xAA;
}

static void entry(uint8_t *mbr, unsigned slot, uint8_t status, uint8_t type,
                  uint32_t start, uint32_t len)
{
    uint8_t *e = mbr + 0x1BE + 16u * slot;

    e[0] = status;
    e[4] = type;
    put32(e + 8, start);
    put32(e + 12, len);
}

/* A FAT32 BPB as mkfs.fat writes one: 512-byte sectors, 32 reserved, two
 * FATs, FAT16 size and root entries 0, extended signature 0x29. */
static void fat32(uint8_t *s, uint32_t hidden, uint8_t drive)
{
    memset(s, 0, 512);
    s[0] = 0xEB;
    s[1] = 0x58;
    s[2] = 0x90;
    memcpy(s + 3, "MSWIN4.1", 8);
    s[0x0B] = 0x00;
    s[0x0C] = 0x02;
    s[0x0D] = 8;
    s[0x0E] = 32;
    s[0x10] = 2;
    s[0x15] = 0xF8;
    put32(s + 0x1C, hidden);
    put32(s + 0x24, 1000);
    s[0x40] = drive;
    s[0x42] = 0x29;
    sign(s);
}

static void fat16(uint8_t *s, uint32_t hidden, uint8_t drive, uint8_t sig)
{
    memset(s, 0, 512);
    s[0] = 0xEB;
    s[1] = 0x3C;
    s[2] = 0x90;
    s[0x0B] = 0x00;
    s[0x0C] = 0x02;
    s[0x0D] = 4;
    s[0x0E] = 1;
    s[0x10] = 2;
    s[0x11] = 0x00;
    s[0x12] = 0x02;
    s[0x15] = 0xF8;
    s[0x16] = 200;
    put32(s + 0x1C, hidden);
    s[0x24] = drive;
    s[0x26] = sig;
    sign(s);
}

static void ntfs(uint8_t *s, uint32_t hidden, uint8_t drive)
{
    memset(s, 0, 512);
    s[0] = 0xEB;
    s[1] = 0x52;
    s[2] = 0x90;
    memcpy(s + 3, "NTFS    ", 8);
    s[0x0B] = 0x00;
    s[0x0C] = 0x02;
    s[0x0D] = 8;
    put32(s + 0x1C, hidden);
    s[0x24] = drive;
    sign(s);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void test_entry_si(void **state)
{
    (void)state;
    /* The Windows MBR's addresses: the copy at 0x600, entries at 0x7BE. */
    assert_int_equal(VBR_ENTRY_SI(0), 0x7BE);
    assert_int_equal(VBR_ENTRY_SI(1), 0x7CE);
    assert_int_equal(VBR_ENTRY_SI(3), 0x7EE);
}

static void test_mark_active(void **state)
{
    uint8_t mbr[512];

    (void)state;
    memset(mbr, 0, sizeof mbr);
    entry(mbr, 0, 0x80, 0x07, 63, 1000);
    entry(mbr, 1, 0x00, 0x0C, 2048, 1000);
    entry(mbr, 2, 0x00, 0x05, 4096, 1000);
    sign(mbr);

    assert_int_equal(vbr_mark_active(mbr, 1, 2048), 0);
    assert_int_equal(mbr[0x1BE], 0x00);
    assert_int_equal(mbr[0x1CE], 0x80);
    /* The type byte is handed over unchanged: Windows 98's FAT32 boot
     * sector reads LBA or CHS off it. */
    assert_int_equal(mbr[0x1CE + 4], 0x0C);

    /* An extended container, an empty slot, a slot past four and a start
     * that is not the row's are refused, and change nothing. */
    assert_int_equal(vbr_mark_active(mbr, 2, 4096), -1);
    assert_int_equal(vbr_mark_active(mbr, 3, 0), -1);
    assert_int_equal(vbr_mark_active(mbr, 4, 63), -1);
    assert_int_equal(vbr_mark_active(mbr, 0, 64), -1);
    assert_int_equal(mbr[0x1CE], 0x80);
    assert_int_equal(mbr[0x1BE], 0x00);
}

static void test_fix_hidden_and_drive(void **state)
{
    uint8_t s[512];
    vbr_fix f;

    (void)state;
    /* Already right: nothing changes. */
    fat32(s, 2048, 0x80);
    vbr_fix_bpb(s, 2048, 0x80, &f);
    assert_int_equal(f.bpb, VBR_BPB_FAT32);
    assert_false(f.hidden_patched);
    assert_false(f.drive_patched);

    /* A disk imaged from elsewhere: 63 on disk, 2048 in the table; and
     * behind no swap, drive 0x81 where the BPB says 0x80. */
    fat32(s, 63, 0x80);
    vbr_fix_bpb(s, 2048, 0x81, &f);
    assert_true(f.hidden_patched);
    assert_int_equal(f.hidden_was, 63);
    assert_int_equal(get32(s + 0x1C), 2048);
    assert_true(f.drive_patched);
    assert_int_equal(f.drive_was, 0x80);
    assert_int_equal(s[0x40], 0x81);

    /* FAT16 with the extended BPB: drive at 0x24. */
    fat16(s, 0, 0x00, 0x29);
    vbr_fix_bpb(s, 63, 0x80, &f);
    assert_int_equal(get32(s + 0x1C), 63);
    assert_int_equal(s[0x24], 0x80);

    /* FAT16 without it (DOS 3.x's BPB): 0x24 is boot code, left alone. */
    fat16(s, 0, 0x55, 0x00);
    vbr_fix_bpb(s, 63, 0x80, &f);
    assert_true(f.hidden_patched);
    assert_false(f.drive_patched);
    assert_int_equal(s[0x24], 0x55);

    /* NTFS always has the field. */
    ntfs(s, 63, 0x80);
    vbr_fix_bpb(s, 63, 0x81, &f);
    assert_false(f.hidden_patched);
    assert_true(f.drive_patched);
    assert_int_equal(s[0x24], 0x81);

    /* Past 32 bits the field cannot say it; it is left and said. */
    fat32(s, 63, 0x80);
    vbr_fix_bpb(s, 0x100000000ull, 0x80, &f);
    assert_true(f.hidden_too_far);
    assert_false(f.hidden_patched);
    assert_int_equal(get32(s + 0x1C), 63);

    /* No BPB: nothing is written. */
    memset(s, 0x33, sizeof s);
    sign(s);
    vbr_fix_bpb(s, 63, 0x80, &f);
    assert_int_equal(f.bpb, VBR_BPB_NONE);
    assert_int_equal(s[0x1C], 0x33);
    assert_int_equal(s[0x24], 0x33);
}

int run_vbrcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_entry_si),
        cmocka_unit_test(test_mark_active),
        cmocka_unit_test(test_fix_hidden_and_drive),
    };

    return cmocka_run_group_tests_name("vbrcore", tests, NULL, NULL);
}
