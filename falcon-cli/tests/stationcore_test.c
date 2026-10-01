/* Tests for falcon-cli/src/mkstick/stationcore.c: the decisions of `station
 * install` (docs/DESIGN.md Section 12.9).
 *
 * The disks here are the ones the installer meets: the A21m's, whose
 * Windows 2000 partition starts at 63 and ran to the end until it was
 * shrunk; one with its four primary slots taken; one with free space only
 * inside an extended partition, where a primary cannot go. The refusals
 * matter as much as the placements: a station partition put over a byte
 * of somebody's filesystem is the one failure this installer must not
 * have.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include <mkstick/stationcore.h>

#define A21M_SECTORS 19640880u   /* DJSA-210: 10,056,130,560 bytes */

static part_entry g_parts[8];

static void table_init(part_table *t, part_kind kind)
{
    memset(t, 0, sizeof *t);
    memset(g_parts, 0, sizeof g_parts);
    t->parts = g_parts;
    t->cap = 8;
    t->id.kind = kind;
}

static void add_part(part_table *t, uint64_t start, uint64_t end,
                     uint8_t type, uint32_t number)
{
    part_entry *p = &g_parts[t->count++];

    p->start_lba = start;
    p->end_lba = end;
    p->mbr_type = type;
    p->number = number;
    p->primary = number <= 4u;
    if (t->first_start == 0u || start < t->first_start)
        t->first_start = start;
}

static void put_entry(uint8_t *lba0, unsigned slot, uint8_t status,
                      uint8_t type, const uint8_t chs_end[3])
{
    uint8_t *e = lba0 + 446 + (slot - 1u) * 16u;

    e[0] = status;
    e[4] = type;
    if (chs_end != NULL)
        memcpy(e + 5, chs_end, 3);
}

/* ---- free space ---- */

static void test_free_a21m(void **state)
{
    part_table t;
    stn_region r;
    uint64_t largest = 0;
    uint64_t want = 64u * 2048u;
    uint64_t hi = A21M_SECTORS - 1u;

    (void)state;
    /* As shipped: NTFS from 63 to the last sector. Nothing is free. */
    table_init(&t, PART_KIND_MBR);
    add_part(&t, 63, hi, 0x07, 1);
    assert_int_equal(stn_first_allowed(&t, 2048), 63);
    assert_int_equal(stn_find_free(&t, 63, hi, want, STN_ALIGN_MIB, &r,
                                   &largest), STN_ERR_NOSPACE);
    assert_int_equal(largest, 0);

    /* Shrunk by 65 MiB: the station goes at the MiB boundary after it. */
    table_init(&t, PART_KIND_MBR);
    add_part(&t, 63, hi - 65u * 2048u, 0x07, 1);
    assert_int_equal(stn_find_free(&t, 63, hi, want, STN_ALIGN_MIB, &r,
                                   &largest), STN_OK);
    assert_int_equal(r.start % 2048u, 0);
    assert_true(r.start > hi - 65u * 2048u);
    assert_true(r.start + r.sectors - 1u <= hi);
    assert_int_equal(r.sectors, want);

    /* Shrunk by exactly 64 MiB: the MiB alignment costs the fit, a 4 KiB
     * one does not. */
    table_init(&t, PART_KIND_MBR);
    add_part(&t, 63, hi - want, 0x07, 1);
    assert_int_equal(stn_find_free(&t, 63, hi, want, STN_ALIGN_MIB, &r,
                                   &largest), STN_ERR_NOSPACE);
    assert_true(largest < want);
    assert_true(largest > 0u);
    assert_int_equal(stn_find_free(&t, 63, hi, want, STN_ALIGN_4K, &r,
                                   &largest), STN_OK);
    assert_int_equal(r.start % 8u, 0);
    assert_true(r.start >= hi - want + 1u);
    assert_true(r.start + want - 1u <= hi);
}

