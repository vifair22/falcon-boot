/* Tests for falcon-core/src/media/biosboot.c: can falcon-bios boot this
 * medium, and does it need a 64-bit processor.
 *
 * The kernel and ELF halves run on heads built here byte by byte, since what
 * they read is a handful of fields. The medium half runs on
 * falcon-core/tools/mk-test-biosboot.sh's ISOs, xorriso-built, one per shape
 * the rules have to tell apart, and on two of mk-test-uefiboot.sh's.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include <media/biosboot.h>
#include <media/bootfs.h>
#include <media/eltorito.h>
#include <media/family.h>
#include <media/medium.h>

#define FIX_EFIONLY   "build/test-bios-efionly.iso"
#define FIX_DATA      "build/test-bios-data.iso"
#define FIX_ARM64     "build/test-bios-arm64.iso"
#define FIX_EFIKERNEL "build/test-bios-efikernel.iso"
#define FIX_64        "build/test-bios-64.iso"
#define FIX_MULTI     "build/test-bios-multi.iso"
#define FIX_32        "build/test-bios-32.iso"
#define FIX_OLD       "build/test-bios-old.iso"
#define FIX_MEMTEST   "build/test-bios-memtest.iso"
#define FIX_MISSING   "build/test-bios-missing.iso"
#define FIX_HDD       "build/test-bios-hdd.iso"
#define FIX_BSD64     "build/test-bios-bsd64.iso"
#define FIX_BSD32     "build/test-bios-bsd32.iso"
#define FIX_WINEFI    "build/test-bios-win-efi.iso"
/* mk-test-uefiboot.sh's: a BIOS isolinux medium whose kernel's header has
 * no version, and Windows media with a BIOS El Torito image. */
#define FIX_BIOSONLY  "build/test-uefi-biosonly.iso"
#define FIX_WINBIOS   "build/test-uefi-win-bios.iso"

/* Static for the reason medium_test.c gives: a medium must not move once
 * opened, and the work area is tens of kilobytes. */
static medium        g_m;
static biosboot_work g_w;
static bootfs_ctx    g_fam_ctx;
static char          g_scratch[256 * 1024];
static uint8_t       g_head[BIOSBOOT_HEAD];

static int img_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    FILE *f = ctx;

    if (fseek(f, (long)off, SEEK_SET) != 0)
        return -1;
    return fread(buf, 1, (size_t)len, f) == len ? 0 : -1;
}

