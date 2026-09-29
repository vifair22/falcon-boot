/* Tests for falcon-core/src/media/strows.c: the stations' rows, their
 * names, what a STATION.INI section does to them, and the order they are
 * shown in (docs/DESIGN.md Sections 13.3, 13.7, 13.8).
 *
 * The STATION.INI here is the parser's output written by hand, so these
 * tests say what the loader does with a configuration, not how one is
 * parsed (that is falcon-core's stationini tests).
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <media/strows.h>

static const uint8_t k_guid_a[16] = {
    0x4d, 0x3c, 0x2b, 0x1a, 0x6f, 0x5e, 0x80, 0x7f,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
};

static part_disk_id gpt_a(void)
{
    part_disk_id d;

    memset(&d, 0, sizeof d);
    d.kind = PART_KIND_GPT;
    memcpy(d.guid, k_guid_a, sizeof d.guid);
    return d;
}

static part_disk_id mbr(uint32_t sig)
{
    part_disk_id d;

    memset(&d, 0, sizeof d);
    d.kind = PART_KIND_MBR;
    d.mbr_sig = sig;
    return d;
}

static int label_is(const uint16_t *label, const char *want)
{
    size_t i;

    for (i = 0; want[i] != '\0'; i++)
        if (label[i] != (uint16_t)(unsigned char)want[i])
            return 0;
    return label[i] == 0;
}

static void set_label(uint16_t *label, const char *s)
{
    size_t i;

    for (i = 0; s[i] != '\0'; i++)
        label[i] = (uint16_t)(unsigned char)s[i];
    label[i] = 0;
}

/* The list a typical machine gives: a plaintext UKI, Windows' firmware
 * entry on disk A partition 1, a USB entry, Windows found on disk
 * mbr:cdcccdcc's ESP with no entry, memtest, Firmware Setup. */