static void test_free_gaps(void **state)
{
    part_table t;
    stn_region r;
    uint64_t largest = 0;

    (void)state;
    /* Two gaps that fit: the higher one wins. */
    table_init(&t, PART_KIND_MBR);
    add_part(&t, 2048, 99999, 0x07, 1);
    add_part(&t, 400000, 499999, 0x83, 2);
    assert_int_equal(stn_find_free(&t, 2048, 999999, 100000, 2048, &r,
                                   &largest), STN_OK);
    assert_int_equal(r.start, 501760);
    /* Only the hole between them fits. */
    assert_int_equal(stn_find_free(&t, 2048, 599999, 200000, 2048, &r,
                                   &largest), STN_OK);
    assert_int_equal(r.start, 100352);
    /* Nothing fits; largest is the hole. */
    assert_int_equal(stn_find_free(&t, 2048, 599999, 400000, 2048, &r,
                                   &largest), STN_ERR_NOSPACE);
    assert_int_equal(largest, 399999u - 100352u + 1u);

    /* Partitions listed out of order are sorted. */
    table_init(&t, PART_KIND_MBR);
    add_part(&t, 400000, 499999, 0x83, 2);
    add_part(&t, 2048, 99999, 0x07, 1);
    assert_int_equal(stn_find_free(&t, 2048, 599999, 200000, 2048, &r,
                                   &largest), STN_OK);
    assert_int_equal(r.start, 100352);

    /* Free space inside an extended partition is not free for a primary. */
    table_init(&t, PART_KIND_MBR);
    add_part(&t, 2048, 99999, 0x07, 1);
    add_part(&t, 100352, 999999, 0x05, 2);
    add_part(&t, 102400, 199999, 0x83, 5);
    assert_int_equal(stn_find_free(&t, 2048, 999999, 100000, 2048, &r,
                                   &largest), STN_ERR_NOSPACE);

    /* An empty disk. */
    table_init(&t, PART_KIND_MBR);
    assert_int_equal(stn_first_allowed(&t, 2048), 2048);
    assert_int_equal(stn_find_free(&t, 2048, 999999, 1000, 2048, &r,
                                   &largest), STN_OK);
    assert_int_equal(r.start, 2048);

    /* Arguments. */
    assert_int_equal(stn_find_free(NULL, 0, 1, 1, 1, &r, &largest),
                     STN_ERR_ARG);
    assert_int_equal(stn_find_free(&t, 5, 1, 1, 1, &r, &largest),
                     STN_ERR_ARG);
    assert_int_equal(stn_find_free(&t, 0, 1, 0, 1, &r, &largest),
                     STN_ERR_ARG);
    assert_int_equal(stn_find_free(&t, 0, 1, 1, 0, &r, &largest),
                     STN_ERR_ARG);
    assert_int_equal(stn_first_allowed(NULL, 34), 34);
}

/* ---- the MBR ---- */

static void test_mbr_slots(void **state)
{
    uint8_t lba0[512];
    static const uint8_t max_chs[3] = { 0xFE, 0xFF, 0xFF };
    static const uint8_t small_chs[3] = { 15, 0x3F | 0xC0, 0xFF };
    unsigned h = 0;
    unsigned s = 0;

    (void)state;
    memset(lba0, 0, sizeof lba0);
    assert_int_equal(stn_mbr_free_slot(lba0), 1);
    put_entry(lba0, 1, 0x80, 0x07, max_chs);
    assert_int_equal(stn_mbr_free_slot(lba0), 2);
    put_entry(lba0, 2, 0, 0x83, NULL);
    put_entry(lba0, 3, 0, 0x82, NULL);
    assert_int_equal(stn_mbr_free_slot(lba0), 4);
    put_entry(lba0, 4, 0, 0x05, NULL);
    assert_int_equal(stn_mbr_free_slot(lba0), 0);
    assert_int_equal(stn_mbr_free_slot(NULL), 0);

    /* The A21m's entry ends at the 1023/254/63 marker: 255 heads, 63. */
    memset(lba0, 0, sizeof lba0);
    put_entry(lba0, 1, 0x80, 0x07, max_chs);
    stn_mbr_geometry(lba0, &h, &s);
    assert_int_equal(h, 255);
    assert_int_equal(s, 63);
    /* An old disk's own geometry. */
    memset(lba0, 0, sizeof lba0);
    put_entry(lba0, 1, 0x80, 0x06, small_chs);
    stn_mbr_geometry(lba0, &h, &s);
    assert_int_equal(h, 16);
    assert_int_equal(s, 63);
    /* No entry: the default. */
    memset(lba0, 0, sizeof lba0);
    stn_mbr_geometry(lba0, &h, &s);
    assert_int_equal(h, 255);
    assert_int_equal(s, 63);
    stn_mbr_geometry(NULL, &h, &s);
    assert_int_equal(h, 255);
}

