/* Tests for falcon-core/src/media/vbr.c: what a boot sector is. The BIOS
 * station offers a partition as a row by what its boot code names, and the
 * installer names that row in STATION.INI by the same rule; a loader missed
 * here is a system the menu never offers. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <media/vbr.h>

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

static void test_bpb_kinds(void **state)
{
    uint8_t s[512];

    (void)state;
    fat32(s, 63, 0x80);
    assert_int_equal(vbr_bpb_kind(s), VBR_BPB_FAT32);
    fat16(s, 63, 0x80, 0x29);
    assert_int_equal(vbr_bpb_kind(s), VBR_BPB_FAT);
    ntfs(s, 63, 0x80);
    assert_int_equal(vbr_bpb_kind(s), VBR_BPB_NTFS);
    /* An MBR's code in a partition's first sector: no BPB. */
    memset(s, 0x33, sizeof s);
    sign(s);
    assert_int_equal(vbr_bpb_kind(s), VBR_BPB_NONE);
    memset(s, 0, sizeof s);
    assert_int_equal(vbr_bpb_kind(s), VBR_BPB_NONE);
}

static void with_text(uint8_t *s, size_t at, const char *text)
{
    memcpy(s + at, text, strlen(text));
}

static void test_detect(void **state)
{
    uint8_t s[512];

    (void)state;
    fat32(s, 63, 0x80);
    /* mkfs.fat's data boot sector is not a loader. */
    with_text(s, 0x80, "This is not a bootable disk.");
    assert_int_equal(vbr_detect(s), VBR_LOADER_NONE);

    ntfs(s, 63, 0x80);
    with_text(s, 0x180, "A disk read error occurred\r\nBOOTMGR is missing");
    assert_int_equal(vbr_detect(s), VBR_LOADER_BOOTMGR);
    ntfs(s, 63, 0x80);
    with_text(s, 0x180, "A disk read error occurred\r\nNTLDR is missing");
    assert_int_equal(vbr_detect(s), VBR_LOADER_NTLDR);

    /* Windows 98 names IO.SYS as well: WINBOOT wins. */
    fat32(s, 63, 0x80);
    with_text(s, 0x1D0, "IO      SYSMSDOS   SYS");
    assert_int_equal(vbr_detect(s), VBR_LOADER_MSDOS);
    with_text(s, 0x1E8, "WINBOOT SYS");
    assert_int_equal(vbr_detect(s), VBR_LOADER_WIN9X);

    fat16(s, 63, 0x80, 0x29);
    with_text(s, 0x1F0, "KERNEL  SYS");
    assert_int_equal(vbr_detect(s), VBR_LOADER_FREEDOS);
    fat16(s, 63, 0x80, 0x29);
    with_text(s, 0x1E0, "IBMBIO  COM");
    assert_int_equal(vbr_detect(s), VBR_LOADER_MSDOS);

    memset(s, 0x90, sizeof s);
    with_text(s, 0x180, "GRUB \0Geom");
    sign(s);
    assert_int_equal(vbr_detect(s), VBR_LOADER_GRUB);
    fat32(s, 63, 0x80);
    memcpy(s + 3, "SYSLINUX", 8);
    assert_int_equal(vbr_detect(s), VBR_LOADER_SYSLINUX);
    memcpy(s + 3, "EXTLINUX", 8);
    assert_int_equal(vbr_detect(s), VBR_LOADER_SYSLINUX);

    /* A signature with no 0x55AA is no boot sector. */
    s[511] = 0;
    assert_int_equal(vbr_detect(s), VBR_LOADER_NONE);

    assert_string_equal(vbr_loader_name(VBR_LOADER_NTLDR), "Windows (NTLDR)");
    assert_string_equal(vbr_loader_name(VBR_LOADER_WIN9X), "Windows 9x");
    assert_null(vbr_loader_name(VBR_LOADER_NONE));
    assert_int_equal(vbr_fat32_code_sectors[0], 2);
    assert_int_equal(vbr_fat32_code_sectors[1], 12);
    assert_int_equal(vbr_fat32_code_sectors[2], 0);
}