static void found(strows *r)
{
    strow *w;
    part_disk_id a = gpt_a();

    strows_init(r);
    w = strows_add(r, STROW_UKI);
    strcpy(w->file, "arch.efi");
    strows_name_uki(w->name, w->file);
    set_label(w->label, "Personal OS: Arch Linux");

    w = strows_add(r, STROW_NXT);
    strows_name_nxt(w->name, 0x0003);
    set_label(w->label, "Windows Boot Manager");
    w->has_part = 1;
    w->disk = a;
    w->part = 1;
    strcpy(w->file, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi");

    w = strows_add(r, STROW_NXT);
    strows_name_nxt(w->name, 0x000A);
    set_label(w->label, "UEFI USB");

    w = strows_add(r, STROW_ESP);
    w->has_part = 1;
    w->disk = mbr(0xcdcccdccu);
    w->part = 1;
    strcpy(w->file, STROWS_BOOTMGFW);
    strows_name_part(w->name, &w->disk, w->part);
    strows_label_part(w->label, STROWS_LABEL_MAX, u"Windows Boot Manager",
                      &w->disk, w->part);

    w = strows_add(r, STROW_MEMTEST);
    strcpy(w->name, "memtest");
    set_label(w->label, "Memory test (memtest86+)");

    w = strows_add(r, STROW_FWSETUP);
    set_label(w->label, "Reboot to firmware setup");
}

static stationini_entry *entry(stationini *c, const char *name)
{
    stationini_entry *e = &c->entries[c->n_entries++];

    memset(e, 0, sizeof *e);
    strcpy(e->name, name);
    return e;
}

static void test_names(void **state)
{
    char n[STATIONINI_NAME_MAX];
    part_disk_id a = gpt_a();
    part_disk_id m = mbr(0xcdcccdccu);
    uint16_t label[STROWS_LABEL_MAX];
    char ascii[STROWS_LABEL_MAX];

    (void)state;
    strows_name_part(n, &a, 2);
    assert_string_equal(n, "gpt-1a2b3c4d-p2");
    strows_name_part(n, &m, 0);
    assert_string_equal(n, "mbr-cdcccdcc");
    strows_name_part(n, &m, 12);
    assert_string_equal(n, "mbr-cdcccdcc-p12");
    strows_name_nxt(n, 0x00Af);
    assert_string_equal(n, "nxt-00af");
    strows_name_uki(n, "arch.EFI");
    assert_string_equal(n, "uki-arch");
    strows_name_uki(n, "linux 7.2.efi");
    assert_string_equal(n, "uki-linux-7-2");
    strows_name_uki(n, "a-very-long-name-that-runs-past-the-limit.efi");
    assert_int_equal(strlen(n), STATIONINI_NAME_MAX - 1u);

    strows_label_part(label, STROWS_LABEL_MAX, u"Windows Boot Manager", &a,
                      1);
    assert_true(label_is(label,
                         "Windows Boot Manager (disk gpt:1a2b3c4d, "
                         "partition 1)"));
    strows_label_part(label, STROWS_LABEL_MAX, u"Windows 7", &m, 0);
    assert_true(label_is(label, "Windows 7 (disk mbr:cdcccdcc)"));

    /* The installer's copy says the same, and cuts where it must. */
    strows_label_part_ascii(ascii, sizeof ascii, STROWS_LABEL_BOOTMGR, &a, 1);
    assert_string_equal(ascii, "Windows Boot Manager (disk gpt:1a2b3c4d, "
                               "partition 1)");
    strows_label_part_ascii(ascii, sizeof ascii, STROWS_LABEL_BOOTCODE, &m, 0);
    assert_string_equal(ascii, "Boot code (disk mbr:cdcccdcc)");
    strows_label_part_ascii(ascii, 10, "Windows 9x", &m, 1);
    assert_string_equal(ascii, "Windows 9");
    strows_label_part_ascii(ascii, 0, "x", &m, 1);

    assert_true(strows_path_eq("/efi/microsoft/boot/BOOTMGFW.EFI",
                               STROWS_BOOTMGFW));
    assert_false(strows_path_eq("\\EFI\\x.efi", "\\EFI\\x.ef"));
}

static void test_no_ini(void **state)
{
    static strows r;
    size_t view[STROWS_MAX];
    size_t sel = 99;

    (void)state;
    found(&r);
    assert_int_equal(strows_arrange(&r, NULL, view, STROWS_MAX), 6);
    assert_int_equal(view[0], 0);
    assert_int_equal(view[5], 5);
    assert_false(strows_default(&r, NULL, view, 6, &sel));
    assert_ptr_equal(strows_find(&r, "NXT-0003"), &r.row[1]);
    assert_ptr_equal(strows_find(&r, "mbr-cdcccdcc-p1"), &r.row[3]);
    assert_null(strows_find(&r, "nxt-0004"));
    assert_null(strows_find(&r, ""));
}

static void test_order_default_hide(void **state)
{
    static strows r;
    static stationini c;
    size_t view[STROWS_MAX];
    size_t sel = 99;
    size_t n;

    (void)state;
    found(&r);
    memset(&c, 0, sizeof c);
    strcpy(c.default_name, "win");
    c.timeout = 5;
    strcpy(c.order[c.n_order++], "win");
    strcpy(c.order[c.n_order++], "memtest");
    strcpy(c.order[c.n_order++], "nowhere");
    strcpy(c.order[c.n_order++], "win");     /* twice: shown once */
    {
        stationini_entry *e = entry(&c, "win");

        e->has_disk = 1;
        e->disk = gpt_a();
        e->part = 1;
        strcpy(e->label, "Windows 11");
    }
    entry(&c, "nxt-000a")->hide = STATIONINI_TRI_YES;
    strows_apply(&r, &c, STROWS_UEFI);

    /* The firmware entry on that disk took the section. */
    assert_string_equal(r.row[1].section, "win");
    assert_true(label_is(r.row[1].label, "Windows 11"));
    assert_int_equal(r.row[2].hidden, 1);

    n = strows_arrange(&r, &c, view, STROWS_MAX);
    assert_int_equal(n, 5);
    assert_int_equal(view[0], 1);   /* win */
    assert_int_equal(view[1], 4);   /* memtest */
    assert_int_equal(view[2], 0);   /* arch */
    assert_int_equal(view[3], 3);   /* the ESP row */
    assert_int_equal(view[4], 5);   /* Firmware Setup, last */
    assert_true(strows_default(&r, &c, view, n, &sel));
    assert_int_equal(sel, 0);

    /* A hidden row is still a name next boot can use. */
    assert_ptr_equal(strows_find(&r, "nxt-000a"), &r.row[2]);

    /* A default that is hidden gets no countdown. */
    strcpy(c.default_name, "nxt-000a");
    assert_false(strows_default(&r, &c, view, n, &sel));
}

static void test_fwsetup_stays_last(void **state)
{
    static strows r;
    static stationini c;
    size_t view[STROWS_MAX];

    (void)state;
    found(&r);
    memset(&c, 0, sizeof c);
    strcpy(r.row[5].name, "fwsetup");
    strcpy(c.order[c.n_order++], "fwsetup");
    assert_int_equal(strows_arrange(&r, &c, view, STROWS_MAX), 6);
    assert_int_equal(view[0], 0);
    assert_int_equal(view[5], 5);
}

static void test_method_efi(void **state)
{
    static strows r;
    static stationini c;
    stationini_entry *e;

    (void)state;
    found(&r);
    memset(&c, 0, sizeof c);
    /* Disk A has a firmware entry, but method = efi wants its ESP: a new
     * row, found when booted. */
    e = entry(&c, "win-direct");
    e->has_disk = 1;
    e->disk = gpt_a();
    e->method = STATIONINI_METHOD_EFI;
    /* The found ESP on the MBR disk, named by disk and partition. */
    e = entry(&c, "old");
    e->has_disk = 1;
    e->disk = mbr(0xcdcccdccu);
    e->part = 1;
    e->method = STATIONINI_METHOD_EFI;
    strcpy(e->label, "Windows 10 (old disk)");
    strows_apply(&r, &c, STROWS_UEFI);

    assert_int_equal(r.n, 7);
    assert_int_equal(r.row[6].kind, STROW_ESP);
    assert_string_equal(r.row[6].section, "win-direct");
    assert_string_equal(r.row[6].name, "gpt-1a2b3c4d");
    assert_string_equal(r.row[6].file, STROWS_BOOTMGFW);
    assert_int_equal(r.row[6].part, 0);
    assert_int_equal(r.row[6].ref_none, 1);
    assert_true(label_is(r.row[6].label,
                         "Windows Boot Manager (disk gpt:1a2b3c4d)"));
    assert_string_equal(r.row[3].section, "old");
    assert_true(label_is(r.row[3].label, "Windows 10 (old disk)"));
}

static void test_disk_without_entry(void **state)
{
    static strows r;
    static stationini c;
    stationini_entry *e;

    (void)state;
    found(&r);
    memset(&c, 0, sizeof c);
    /* No firmware entry on the MBR disk, but its ESP row is found. */
    e = entry(&c, "old");
    e->has_disk = 1;
    e->disk = mbr(0xcdcccdccu);
    strows_apply(&r, &c, STROWS_UEFI);
    assert_int_equal(r.n, 6);
    assert_string_equal(r.row[3].section, "old");

    /* A disk nothing was found on: a firmware-entry row with no entry. */
    e = entry(&c, "gone");
    e->has_disk = 1;
    e->disk = mbr(0x11223344u);
    e->part = 2;
    strows_apply(&r, &c, STROWS_UEFI);
    assert_int_equal(r.n, 7);
    assert_int_equal(r.row[6].kind, STROW_NXT);
    assert_int_equal(r.row[6].ref_none, 1);
    assert_string_equal(r.row[6].name, "mbr-11223344-p2");
    assert_true(label_is(r.row[6].label, "gone"));
}

static void test_uki_and_efi(void **state)
{
    static strows r;
    static stationini c;
    stationini_entry *e;

    (void)state;
    found(&r);
    memset(&c, 0, sizeof c);
    e = entry(&c, "arch");
    strcpy(e->uki, "ARCH.efi");
    e->hide = STATIONINI_TRI_NO;
    e = entry(&c, "lts");
    strcpy(e->uki, "arch-lts.efi");
    e = entry(&c, "shell");
    strcpy(e->efi, "/EFI/tools/shellx64.efi");
    strcpy(e->label, "UEFI Shell");
    e = entry(&c, "memtest");
    e->hide = STATIONINI_TRI_YES;
    e = entry(&c, "nothing");   /* names nothing found, adds nothing */
    strows_apply(&r, &c, STROWS_UEFI);

    assert_string_equal(r.row[0].section, "arch");
    assert_int_equal(r.n, 8);
    assert_int_equal(r.row[6].kind, STROW_UKI);
    assert_string_equal(r.row[6].file, "arch-lts.efi");
    assert_string_equal(r.row[6].name, "uki-arch-lts");
    assert_int_equal(r.row[6].ref_none, 1);
    assert_int_equal(r.row[7].kind, STROW_ESP);
    assert_int_equal(r.row[7].own_esp, 1);
    assert_string_equal(r.row[7].file, "\\EFI\\tools\\shellx64.efi");
    assert_true(label_is(r.row[7].label, "UEFI Shell"));
    assert_int_equal(r.row[4].hidden, 1);
    assert_ptr_equal(strows_find(&r, "shell"), &r.row[7]);
    assert_ptr_equal(strows_find(&r, "uki-arch"), &r.row[0]);
}

static void test_full(void **state)
{
    static strows r;
    size_t i;

    (void)state;
    strows_init(&r);
    for (i = 0; i < STROWS_MAX; i++)
        assert_non_null(strows_add(&r, STROW_UKI));
    assert_null(strows_add(&r, STROW_UKI));
}

/* What the BIOS station finds on the rig's two disks (DESIGN Section
 * 13.3): the encrypted shelf's row, Windows on disk A's first
 * partition, disk B's MBR, memtest. */
static void found_bios(strows *r)
{
    strow *w;

    strows_init(r);
    w = strows_add(r, STROW_POS);
    strcpy(w->name, STROWS_NAME_POS);
    set_label(w->label, "Personal OS (encrypted)");

    w = strows_add(r, STROW_VBR);
    w->has_part = 1;
    w->disk = mbr(0xcdcccdccu);
    w->part = 1;
    strows_name_part(w->name, &w->disk, w->part);
    strows_label_part(w->label, STROWS_LABEL_MAX, u"Windows (NTLDR)",
                      &w->disk, w->part);

    w = strows_add(r, STROW_MBR);
    w->has_part = 1;
    w->disk = mbr(0x1a2b3c4du);
    strows_name_part(w->name, &w->disk, 0);
    strows_label_part(w->label, STROWS_LABEL_MAX, u"Boot code", &w->disk, 0);

    w = strows_add(r, STROW_MEMTEST);
    strcpy(w->name, "memtest");
    set_label(w->label, "Memory test (memtest86+)");
}

static void test_bios_found(void **state)
{
    strows r;
    size_t view[STROWS_MAX];

    (void)state;
    found_bios(&r);
    assert_string_equal(r.row[1].name, "mbr-cdcccdcc-p1");
    assert_true(label_is(r.row[1].label,
                         "Windows (NTLDR) (disk mbr:cdcccdcc, partition 1)"));
    assert_string_equal(r.row[2].name, "mbr-1a2b3c4d");
    assert_true(label_is(r.row[2].label, "Boot code (disk mbr:1a2b3c4d)"));
    assert_int_equal(strows_arrange(&r, NULL, view, STROWS_MAX), 4);
    assert_ptr_equal(strows_find(&r, "POS"), &r.row[0]);
    assert_null(strows_find(&r, ""));
}

/* The encrypted shelf's row by its generated name: a section of that name
 * hides it, relabels it, and default and order take it. A `uki` section
 * beside it (the T430's: a default behind the prompt) is a row of its own,
 * and with `[pos]` hidden it is the one prompt the list shows. */
static void test_pos_by_name(void **state)
{
    static strows r;
    static stationini c;
    stationini_entry *e;
    strow *w;
    size_t view[STROWS_MAX];
    size_t sel = 99;
    size_t n;

    (void)state;
    found_bios(&r);
    memset(&c, 0, sizeof c);
    strcpy(c.default_name, "pos");
    c.timeout = 5;
    strcpy(c.order[c.n_order++], "memtest");
    strcpy(c.order[c.n_order++], "pos");
    e = entry(&c, "pos");
    strcpy(e->label, "Gentoo");
    strows_apply(&r, &c, STROWS_BIOS);
    assert_int_equal(r.n, 4);
    assert_ptr_equal(strows_find(&r, "pos"), &r.row[0]);
    assert_string_equal(r.row[0].section, "pos");
    assert_true(label_is(r.row[0].label, "Gentoo"));
    n = strows_arrange(&r, &c, view, STROWS_MAX);
    assert_int_equal(n, 4);
    assert_int_equal(view[0], 3);   /* memtest */
    assert_int_equal(view[1], 0);   /* pos */
    assert_true(strows_default(&r, &c, view, n, &sel));
    assert_int_equal(sel, 1);

    found(&r);
    w = strows_add(&r, STROW_POS);
    strcpy(w->name, STROWS_NAME_POS);
    memset(&c, 0, sizeof c);
    strcpy(c.default_name, "linux");
    c.timeout = 5;
    strcpy(entry(&c, "linux")->uki, "gentoo.efi");
    entry(&c, "pos")->hide = STATIONINI_TRI_YES;
    strows_apply(&r, &c, STROWS_UEFI);
    assert_true(strows_find(&r, "pos")->hidden);
    assert_int_equal(strows_find(&r, "pos")->kind, STROW_POS);
    assert_int_equal(strows_find(&r, "linux")->kind, STROW_UKI);
    assert_true(strows_find(&r, "linux")->ref_none);
    n = strows_arrange(&r, &c, view, STROWS_MAX);
    assert_int_equal(n, 7);
    assert_true(strows_default(&r, &c, view, n, &sel));
    assert_ptr_equal(&r.row[view[sel]], strows_find(&r, "linux"));

    /* A default naming the hidden pos gets no countdown. */
    strcpy(c.default_name, "pos");
    assert_false(strows_default(&r, &c, view, n, &sel));
}

/* A section with disk (+part) on BIOS: the chain row of exactly that disk
 * and partition, its label, swap and name; or a new one, looked for when it
 * is chosen. part 0 is the disk's MBR, not "any partition". */
static void test_bios_disk(void **state)
{
    strows r;
    stationini c;
    stationini_entry *e;
    strow *w;

    (void)state;
    found_bios(&r);
    memset(&c, 0, sizeof c);
    e = entry(&c, "win2k");
    e->has_disk = 1;
    e->disk = mbr(0xcdcccdccu);
    e->part = 1;
    strcpy(e->label, "Windows 2000");
    e = entry(&c, "second");
    e->has_disk = 1;
    e->disk = mbr(0x1a2b3c4du);
    e->part = 1;
    e->swap = STATIONINI_TRI_NO;
    e = entry(&c, "b-mbr");
    e->has_disk = 1;
    e->disk = mbr(0x1a2b3c4du);
    e->swap = STATIONINI_TRI_NO;
    e = entry(&c, "logical");
    e->has_disk = 1;
    e->disk = mbr(0xcdcccdccu);
    e->part = 5;
    strows_apply(&r, &c, STROWS_BIOS);

    w = strows_find(&r, "win2k");
    assert_ptr_equal(w, &r.row[1]);
    assert_true(label_is(w->label, "Windows 2000"));
    assert_false(w->no_swap);

    w = strows_find(&r, "second");
    assert_non_null(w);
    assert_int_equal(w->kind, STROW_VBR);
    assert_true(w->added);
    assert_true(w->ref_none);
    assert_int_equal(w->part, 1);
    assert_true(w->no_swap);
    assert_string_equal(w->name, "mbr-1a2b3c4d-p1");
    assert_true(label_is(w->label, "Partition (disk mbr:1a2b3c4d, partition 1)"));

    /* The disk alone is its MBR row, found. */
    w = strows_find(&r, "b-mbr");
    assert_ptr_equal(w, &r.row[2]);
    assert_true(w->no_swap);

    /* A logical partition is a row, refused when it is chosen. */
    w = strows_find(&r, "logical");
    assert_non_null(w);
    assert_int_equal(w->part, 5);
    assert_int_equal(r.n, 6);
}

/* efi and method = efi are UEFI's: on BIOS such a section is left out, and
 * on UEFI a disk section is never a chain. */
static void test_bios_efi_left_out(void **state)
{
    strows r;
    stationini c;
    stationini_entry *e;

    (void)state;
    found_bios(&r);
    memset(&c, 0, sizeof c);
    e = entry(&c, "uefi");
    strcpy(e->efi, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
    e = entry(&c, "bm");
    e->has_disk = 1;
    e->disk = mbr(0xcdcccdccu);
    e->part = 1;
    e->method = STATIONINI_METHOD_EFI;
    e = entry(&c, "memtest");
    e->hide = STATIONINI_TRI_YES;
    strows_apply(&r, &c, STROWS_BIOS);
    assert_int_equal(r.n, 4);
    assert_null(strows_find(&r, "uefi"));
    assert_null(strows_find(&r, "bm"));
    assert_true(r.row[3].hidden);

    found(&r);
    memset(&c, 0, sizeof c);
    e = entry(&c, "win");
    e->has_disk = 1;
    e->disk = mbr(0xcdcccdccu);
    e->part = 1;
    strows_apply(&r, &c, STROWS_UEFI);
    assert_int_not_equal(strows_find(&r, "win")->kind, STROW_VBR);
}

int run_strows_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_names),
        cmocka_unit_test(test_no_ini),
        cmocka_unit_test(test_order_default_hide),
        cmocka_unit_test(test_fwsetup_stays_last),
        cmocka_unit_test(test_method_efi),
        cmocka_unit_test(test_disk_without_entry),
        cmocka_unit_test(test_uki_and_efi),
        cmocka_unit_test(test_full),
        cmocka_unit_test(test_bios_found),
        cmocka_unit_test(test_pos_by_name),
        cmocka_unit_test(test_bios_disk),
        cmocka_unit_test(test_bios_efi_left_out),
    };

    return cmocka_run_group_tests_name("strows", tests, NULL, NULL);
}