static void test_mbr_put(void **state)
{
    uint8_t lba0[512];
    const uint8_t *e;

    (void)state;
    memset(lba0, 0, sizeof lba0);
    /* Past cylinder 1023: the marker, and the LBA fields carry it. */
    assert_int_equal(stn_mbr_put(lba0, 2, 0x1C, 19509248, 131072, 255, 63),
                     STN_OK);
    e = lba0 + 446 + 16;
    assert_int_equal(e[0], 0);
    assert_int_equal(e[4], 0x1C);
    assert_int_equal(e[1], 254);
    assert_int_equal(e[2], 0xFF);
    assert_int_equal(e[3], 0xFF);
    assert_int_equal(e[5], 254);
    assert_int_equal(e[6], 0xFF);
    assert_int_equal(e[7], 0xFF);
    assert_int_equal(e[8] | (e[9] << 8) | (e[10] << 16) | (e[11] << 24),
                     19509248);
    assert_int_equal(e[12] | (e[13] << 8) | (e[14] << 16), 131072);

    /* Inside cylinder 1023: exact CHS. LBA 63 is 0/1/1 at 255x63; the
     * end, LBA 16127, is cylinder 1, head 0, sector 63 ... */
    memset(lba0, 0, sizeof lba0);
    assert_int_equal(stn_mbr_put(lba0, 1, 0x0C, 63, 16065, 255, 63), STN_OK);
    e = lba0 + 446;
    assert_int_equal(e[1], 1);
    assert_int_equal(e[2], 1);
    assert_int_equal(e[3], 0);
    assert_int_equal(e[5], 0);
    assert_int_equal(e[6], 63);
    assert_int_equal(e[7], 1);
    /* ... and a cylinder above 255 carries its top bits in the sector
     * byte. */
    memset(lba0, 0, sizeof lba0);
    assert_int_equal(stn_mbr_put(lba0, 1, 0x0C, 300u * 16065u, 1, 255, 63),
                     STN_OK);
    e = lba0 + 446;
    assert_int_equal(e[2], 0x41);
    assert_int_equal(e[3], 300 & 0xFF);

    /* Refusals. */
    assert_int_equal(stn_mbr_put(lba0, 1, 0x1C, 1000, 1, 255, 63),
                     STN_ERR_NOSLOT);
    assert_int_equal(stn_mbr_put(lba0, 2, 0x1C, 0x100000000ull, 1, 255, 63),
                     STN_ERR_RANGE);
    assert_int_equal(stn_mbr_put(lba0, 2, 0x1C, 0xFFFFFFF0u, 0x20, 255, 63),
                     STN_ERR_RANGE);
    assert_int_equal(stn_mbr_put(lba0, 0, 0x1C, 1, 1, 255, 63), STN_ERR_ARG);
    assert_int_equal(stn_mbr_put(lba0, 5, 0x1C, 1, 1, 255, 63), STN_ERR_ARG);
    assert_int_equal(stn_mbr_put(lba0, 2, 0, 1, 1, 255, 63), STN_ERR_ARG);
    assert_int_equal(stn_mbr_put(lba0, 2, 0x1C, 1, 0, 255, 63), STN_ERR_ARG);
    assert_int_equal(stn_mbr_put(lba0, 2, 0x1C, 1, 1, 0, 63), STN_ERR_ARG);
    assert_int_equal(stn_mbr_put(lba0, 2, 0x1C, 1, 1, 255, 64), STN_ERR_ARG);
    assert_int_equal(stn_mbr_put(NULL, 2, 0x1C, 1, 1, 255, 63), STN_ERR_ARG);

    /* The signature. */
    stn_mbr_set_sig(lba0, 0xcccdcccdu);
    assert_int_equal(lba0[440], 0xcd);
    assert_int_equal(lba0[441], 0xcc);
    assert_int_equal(stn_mbr_sig(lba0), 0xcccdcccdu);
}

static void test_chainable(void **state)
{
    uint8_t lba0[512];

    (void)state;
    memset(lba0, 0, sizeof lba0);
    lba0[510] = 0x55;
    lba0[511] = 0xAA;
    /* Ends 0x55AA and nothing else: a data disk. */
    assert_false(stn_mbr_chainable(lba0));
    put_entry(lba0, 1, 0x00, 0x07, NULL);
    lba0[0] = 0xFA;
    /* Code, but no active entry. */
    assert_false(stn_mbr_chainable(lba0));
    put_entry(lba0, 1, 0x80, 0x07, NULL);
    assert_true(stn_mbr_chainable(lba0));
    /* No code. */
    lba0[0] = 0;
    assert_false(stn_mbr_chainable(lba0));
    lba0[0] = 0xFA;
    /* The loader's rule (media/vbr.c): an active entry and code are what
     * count, so a hybrid table beside a protective entry is offered too. */
    put_entry(lba0, 2, 0x00, 0xEE, NULL);
    assert_true(stn_mbr_chainable(lba0));
    put_entry(lba0, 2, 0x00, 0x00, NULL);
    lba0[511] = 0;
    assert_false(stn_mbr_chainable(lba0));
    assert_false(stn_mbr_chainable(NULL));
}

/* ---- boot sectors ---- */

/* Which loader a boot sector names is core's vbr_identify, tested there;
 * what the installer adds is which of them it makes the default. */