static void test_mbr_bootable(void **state)
{
    uint8_t mbr[512];

    (void)state;
    memset(mbr, 0, sizeof mbr);
    entry(mbr, 0, 0x80, 0x07, 63, 1000);
    sign(mbr);
    /* No code: a table written by a partitioner that left LBA 0 zero. */
    assert_false(vbr_mbr_bootable(mbr));
    mbr[0] = 0xFA;
    assert_true(vbr_mbr_bootable(mbr));
    /* No active entry: a data disk. */
    mbr[0x1BE] = 0;
    assert_false(vbr_mbr_bootable(mbr));
    /* A GPT's protective entry marked active, as some BIOSes need. */
    entry(mbr, 0, 0x80, 0xEE, 1, 1000);
    assert_true(vbr_mbr_bootable(mbr));
    mbr[511] = 0;
    assert_false(vbr_mbr_bootable(mbr));
}

/* A FAT32 volume's reserved sectors, as vbr_identify reads them. */
typedef struct {
    uint8_t sec[16][512];
    int     fail_from;   /* sectors from here fail to read; 16: none */
    int     reads;
} fake_part;

static int fake_read(void *ctx, uint32_t sector, uint8_t *buf)
{
    fake_part *f = (fake_part *)ctx;

    f->reads++;
    if (sector >= 16u || (int)sector >= f->fail_from)
        return -1;
    memcpy(buf, f->sec[sector], 512);
    return 0;
}

static void test_identify(void **state)
{
    static fake_part f;
    uint8_t scratch[512];

    (void)state;
    memset(&f, 0, sizeof f);
    f.fail_from = 16;

    /* Windows 98's FAT32 sector 0 names the loader itself. */
    fat32(f.sec[0], 63, 0x80);
    with_text(f.sec[0], 0x1D0, "IO      SYSMSDOS   SYS");
    with_text(f.sec[0], 0x1E8, "WINBOOT SYS");
    assert_int_equal(vbr_identify(f.sec[0], fake_read, &f, scratch),
                     VBR_LOADER_WIN9X);
    assert_int_equal(f.reads, 0);

    /* A sector 0 that names nothing: the name in sector 2 (98's second
     * code sector), which carries no 0x55AA of its own. */
    fat32(f.sec[0], 63, 0x80);
    with_text(f.sec[2], 0x100, "WINBOOT SYS");
    assert_int_equal(vbr_identify(f.sec[0], fake_read, &f, scratch),
                     VBR_LOADER_WIN9X);

    /* NT's FAT32 boot code keeps NTLDR in sector 12. */
    memset(f.sec[2], 0, 512);
    with_text(f.sec[12], 0x40, "NTLDR");
    f.reads = 0;
    assert_int_equal(vbr_identify(f.sec[0], fake_read, &f, scratch),
                     VBR_LOADER_NTLDR);
    assert_int_equal(f.reads, 2);

    /* A read that fails ends the search. */
    f.fail_from = 12;
    assert_int_equal(vbr_identify(f.sec[0], fake_read, &f, scratch),
                     VBR_LOADER_NONE);
    /* No callback: sector 0 alone. */
    assert_int_equal(vbr_identify(f.sec[0], (vbr_read_fn)0, &f, scratch),
                     VBR_LOADER_NONE);

    /* Not FAT32: sector 0 is the whole answer, and nothing is read. */
    fat16(f.sec[0], 63, 0x80, 0x29);
    f.fail_from = 16;
    f.reads = 0;
    assert_int_equal(vbr_identify(f.sec[0], fake_read, &f, scratch),
                     VBR_LOADER_NONE);
    assert_int_equal(f.reads, 0);

    /* Unsigned sector 0: no boot sector, whatever follows. */
    fat32(f.sec[0], 63, 0x80);
    f.sec[0][511] = 0;
    assert_int_equal(vbr_identify(f.sec[0], fake_read, &f, scratch),
                     VBR_LOADER_NONE);
}

int run_vbr_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_bpb_kinds),
        cmocka_unit_test(test_detect),
        cmocka_unit_test(test_mbr_bootable),
        cmocka_unit_test(test_identify),
    };

    return cmocka_run_group_tests_name("vbr", tests, NULL, NULL);
}