static int have(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

/* The family as the BIOS loader finds it: the first volume that
 * classifies, in the medium's own order. */
static family_id detect(medium *m)
{
    unsigned i;

    for (i = 0; i < medium_count(m); i++) {
        family_probe probe;
        const family_desc *fam;

        bootfs_probe(&g_fam_ctx, medium_vol_at(m, i), &probe);
        fam = family_detect(&probe);
        if (fam->id != FAMILY_UNKNOWN)
            return fam->id;
    }
    return FAMILY_UNKNOWN;
}

/* Opens path, asks both rules, and hands back the family it used. */
static biosboot_result ask(const char *path, family_id *fam_out)
{
    FILE *f = fopen(path, "rb");
    biosboot_result r;
    family_id fam;
    long end;

    assert_non_null(f);
    assert_int_equal(fseek(f, 0, SEEK_END), 0);
    end = ftell(f);
    assert_true(end > 0);
    assert_int_equal(medium_open(&g_m, img_read, f, (uint64_t)end),
                     MEDIUM_OK);
    fam = detect(&g_m);
    biosboot_medium(&g_m, fam, &g_w, g_scratch, sizeof g_scratch, &r);
    fclose(f);
    if (fam_out != NULL)
        *fam_out = fam;
    return r;
}

/* ---- the kernel and ELF halves ---- */

static void bz_head(uint16_t version, uint16_t xload)
{
    memset(g_head, 0, sizeof g_head);
    g_head[0x1fe] = 0x55;
    g_head[0x1ff] = 0xaa;
    memcpy(g_head + 0x202, "HdrS", 4);
    g_head[0x206] = (uint8_t)(version & 0xffu);
    g_head[0x207] = (uint8_t)(version >> 8);
    g_head[0x236] = (uint8_t)(xload & 0xffu);
    g_head[0x237] = (uint8_t)(xload >> 8);
}

static void test_kernel_widths(void **state)
{
    (void)state;
    bz_head(0x020fu, 0x0001u);
    assert_int_equal(biosboot_kernel_kind(g_head, sizeof g_head, 8192u),
                     BIOSBOOT_KERNEL_64);
    bz_head(0x020fu, 0x0000u);
    assert_int_equal(biosboot_kernel_kind(g_head, sizeof g_head, 8192u),
                     BIOSBOOT_KERNEL_32);
    /* 2.12 exactly is where xloadflags is read. */
    bz_head(0x020cu, 0x0003u);
    assert_int_equal(biosboot_kernel_kind(g_head, sizeof g_head, 8192u),
                     BIOSBOOT_KERNEL_64);
}

/* A bzImage the width of which the header cannot say. */
static void test_kernel_old(void **state)
{
    (void)state;
    bz_head(0x020bu, 0x0001u);
    assert_int_equal(biosboot_kernel_kind(g_head, sizeof g_head, 8192u),
                     BIOSBOOT_KERNEL_OLD);
    bz_head(0x020fu, 0x0001u);
    /* Cut before xloadflags: HdrS was seen, the width was not. */
    assert_int_equal(biosboot_kernel_kind(g_head, 0x220u, 8192u),
                     BIOSBOOT_KERNEL_OLD);
}

static void test_kernel_none(void **state)
{
    (void)state;
    memset(g_head, 0x5a, sizeof g_head);
    assert_int_equal(biosboot_kernel_kind(g_head, sizeof g_head, 8192u),
                     BIOSBOOT_KERNEL_NONE);
    /* Shorter than the setup header's two sectors: refused outright. */
    bz_head(0x020fu, 0x0001u);
    assert_int_equal(biosboot_kernel_kind(g_head, 512u, 512u),
                     BIOSBOOT_KERNEL_NONE);
    /* Too little read to see the magic, of a file that is long enough. */
    assert_int_equal(biosboot_kernel_kind(g_head, 0x100u, 8192u),
                     BIOSBOOT_KERNEL_UNKNOWN);
    assert_int_equal(biosboot_kernel_kind(NULL, 16u, 8192u),
                     BIOSBOOT_KERNEL_UNKNOWN);
}

static void test_elf(void **state)
{
    static const uint8_t amd64[20] = {
        0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 62, 0
    };
    static const uint8_t i386[20] = {
        0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 3, 0
    };
    uint8_t odd[20];

    (void)state;
    assert_int_equal(biosboot_elf_is_64(amd64, sizeof amd64), BIOSBOOT_YES);
    assert_int_equal(biosboot_elf_is_64(i386, sizeof i386), BIOSBOOT_NO);
    /* An x86-64 machine with a 32-bit class is neither. */
    memcpy(odd, amd64, sizeof odd);
    odd[4] = 1;
    assert_int_equal(biosboot_elf_is_64(odd, sizeof odd), BIOSBOOT_UNKNOWN);
    memcpy(odd, amd64, sizeof odd);
    odd[1] = 'X';
    assert_int_equal(biosboot_elf_is_64(odd, sizeof odd), BIOSBOOT_UNKNOWN);
    assert_int_equal(biosboot_elf_is_64(amd64, 19u), BIOSBOOT_UNKNOWN);
    assert_int_equal(biosboot_elf_is_64(NULL, 20u), BIOSBOOT_UNKNOWN);
}

/* The rule falcon-bios's virtual CD serves by, now core's. */
static void test_elt_bios_entry(void **state)
{
    elt_catalog cat;

    (void)state;
    memset(&cat, 0, sizeof cat);
    assert_null(elt_bios_entry(&cat));
    assert_null(elt_bios_entry(NULL));
    /* A default entry that says 0xEF, and a section entry for x86. */
    cat.n = 2;
    cat.entry[0].is_default = 1;
    cat.entry[0].platform = ELT_PLATFORM_UEFI;
    cat.entry[1].platform = ELT_PLATFORM_X86;
    assert_ptr_equal(elt_bios_entry(&cat), &cat.entry[1]);
    /* A default entry for anything but 0xEF comes first. */
    cat.entry[0].platform = ELT_PLATFORM_MAC;
    assert_ptr_equal(elt_bios_entry(&cat), &cat.entry[0]);
    /* 0xEF only. */
    cat.entry[0].platform = ELT_PLATFORM_UEFI;
    cat.entry[1].platform = ELT_PLATFORM_UEFI;
    assert_null(elt_bios_entry(&cat));
}

static void test_arg_errors(void **state)
{
    biosboot_result r;

    (void)state;
    r.boots = BIOSBOOT_NO;
    r.needs64 = BIOSBOOT_YES;
    biosboot_medium(NULL, FAMILY_UNKNOWN, &g_w, g_scratch, sizeof g_scratch,
                    &r);
    assert_int_equal(r.boots, BIOSBOOT_UNKNOWN);
    assert_int_equal(r.needs64, BIOSBOOT_UNKNOWN);
    memset(&g_m, 0, sizeof g_m);
    biosboot_medium(&g_m, FAMILY_UNKNOWN, &g_w, g_scratch, sizeof g_scratch,
                    &r);
    assert_int_equal(r.boots, BIOSBOOT_UNKNOWN);
    biosboot_medium(&g_m, FAMILY_UNKNOWN, &g_w, g_scratch, sizeof g_scratch,
                    NULL);
    assert_int_equal(biosboot_has_image(NULL), BIOSBOOT_UNKNOWN);
}

/* ---- the medium ---- */

static void test_medium_efi_only(void **state)
{
    biosboot_result r;

    (void)state;
    r = ask(FIX_EFIONLY, NULL);
    assert_int_equal(r.boots, BIOSBOOT_NO);
    assert_int_equal(r.arm64, 0);
    /* Nothing here runs at all, so no width is claimed. */
    assert_int_equal(r.needs64, BIOSBOOT_UNKNOWN);
}

/* Its only UEFI loader is ARM64's: {ARM64 ONLY}, since an x86-64 PC's UEFI
 * could not start it either. */
static void test_medium_arm64_only(void **state)
{
    biosboot_result r;

    (void)state;
    r = ask(FIX_ARM64, NULL);
    assert_int_equal(r.boots, BIOSBOOT_NO);
    assert_int_equal(r.arm64, 1);
    assert_int_equal(r.needs64, BIOSBOOT_UNKNOWN);
}

/* A data disc neither loader can start: {EFI ONLY} would say it boots on
 * UEFI, which nothing on it shows, so it is left unmarked. */
static void test_medium_data_disc(void **state)
{
    biosboot_result r;

    (void)state;
    r = ask(FIX_DATA, NULL);
    assert_int_equal(r.boots, BIOSBOOT_UNKNOWN);
}

/* No BIOS image, but a kernel the direct route starts. */
static void test_medium_efi_image_bios_kernel(void **state)
{
    biosboot_result r;

    (void)state;
    r = ask(FIX_EFIKERNEL, NULL);
    assert_int_equal(r.boots, BIOSBOOT_YES);
    assert_int_equal(r.needs64, BIOSBOOT_YES);
}

static void test_medium_64(void **state)
{
    biosboot_result r;

    (void)state;
    r = ask(FIX_64, NULL);
    assert_int_equal(r.boots, BIOSBOOT_YES);
    assert_int_equal(r.needs64, BIOSBOOT_YES);
}

/* Debian netinst's shape: one 32-bit entry is enough. */
static void test_medium_multi_arch(void **state)
{
    biosboot_result r;

    (void)state;
    r = ask(FIX_MULTI, NULL);
    assert_int_equal(r.boots, BIOSBOOT_YES);
    assert_int_equal(r.needs64, BIOSBOOT_NO);
}

static void test_medium_32(void **state)
{
    biosboot_result r;

    (void)state;
    r = ask(FIX_32, NULL);
    assert_int_equal(r.boots, BIOSBOOT_YES);
    assert_int_equal(r.needs64, BIOSBOOT_NO);
}

static void test_medium_old_header(void **state)
{
    biosboot_result r;

    (void)state;
    r = ask(FIX_OLD, NULL);
    assert_int_equal(r.boots, BIOSBOOT_YES);
    assert_int_equal(r.needs64, BIOSBOOT_UNKNOWN);
}

/* The memory tester beside the OS is not asked. */
static void test_medium_memtest(void **state)
{
    biosboot_result r;

    (void)state;
    r = ask(FIX_MEMTEST, NULL);
    assert_int_equal(r.boots, BIOSBOOT_YES);
    assert_int_equal(r.needs64, BIOSBOOT_YES);
}

static void test_medium_missing_kernel(void **state)
{
    biosboot_result r;

    (void)state;
    r = ask(FIX_MISSING, NULL);
    assert_int_equal(r.boots, BIOSBOOT_YES);
    assert_int_equal(r.needs64, BIOSBOOT_UNKNOWN);
}

/* A BIOS image the virtual CD does not serve is still a BIOS image. */
static void test_medium_hdd_emulation(void **state)
{
    biosboot_result r;

    (void)state;
    r = ask(FIX_HDD, NULL);
    assert_int_equal(r.boots, BIOSBOOT_UNKNOWN);
}

static void test_medium_bsd(void **state)
{
    biosboot_result r;
    family_id fam;

    (void)state;
    r = ask(FIX_BSD64, &fam);
    assert_int_equal(fam, FAMILY_BSD);
    assert_int_equal(r.boots, BIOSBOOT_YES);
    assert_int_equal(r.needs64, BIOSBOOT_YES);
    r = ask(FIX_BSD32, &fam);
    assert_int_equal(fam, FAMILY_BSD);
    assert_int_equal(r.boots, BIOSBOOT_YES);
    assert_int_equal(r.needs64, BIOSBOOT_NO);
}

static void test_medium_windows(void **state)
{
    biosboot_result r;
    family_id fam;

    (void)state;
    r = ask(FIX_WINEFI, &fam);
    assert_int_equal(fam, FAMILY_WINDOWS);
    assert_int_equal(r.boots, BIOSBOOT_NO);
    assert_int_equal(r.needs64, BIOSBOOT_UNKNOWN);
    r = ask(FIX_WINBIOS, &fam);
    assert_int_equal(fam, FAMILY_WINDOWS);
    assert_int_equal(r.boots, BIOSBOOT_YES);
    /* The width is boot.wim's, which this does not read. */
    assert_int_equal(r.needs64, BIOSBOOT_UNKNOWN);
}

/* A BIOS medium whose kernel has no version: bootable, width unproved. */
static void test_medium_bios_versionless(void **state)
{
    biosboot_result r;

    (void)state;
    if (!have(FIX_BIOSONLY))
        skip();
    r = ask(FIX_BIOSONLY, NULL);
    assert_int_equal(r.boots, BIOSBOOT_YES);
    assert_int_equal(r.needs64, BIOSBOOT_UNKNOWN);
}

int run_biosboot_tests(void)
{
    const struct CMUnitTest heads[] = {
        cmocka_unit_test(test_kernel_widths),
        cmocka_unit_test(test_kernel_old),
        cmocka_unit_test(test_kernel_none),
        cmocka_unit_test(test_elf),
        cmocka_unit_test(test_elt_bios_entry),
        cmocka_unit_test(test_arg_errors),
    };
    const struct CMUnitTest media[] = {
        cmocka_unit_test(test_medium_efi_only),
        cmocka_unit_test(test_medium_arm64_only),
        cmocka_unit_test(test_medium_data_disc),
        cmocka_unit_test(test_medium_efi_image_bios_kernel),
        cmocka_unit_test(test_medium_64),
        cmocka_unit_test(test_medium_multi_arch),
        cmocka_unit_test(test_medium_32),
        cmocka_unit_test(test_medium_old_header),
        cmocka_unit_test(test_medium_memtest),
        cmocka_unit_test(test_medium_missing_kernel),
        cmocka_unit_test(test_medium_hdd_emulation),
        cmocka_unit_test(test_medium_bsd),
        cmocka_unit_test(test_medium_windows),
        cmocka_unit_test(test_medium_bios_versionless),
    };
    static const char *const fixtures[] = {
        FIX_EFIONLY, FIX_EFIKERNEL, FIX_64, FIX_MULTI, FIX_32, FIX_OLD,
        FIX_MEMTEST, FIX_MISSING, FIX_HDD, FIX_BSD64, FIX_BSD32, FIX_WINEFI,
        FIX_WINBIOS,
    };
    int failed = cmocka_run_group_tests_name("biosboot", heads, NULL, NULL);
    size_t i;

    for (i = 0; i < sizeof fixtures / sizeof fixtures[0]; i++) {
        if (!have(fixtures[i])) {
            printf("\n[  SKIPPED ] biosboot media: %s not found.\n"
                   "             Build them with: make test-biosboot "
                   "test-uefiboot.\n", fixtures[i]);
            return failed != 0 ? failed : test_skip_rc();
        }
    }
    return failed +
           cmocka_run_group_tests_name("biosboot media", media, NULL, NULL);
}