static void test_windows_or_dos(void **state)
{
    (void)state;
    assert_true(stn_is_windows_or_dos(VBR_LOADER_NTLDR));
    assert_true(stn_is_windows_or_dos(VBR_LOADER_BOOTMGR));
    assert_true(stn_is_windows_or_dos(VBR_LOADER_WIN9X));
    assert_true(stn_is_windows_or_dos(VBR_LOADER_MSDOS));
    assert_true(stn_is_windows_or_dos(VBR_LOADER_FREEDOS));
    assert_false(stn_is_windows_or_dos(VBR_LOADER_GRUB));
    assert_false(stn_is_windows_or_dos(VBR_LOADER_SYSLINUX));
    assert_false(stn_is_windows_or_dos(VBR_LOADER_NONE));
}

/* ---- names ---- */

static void gpt_id(part_disk_id *id)
{
    assert_int_equal(part_disk_id_parse(
                         "gpt:1a2b3c4d-0000-4000-8000-00000000abcd", 40, id),
                     0);
}

static void test_names(void **state)
{
    part_disk_id id;
    char buf[STATIONINI_NAME_MAX];

    (void)state;
    memset(&id, 0, sizeof id);
    id.kind = PART_KIND_MBR;
    id.mbr_sig = 0xcccdcccdu;
    assert_int_equal(stn_name_disk(&id, 1, buf, sizeof buf), 0);
    assert_string_equal(buf, "mbr-cccdcccd-p1");
    assert_int_equal(stn_name_disk(&id, 0, buf, sizeof buf), 0);
    assert_string_equal(buf, "mbr-cccdcccd");
    gpt_id(&id);
    assert_int_equal(stn_name_disk(&id, 12, buf, sizeof buf), 0);
    assert_string_equal(buf, "gpt-1a2b3c4d-p12");
    assert_int_equal(stn_name_disk(&id, 0, buf, sizeof buf), 0);
    assert_string_equal(buf, "gpt-1a2b3c4d");
    assert_int_equal(stn_name_disk(&id, 1, buf, 8), -1);
    assert_int_equal(stn_name_disk(NULL, 1, buf, sizeof buf), -1);

    assert_int_equal(stn_name_uki("arch-linux.efi", buf, sizeof buf), 0);
    assert_string_equal(buf, "uki-arch-linux");
    assert_int_equal(stn_name_uki("ARCH.EFI", buf, sizeof buf), 0);
    assert_string_equal(buf, "uki-ARCH");
    assert_int_equal(stn_name_uki("devlink", buf, sizeof buf), 0);
    assert_string_equal(buf, "uki-devlink");
    assert_int_equal(stn_name_uki("arch 6.1.efi", buf, sizeof buf), -1);
    assert_int_equal(stn_name_uki(".efi", buf, sizeof buf), -1);
    assert_int_equal(stn_name_uki("x.efi", buf, 4), -1);
    assert_int_equal(stn_name_uki(NULL, buf, sizeof buf), -1);
}

static void test_labels(void **state)
{
    stn_found f;

    (void)state;
    memset(&f, 0, sizeof f);
    /* The loaders' words (station-rework.md section 11). */
    f.kind = STN_F_PART;
    f.os = VBR_LOADER_NTLDR;
    f.disk.kind = PART_KIND_MBR;
    f.disk.mbr_sig = 0xcccdcccdu;
    f.part = 1;
    stn_found_label(&f);
    assert_string_equal(f.label,
                        "Windows (NTLDR) (disk mbr:cccdcccd, partition 1)");
    f.os = VBR_LOADER_WIN9X;
    stn_found_label(&f);
    assert_string_equal(f.label,
                        "Windows 9x (disk mbr:cccdcccd, partition 1)");
    f.os = VBR_LOADER_NONE;
    stn_found_label(&f);
    assert_string_equal(f.label,
                        "Partition (disk mbr:cccdcccd, partition 1)");
    f.kind = STN_F_DISK;
    stn_found_label(&f);
    assert_string_equal(f.label, "Boot code (disk mbr:cccdcccd)");
    /* A GPT id is cut to its first eight digits, and the label fits. */
    gpt_id(&f.disk);
    f.kind = STN_F_PART;
    f.os = VBR_LOADER_MSDOS;
    f.part = 9999;
    stn_found_label(&f);
    assert_string_equal(f.label,
                        "MS-DOS (disk gpt:1a2b3c4d, partition 9999)");
    f.kind = STN_F_EFIWIN;
    f.part = 1;
    stn_found_label(&f);
    assert_string_equal(f.label,
                        "Windows Boot Manager (disk gpt:1a2b3c4d, partition 1)");
    f.kind = STN_F_UKI;
    f.label[0] = 'x';
    f.label[1] = '\0';
    stn_found_label(&f);
    assert_string_equal(f.label, "x");
    stn_found_label(NULL);
}

