/* Tests for falcon-bios/src/station/rows.c: the BIOS station's names,
 * labels and STATION.INI merge. What a countdown boots is decided here. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <station/rows.h>

static part_disk_id mbr_id(uint32_t sig)
{
    part_disk_id id;

    memset(&id, 0, sizeof id);
    id.kind = PART_KIND_MBR;
    id.mbr_sig = sig;
    return id;
}

/* Two disks: 0 is mbr:cdcccdcc, 1 is mbr:1a2b3c4d. */
static int disk_of(const part_disk_id *id, void *ctx)
{
    part_disk_id a = mbr_id(0xcdcccdccu);
    part_disk_id b = mbr_id(0x1a2b3c4du);

    (void)ctx;
    if (part_disk_id_eq(id, &a))
        return 0;
    if (part_disk_id_eq(id, &b))
        return 1;
    return -1;
}

static char g_notes[512];

static void note(const char *section, const char *why, void *ctx)
{
    (void)ctx;
    strncat(g_notes, section, sizeof g_notes - strlen(g_notes) - 1);
    strncat(g_notes, ": ", sizeof g_notes - strlen(g_notes) - 1);
    strncat(g_notes, why, sizeof g_notes - strlen(g_notes) - 1);
    strncat(g_notes, "\n", sizeof g_notes - strlen(g_notes) - 1);
}

static void ascii(const uint16_t *u, char *out, size_t cap)
{
    size_t i;

    for (i = 0; u[i] != 0 && i + 1 < cap; i++)
        out[i] = (char)u[i];
    out[i] = '\0';
}

/* What the station finds on the rig's two disks, before any INI. */
static void found(st_rows *rs)
{
    part_disk_id a = mbr_id(0xcdcccdccu);
    part_disk_id b = mbr_id(0x1a2b3c4du);
    static const uint16_t file[] = u"arch-linux.6.efi";
    char name[STATIONINI_NAME_MAX];
    st_row *r;

    rows_init(rs);
    rows_name_uki(name, file);
    r = rows_add(rs, ROW_UKI, name);
    memcpy(r->uki, "arch-linux.6.efi", 17);
    rows_label_ascii(r->label, "Personal OS: Arch Linux");

    rows_name_disk(name, &a, 1);
    r = rows_add(rs, ROW_PART, name);
    r->disk = 0;
    r->part = 1;
    rows_label_disk(r->label, "Windows (NTLDR)", &a, 1);

    rows_name_disk(name, &b, 0);
    r = rows_add(rs, ROW_DISK, name);
    r->disk = 1;
    rows_label_disk(r->label, "Boot code", &b, 0);

    r = rows_add(rs, ROW_MEMTEST, "memtest");
    rows_label_ascii(r->label, "Memory test (memtest86+)");
}

static void parse(stationini *c, const char *text)
{
    memset(c, 0, sizeof *c);
    assert_int_equal(stationini_parse(text, strlen(text), c), 0);
}

static void test_names_and_labels(void **state)
{
    st_rows rs;
    char text[ROW_LABEL_MAX];
    char name[STATIONINI_NAME_MAX];
    part_disk_id g;

    (void)state;
    found(&rs);
    assert_string_equal(rs.row[0].name, "uki-arch-linux_6");
    assert_string_equal(rs.row[1].name, "mbr-cdcccdcc-p1");
    assert_string_equal(rs.row[2].name, "mbr-1a2b3c4d");
    ascii(rs.row[1].label, text, sizeof text);
    assert_string_equal(text, "Windows (NTLDR) (disk mbr:cdcccdcc, partition 1)");
    ascii(rs.row[2].label, text, sizeof text);
    assert_string_equal(text, "Boot code (disk mbr:1a2b3c4d)");

    /* GPT: the DiskGUID's first group, as its text starts. */
    memset(&g, 0, sizeof g);
    g.kind = PART_KIND_GPT;
    assert_int_equal(part_disk_id_parse("gpt:0f1e2d3c-1111-2222-3333-444455556666",
                                        40, &g), 0);
    rows_name_disk(name, &g, 12);
    assert_string_equal(name, "gpt-0f1e2d3c-p12");

    /* A second disk with the same name is still a row. */
    assert_non_null(rows_add(&rs, ROW_DISK, "mbr-1a2b3c4d"));
    assert_string_equal(rs.row[4].name, "mbr-1a2b3c4d-2");
    assert_int_equal(rows_find(&rs, "mbr-1a2b3c4d-2"), 4);
    assert_int_equal(rows_find(&rs, "MBR-1A2B3C4D"), -1);
}

static void test_no_ini(void **state)
{
    st_rows rs;

    (void)state;
    found(&rs);
    rows_arrange(&rs, NULL);
    assert_int_equal(rs.n_shown, 4);
    assert_int_equal(rs.def, -1);
    assert_int_equal(rs.timeout, 0);
}