/* ---- the INI ---- */

static stn_found found_part(vbr_loader os, uint32_t sig, uint32_t part)
{
    stn_found f;

    memset(&f, 0, sizeof f);
    f.kind = STN_F_PART;
    f.os = os;
    f.disk.kind = PART_KIND_MBR;
    f.disk.mbr_sig = sig;
    f.part = part;
    (void)stn_name_disk(&f.disk, part, f.name, sizeof f.name);
    stn_found_label(&f);
    return f;
}

static stn_found found_uki(const char *file)
{
    stn_found f;

    memset(&f, 0, sizeof f);
    f.kind = STN_F_UKI;
    (void)stn_name_uki(file, f.name, sizeof f.name);
    (void)snprintf(f.label, sizeof f.label, "%s", file);
    return f;
}

/* Formats, parses back, and hands the parse to the caller. */
static void roundtrip(const stationini *in, stationini *out)
{
    static char buf[8192];
    size_t len = stationini_format(in, buf, sizeof buf);

    assert_true(len > 0u);
    assert_int_equal(stationini_parse(buf, len, out), 0);
}

static void test_ini_windows(void **state)
{
    static stationini ini;
    static stationini back;
    stn_found f[5];
    const stationini_entry *e;

    (void)state;
    f[0] = found_uki("arch.efi");
    f[1] = found_part(VBR_LOADER_GRUB, 0x11111111u, 2);
    f[2] = found_part(VBR_LOADER_NTLDR, 0xcccdcccdu, 1);
    f[3] = found_part(VBR_LOADER_BOOTMGR, 0x22222222u, 1);
    memset(&f[4], 0, sizeof f[4]);
    f[4].kind = STN_F_DISK;
    f[4].disk.kind = PART_KIND_MBR;
    f[4].disk.mbr_sig = 0x33333333u;
    (void)stn_name_disk(&f[4].disk, 0, f[4].name, sizeof f[4].name);
    stn_found_label(&f[4]);

    assert_int_equal(stn_ini_build(f, 5, 5, &ini), STN_OK);
    /* Sections for what the loader chains, none for the UKI. */
    assert_int_equal(ini.n_entries, 4);
    assert_string_equal(ini.entries[0].name, "grub");
    assert_string_equal(ini.entries[1].name, "windows");
    assert_string_equal(ini.entries[2].name, "windows-2");
    assert_string_equal(ini.entries[3].name, "disk");
    assert_int_equal(ini.entries[3].part, 0);
    /* The first Windows is the default, over the UKI and GRUB before it. */
    assert_string_equal(ini.default_name, "windows");
    assert_int_equal(ini.timeout, 5);

    roundtrip(&ini, &back);
    assert_string_equal(back.default_name, "windows");
    assert_int_equal(back.timeout, 5);
    e = stationini_find(&back, "windows");
    assert_non_null(e);
    assert_true(e->has_disk);
    assert_int_equal(e->disk.mbr_sig, 0xcccdcccdu);
    assert_int_equal(e->part, 1);
    assert_string_equal(e->label,
                        "Windows (NTLDR) (disk mbr:cccdcccd, partition 1)");
    assert_non_null(stationini_find(&back, "disk"));
}

static void test_ini_linux(void **state)
{
    static stationini ini;
    static stationini back;
    stn_found f[3];

    (void)state;
    /* No Windows: the first UKI, by the name the loader gives it. */
    f[0] = found_part(VBR_LOADER_GRUB, 0x11111111u, 2);
    f[1] = found_uki("arch.efi");
    f[2] = found_uki("lts.efi");
    assert_int_equal(stn_ini_build(f, 3, 5, &ini), STN_OK);
    assert_string_equal(ini.default_name, "uki-arch");
    roundtrip(&ini, &back);
    assert_string_equal(back.default_name, "uki-arch");

    /* On an encrypted shelf no row carries uki-arch before the prompt:
     * the default is a section naming the file, which is that row. */
    f[1].locked = 1;
    f[2].locked = 1;
    assert_int_equal(stn_ini_build(f, 3, 5, &ini), STN_OK);
    assert_string_equal(ini.default_name, "linux");
    roundtrip(&ini, &back);
    assert_string_equal(back.default_name, "linux");
    assert_non_null(stationini_find(&back, "linux"));
    assert_string_equal(stationini_find(&back, "linux")->uki, "arch.efi");
    assert_int_equal(stationini_find(&back, "linux")->has_disk, 0);
    f[1].locked = 0;
    f[2].locked = 0;

    /* A UKI whose name the INI cannot say is passed over. */
    f[1].name[0] = '\0';
    assert_int_equal(stn_ini_build(f, 3, 5, &ini), STN_OK);
    assert_string_equal(ini.default_name, "uki-lts");

    /* Nothing but GRUB: GRUB. */
    assert_int_equal(stn_ini_build(f, 1, 7, &ini), STN_OK);
    assert_string_equal(ini.default_name, "grub");
    assert_int_equal(ini.timeout, 7);

    /* Nothing: no default and no countdown, and still an INI. */
    assert_int_equal(stn_ini_build(NULL, 0, 5, &ini), STN_OK);
    assert_string_equal(ini.default_name, "");
    assert_int_equal(ini.timeout, 0);
    assert_int_equal(stn_ini_build(NULL, 1, 5, &ini), STN_ERR_ARG);
    assert_int_equal(stn_ini_build(f, 1, 5, NULL), STN_ERR_ARG);
}

static void test_ini_uefi(void **state)
{
    static stationini ini;
    static stationini back;
    stn_found f[3];

    (void)state;
    memset(f, 0, sizeof f);
    f[0] = found_uki("arch.efi");
    f[1].kind = STN_F_FWENTRY;
    f[1].os = VBR_LOADER_BOOTMGR;
    (void)snprintf(f[1].name, sizeof f[1].name, "nxt-0001");
    (void)snprintf(f[1].label, sizeof f[1].label, "Windows Boot Manager");
    assert_int_equal(stn_ini_build(f, 2, 5, &ini), STN_OK);
    assert_int_equal(ini.n_entries, 0);
    assert_string_equal(ini.default_name, "nxt-0001");
    roundtrip(&ini, &back);
    assert_string_equal(back.default_name, "nxt-0001");

    /* Windows on an ESP, from an image: a section, the firmware's way. */
    f[1].kind = STN_F_EFIWIN;
    gpt_id(&f[1].disk);
    stn_found_label(&f[1]);
    assert_int_equal(stn_ini_build(f, 2, 5, &ini), STN_OK);
    assert_int_equal(ini.n_entries, 1);
    assert_string_equal(ini.entries[0].name, "windows");
    assert_int_equal(ini.entries[0].method, STATIONINI_METHOD_FIRMWARE);
    assert_string_equal(ini.default_name, "windows");
    roundtrip(&ini, &back);
    assert_int_equal(stationini_find(&back, "windows")->method,
                     STATIONINI_METHOD_FIRMWARE);
}

static void test_ini_many(void **state)
{
    static stationini ini;
    static stn_found f[STATIONINI_ENTRIES_MAX + 1u];
    unsigned i;

    (void)state;
    for (i = 0; i < STATIONINI_ENTRIES_MAX + 1u; i++)
        f[i] = found_part(VBR_LOADER_MSDOS, 0x10000000u + i, 1);
    assert_int_equal(stn_ini_build(f, STATIONINI_ENTRIES_MAX, 5, &ini),
                     STN_OK);
    assert_string_equal(ini.entries[2].name, "dos-3");
    assert_int_equal(stn_ini_build(f, STATIONINI_ENTRIES_MAX + 1u, 5, &ini),
                     STN_ERR_FULL);
}

/* ---- the backup ---- */