static void test_change_add_order_default(void **state)
{
    st_rows rs;
    stationini c;
    char text[ROW_LABEL_MAX];

    (void)state;
    g_notes[0] = '\0';
    found(&rs);
    parse(&c,
          "[general]\n"
          "default = win2k\n"
          "timeout = 5\n"
          "order = memtest, win2k\n"
          "\n"
          "[win2k]\n"
          "disk = mbr:cdcccdcc\n"
          "part = 1\n"
          "label = Windows 2000\n"
          "\n"
          "[second]\n"
          "disk = mbr:1a2b3c4d\n"
          "part = 1\n"
          "swap = no\n"
          "\n"
          "[b-mbr]\n"
          "disk = mbr:1a2b3c4d\n"
          "swap = no\n"
          "\n"
          "[gone]\n"
          "disk = mbr:deadbeef\n"
          "part = 2\n"
          "\n"
          "[logical]\n"
          "disk = mbr:cdcccdcc\n"
          "part = 5\n"
          "\n"
          "[uefi]\n"
          "efi = \\EFI\\Microsoft\\Boot\\bootmgfw.efi\n"
          "\n"
          "[arch]\n"
          "uki = /uki/ARCH-LINUX.6.EFI\n"
          "label = Arch\n");
    rows_apply_ini(&rs, &c, 1, disk_of, note, NULL);

    /* A section on a found entry changes it and names it. */
    assert_int_equal(rows_find(&rs, "win2k"), 1);
    ascii(rs.row[1].label, text, sizeof text);
    assert_string_equal(text, "Windows 2000");
    assert_int_equal(rows_find(&rs, "arch"), 0);
    ascii(rs.row[0].label, text, sizeof text);
    assert_string_equal(text, "Arch");
    assert_int_equal(rs.row[2].swap, 0);

    /* One that matches nothing adds a row: a partition on the second disk
     * with its swap off, and a logical partition, kept for chain.c to
     * refuse with its sentence. */
    assert_int_equal(rows_find(&rs, "second"), 4);
    assert_int_equal(rs.row[4].kind, ROW_PART);
    assert_int_equal(rs.row[4].disk, 1);
    assert_int_equal(rs.row[4].part, 1);
    assert_int_equal(rs.row[4].swap, 0);
    assert_true(rs.row[4].added);
    ascii(rs.row[4].label, text, sizeof text);
    assert_string_equal(text, "Partition (disk mbr:1a2b3c4d, partition 1)");
    assert_int_equal(rs.row[rows_find(&rs, "logical")].part, 5);

    /* What BIOS cannot use is noted, not listed. */
    assert_int_equal(rows_find(&rs, "gone"), -1);
    assert_int_equal(rows_find(&rs, "uefi"), -1);
    assert_non_null(strstr(g_notes, "gone: its disk is not attached"));
    assert_non_null(strstr(g_notes, "uefi: efi entries"));

    rows_arrange(&rs, &c);
    /* order first, then the rest as found. */
    assert_int_equal(rs.shown[0], 3);
    assert_int_equal(rs.shown[1], 1);
    assert_int_equal(rs.shown[2], 0);
    assert_int_equal(rs.def, 1);
    assert_int_equal(rs.timeout, 5);
}

static void test_hide(void **state)
{
    st_rows rs;
    stationini c;
    unsigned i;

    (void)state;
    g_notes[0] = '\0';
    found(&rs);
    parse(&c, "[memtest]\nhide = yes\n[nothing]\nhide = yes\n");
    rows_apply_ini(&rs, &c, 0, disk_of, note, NULL);
    rows_arrange(&rs, &c);
    assert_int_equal(rs.n_shown, 3);
    for (i = 0; i < rs.n_shown; i++)
        assert_int_not_equal(rs.row[rs.shown[i]].kind, ROW_MEMTEST);
    /* Hidden is not gone: a next-boot value still finds it. */
    assert_int_equal(rows_find(&rs, "memtest"), 3);
    assert_non_null(strstr(g_notes, "nothing: it names no entry"));

    /* A default names a hidden row: shown after all, and it counts. */
    parse(&c, "[general]\ndefault = memtest\ntimeout = 3\n[memtest]\nhide = yes\n");
    found(&rs);
    rows_apply_ini(&rs, &c, 0, disk_of, note, NULL);
    rows_arrange(&rs, &c);
    assert_int_equal(rs.n_shown, 4);
    assert_int_equal(rs.row[rs.shown[rs.def]].kind, ROW_MEMTEST);
    assert_int_equal(rs.timeout, 3);
}

static void test_default_missing_and_uki_without_shelf(void **state)
{
    st_rows rs;
    stationini c;

    (void)state;
    g_notes[0] = '\0';
    found(&rs);
    /* An autodetected name that is not on this machine: no default. */
    parse(&c, "[general]\ndefault = mbr-00000001-p3\ntimeout = 5\n");
    rows_apply_ini(&rs, &c, 1, disk_of, note, NULL);
    rows_arrange(&rs, &c);
    assert_int_equal(rs.def, -1);
    assert_int_equal(rs.timeout, 0);

    /* The autodetected name itself as the default. */
    parse(&c, "[general]\ndefault = mbr-1a2b3c4d\ntimeout = 2\n");
    rows_arrange(&rs, &c);
    assert_int_equal(rs.row[rs.shown[rs.def]].kind, ROW_DISK);

    /* A uki on no shelf: noted. */
    parse(&c, "[other]\nuki = other.efi\n");
    rows_apply_ini(&rs, &c, 0, disk_of, note, NULL);
    assert_int_equal(rows_find(&rs, "other"), -1);
    assert_non_null(strstr(g_notes, "other: there is no shelf"));
    /* On a shelf the loader could not list (an encrypted one): added. */
    rows_apply_ini(&rs, &c, 1, disk_of, note, NULL);
    assert_int_equal(rs.row[rows_find(&rs, "other")].kind, ROW_UKI);
    assert_string_equal(rs.row[rows_find(&rs, "other")].uki, "other.efi");
}

int run_rows_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_names_and_labels),
        cmocka_unit_test(test_no_ini),
        cmocka_unit_test(test_change_add_order_default),
        cmocka_unit_test(test_hide),
        cmocka_unit_test(test_default_missing_and_uki_without_shelf),
    };

    return cmocka_run_group_tests_name("rows", tests, NULL, NULL);
}