static void test_backup(void **state)
{
    static uint8_t hdr[STN_BK_HDR];
    static stn_backup b;
    static stn_backup back;
    unsigned i;

    (void)state;
    memset(&b, 0, sizeof b);
    b.sector = 512;
    b.flags = STN_FLAG_BIOS | STN_FLAG_MADE_PART;
    b.station_part = 2;
    b.fw_entry = 0xFFFFFFFFu;
    for (i = 0; i < 16u; i++)
        b.install_id[i] = (uint8_t)i;
    assert_int_equal(stn_backup_disk(&b, "/dev/sda"), 0);
    assert_int_equal(stn_backup_disk(&b, "/dev/sdb"), 1);
    assert_int_equal(stn_backup_disk(&b, "/dev/sda"), 0);
    assert_int_equal(stn_backup_run(&b, 0, STN_RUN_LBA0, 0, 1), STN_OK);
    assert_int_equal(stn_backup_run(&b, 0, STN_RUN_GAP, 1, 62), STN_OK);
    assert_int_equal(stn_backup_run(&b, 0, STN_RUN_REGION, 19509248, 16384),
                     STN_OK);
    assert_int_equal(stn_backup_run(&b, 1, STN_RUN_SIG, 0, 1), STN_OK);
    assert_int_equal(stn_backup_run(&b, 2, STN_RUN_SIG, 0, 1), STN_ERR_ARG);
    assert_int_equal(stn_backup_run(&b, 0, STN_RUN_GAP, 1, 0), STN_ERR_ARG);
    assert_int_equal(stn_backup_run(&b, 0, STN_RUN_GAP, 1,
                                    STN_BK_RUN_MAX + 1u), STN_ERR_ARG);

    assert_int_equal(stn_backup_encode(&b, hdr), STN_OK);
    assert_int_equal(stn_backup_decode(hdr, sizeof hdr, &back), STN_OK);
    assert_int_equal(back.sector, 512);
    assert_int_equal(back.flags, b.flags);
    assert_int_equal(back.station_part, 2);
    assert_int_equal(back.fw_entry, 0xFFFFFFFFu);
    assert_memory_equal(back.install_id, b.install_id, 16);
    assert_int_equal(back.n_disks, 2);
    assert_string_equal(back.disks[1], "/dev/sdb");
    assert_int_equal(back.n_runs, 4);
    assert_int_equal(back.runs[2].kind, STN_RUN_REGION);
    assert_int_equal(back.runs[2].lba, 19509248);
    assert_int_equal(back.runs[3].disk, 1);

    /* What remove writes back. */
    assert_true(stn_run_restores(&back.runs[0], 0));
    assert_false(stn_run_whole_lba0(&back.runs[0], 0));
    assert_true(stn_run_whole_lba0(&back.runs[0], 1));
    assert_false(stn_run_whole_lba0(&back.runs[1], 1));
    assert_true(stn_run_restores(&back.runs[1], 0));
    assert_false(stn_run_restores(&back.runs[2], 0));
    assert_true(stn_run_restores(&back.runs[2], 1));
    assert_false(stn_run_restores(&back.runs[3], 0));
    assert_true(stn_run_restores(&back.runs[3], 1));
    back.runs[0].kind = STN_RUN_GPT;
    assert_false(stn_run_restores(&back.runs[0], 0));
    back.runs[0].kind = STN_RUN_GPT_BACKUP;
    assert_true(stn_run_restores(&back.runs[0], 1));
    back.runs[0].kind = STN_RUN_ESP_BOOT;
    assert_true(stn_run_restores(&back.runs[0], 0));
    back.runs[0].kind = 99;
    assert_false(stn_run_restores(&back.runs[0], 1));
    assert_false(stn_run_restores(NULL, 1));

    /* Damage. */
    hdr[0] ^= 1u;
    assert_int_equal(stn_backup_decode(hdr, sizeof hdr, &back),
                     STN_ERR_BACKUP);
    hdr[0] ^= 1u;
    assert_int_equal(stn_backup_decode(hdr, 100, &back), STN_ERR_BACKUP);
    hdr[128 + 2] = 99;                  /* a run of no known kind */
    assert_int_equal(stn_backup_decode(hdr, sizeof hdr, &back),
                     STN_ERR_BACKUP);
    hdr[128 + 2] = STN_RUN_LBA0;
    hdr[128 + 16] = 0;                  /* a run of no sectors */
    assert_int_equal(stn_backup_decode(hdr, sizeof hdr, &back),
                     STN_ERR_BACKUP);
    hdr[128 + 16] = 1;
    hdr[20] = 0;                        /* sector size 0 */
    hdr[21] = 0;
    assert_int_equal(stn_backup_decode(hdr, sizeof hdr, &back),
                     STN_ERR_BACKUP);
    hdr[21] = 2;
    hdr[512] = 0;                       /* a disk with no path */
    assert_int_equal(stn_backup_decode(hdr, sizeof hdr, &back),
                     STN_ERR_BACKUP);
    assert_int_equal(stn_backup_decode(NULL, sizeof hdr, &back), STN_ERR_ARG);

    /* Encode refusals. */
    memset(&back, 0, sizeof back);
    assert_int_equal(stn_backup_encode(&back, hdr), STN_ERR_ARG);
    assert_int_equal(stn_backup_encode(NULL, hdr), STN_ERR_ARG);

    /* Full lists. */
    memset(&b, 0, sizeof b);
    for (i = 0; i < STN_BK_MAX_DISKS; i++) {
        char path[16];

        (void)snprintf(path, sizeof path, "/dev/sd%c", 'a' + (int)i);
        assert_int_equal(stn_backup_disk(&b, path), (int)i);
    }
    assert_int_equal(stn_backup_disk(&b, "/dev/sdz"), -1);
    assert_int_equal(stn_backup_disk(&b, ""), -1);
    assert_int_equal(stn_backup_disk(NULL, "/dev/sda"), -1);
    for (i = 0; i < STN_BK_MAX_RUNS; i++)
        assert_int_equal(stn_backup_run(&b, 0, STN_RUN_GAP, i, 1), STN_OK);
    assert_int_equal(stn_backup_run(&b, 0, STN_RUN_GAP, 99, 1),
                     STN_ERR_FULL);
}

static void test_reach(void **state)
{
    (void)state;
    assert_int_equal(stn_reach_of(10056130560ull), STN_REACH_PAST_CHS);
    assert_int_equal(stn_reach_of(STN_REACH_CHS_BYTES), STN_REACH_OK);
    assert_int_equal(stn_reach_of(STN_REACH_CHS_BYTES + 1u),
                     STN_REACH_PAST_CHS);
    assert_int_equal(stn_reach_of(STN_REACH_LBA28_BYTES), STN_REACH_PAST_CHS);
    assert_int_equal(stn_reach_of(STN_REACH_LBA28_BYTES + 512u),
                     STN_REACH_PAST_LBA28);
    assert_int_equal(stn_reach_of(1024u * 1024u), STN_REACH_OK);
}

/* A 4Kn disk: the sizes are FAT32's floor at 4096-byte clusters, and the
 * alignment is counted in its own sectors. */
static void test_sizes_4kn(void **state)
{
    part_table t;
    stn_region r;
    uint64_t largest = 0;
    uint64_t mib = 1024u * 1024u / 4096u;
    uint64_t want = STN_DEFAULT_MIB_4K * mib;
    uint64_t hi = 262144u - 6u;              /* 1 GiB, GPT's backup after */

    (void)state;
    assert_int_equal(stn_min_mib(512), STN_MIN_MIB);
    assert_int_equal(stn_default_mib(512), STN_DEFAULT_MIB);
    assert_int_equal(stn_min_mib(4096), STN_MIN_MIB_4K);
    assert_int_equal(stn_default_mib(4096), STN_DEFAULT_MIB_4K);
    assert_int_equal(stn_min_mib(2048), 0);
    assert_int_equal(stn_default_mib(1024), 0);
    /* 65525 clusters of one 4096-byte sector, past 2080 reserved sectors
     * and two FATs of 64 sectors: the floor holds them. */
    assert_true((uint64_t)STN_MIN_MIB_4K * mib >= 65525u + 2080u + 128u);
    assert_true(STN_DEFAULT_MIB_4K >= STN_MIN_MIB_4K);
    assert_true(STN_DEFAULT_MIB_4K <= STN_MAX_MIB);

    assert_int_equal(stn_align_for(STN_ALIGN_MIB, 512), 2048);
    assert_int_equal(stn_align_for(STN_ALIGN_4K, 512), 8);
    assert_int_equal(stn_align_for(STN_ALIGN_MIB, 4096), 256);
    assert_int_equal(stn_align_for(STN_ALIGN_4K, 4096), 1);
    assert_int_equal(stn_align_for(STN_ALIGN_MIB, 100), 0);

    /* An ESP of 300 MiB and a shelf of 64 MiB from 1 MiB: the station goes
     * at the top of what is left, on a MiB boundary. */
    table_init(&t, PART_KIND_GPT);
    add_part(&t, mib, mib + 300u * mib - 1u, 0, 1);
    add_part(&t, 301u * mib, 365u * mib - 1u, 0, 2);
    assert_int_equal(stn_find_free(&t, 6, hi, want,
                                   stn_align_for(STN_ALIGN_MIB, 4096), &r,
                                   &largest), STN_OK);
    assert_int_equal(r.start % mib, 0);
    assert_true(r.start >= 365u * mib);
    assert_true(r.start + want - 1u <= hi);
}

static void test_strerror(void **state)
{
    int s;

    (void)state;
    for (s = 0; s >= -7; s--)
        assert_string_not_equal(stn_strerror((stn_status)s), "unknown");
    assert_string_equal(stn_strerror((stn_status)-99), "unknown");
    assert_int_equal(stn_guid_bios_boot[0], 0x48);
}

int run_stationcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_free_a21m),
        cmocka_unit_test(test_free_gaps),
        cmocka_unit_test(test_mbr_slots),
        cmocka_unit_test(test_mbr_put),
        cmocka_unit_test(test_chainable),
        cmocka_unit_test(test_windows_or_dos),
        cmocka_unit_test(test_names),
        cmocka_unit_test(test_labels),
        cmocka_unit_test(test_ini_windows),
        cmocka_unit_test(test_ini_linux),
        cmocka_unit_test(test_ini_uefi),
        cmocka_unit_test(test_ini_many),
        cmocka_unit_test(test_backup),
        cmocka_unit_test(test_reach),
        cmocka_unit_test(test_sizes_4kn),
        cmocka_unit_test(test_strerror),
    };

    return cmocka_run_group_tests_name("stationcore", tests, NULL, NULL);
}
