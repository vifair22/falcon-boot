/* Tests for falcon-core/src/media/uefiboot.c: can an x86-64 UEFI machine
 * boot this medium.
 *
 * The kernel half runs on heads built here byte by byte, since what it reads
 * is a handful of fields. The medium half runs on
 * falcon-core/tools/mk-test-uefiboot.sh's ISOs, xorriso-built, one per shape
 * the rule has to tell apart, and on the ISO9660 suite's Rock Ridge image,
 * whose kernel is random bytes: the case the rule must NOT mark.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include <media/bootfs.h>
#include <media/family.h>
#include <media/medium.h>
#include <media/uefiboot.h>
#include <pe/pe.h>

#define FIX_BIOSONLY "build/test-uefi-biosonly.iso"
#define FIX_STUB     "build/test-uefi-stub.iso"
#define FIX_EFI      "build/test-uefi-efi.iso"
#define FIX_WINBIOS  "build/test-uefi-win-bios.iso"
#define FIX_WINEFI   "build/test-uefi-win-efi.iso"
#define FIX_RR       "build/test-iso-rr.iso"

/* Static for the reason medium_test.c gives: a medium must not move once
 * opened, and the work area is tens of kilobytes. */
static medium        g_m;
static uefiboot_work g_w;
static bootfs_ctx    g_fam_ctx;
static char          g_scratch[256 * 1024];
static uint8_t       g_head[UEFIBOOT_HEAD];

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

/* The family as the loader's survey finds it: the first volume that
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

/* Opens path, asks the whole rule, and hands back the family it used. */
static uefiboot_answer ask(const char *path, family_id *fam_out)
{
    FILE *f = fopen(path, "rb");
    uefiboot_answer a;
    family_id fam;
    long end;

    assert_non_null(f);
    assert_int_equal(fseek(f, 0, SEEK_END), 0);
    end = ftell(f);
    assert_true(end > 0);
    assert_int_equal(medium_open(&g_m, img_read, f, (uint64_t)end),
                     MEDIUM_OK);
    fam = detect(&g_m);
    a = uefiboot_medium(&g_m, fam, &g_w, g_scratch, sizeof g_scratch);
    fclose(f);
    if (fam_out != NULL)
        *fam_out = fam;
    return a;
}

/* ---- the kernel half ---- */

static void bz_head(void)
{
    memset(g_head, 0, sizeof g_head);
    g_head[0x1fe] = 0x55;
    g_head[0x1ff] = 0xaa;
    memcpy(g_head + 0x202, "HdrS", 4);
}

static void stub_head(uint16_t machine, uint16_t magic)
{
    bz_head();
    g_head[0] = 'M';
    g_head[1] = 'Z';
    g_head[0x3c] = 0x00;
    g_head[0x3d] = 0x04;
    memcpy(g_head + 0x400, "PE\0\0", 4);
    g_head[0x404] = (uint8_t)(machine & 0xffu);
    g_head[0x405] = (uint8_t)(machine >> 8);
    g_head[0x414] = 0xf0;
    g_head[0x418] = (uint8_t)(magic & 0xffu);
    g_head[0x419] = (uint8_t)(magic >> 8);
}

static void test_kernel_bzimage_no_stub(void **state)
{
    (void)state;
    bz_head();
    assert_int_equal(uefiboot_kernel(g_head, sizeof g_head), UEFIBOOT_NO);
}

static void test_kernel_x64_stub(void **state)
{
    (void)state;
    stub_head(PE_MACHINE_AMD64, 0x20bu);
    assert_int_equal(uefiboot_kernel(g_head, sizeof g_head), UEFIBOOT_YES);
}

/* A 32-bit EFI stub: a PE, so it is judged as one, and not one an x86-64
 * firmware loads. */
static void test_kernel_i386_stub(void **state)
{
    (void)state;
    stub_head(0x014cu, 0x10bu);
    assert_int_equal(uefiboot_kernel(g_head, sizeof g_head), UEFIBOOT_NO);
}

/* x86-64 in the COFF header but a PE32 optional header: not loadable. */
static void test_kernel_x64_pe32(void **state)
{
    (void)state;
    stub_head(PE_MACHINE_AMD64, 0x10bu);
    assert_int_equal(uefiboot_kernel(g_head, sizeof g_head), UEFIBOOT_NO);
}

/* The PE header lies past the window: not provable either way. */
static void test_kernel_mz_short(void **state)
{
    (void)state;
    stub_head(PE_MACHINE_AMD64, 0x20bu);
    assert_int_equal(uefiboot_kernel(g_head, 0x300), UEFIBOOT_UNKNOWN);
}

static void test_kernel_neither(void **state)
{
    (void)state;
    memset(g_head, 0x5a, sizeof g_head);
    assert_int_equal(uefiboot_kernel(g_head, sizeof g_head),
                     UEFIBOOT_UNKNOWN);
    bz_head();
    /* A head too short to reach the setup header proves nothing. */
    assert_int_equal(uefiboot_kernel(g_head, 0x204), UEFIBOOT_UNKNOWN);
    assert_int_equal(uefiboot_kernel(NULL, 16), UEFIBOOT_UNKNOWN);
}

/* ---- the medium ---- */

static void test_medium_biosonly(void **state)
{
    family_id fam;

    (void)state;
    assert_int_equal(ask(FIX_BIOSONLY, &fam), UEFIBOOT_NO);
    assert_int_not_equal(fam, FAMILY_WINDOWS);
}

static void test_medium_stub(void **state)
{
    (void)state;
    assert_int_equal(ask(FIX_STUB, NULL), UEFIBOOT_YES);
}

/* A UEFI boot image wins before the kernel is looked at: this medium's
 * kernel is the BIOS-only one. */
static void test_medium_efi_image(void **state)
{
    (void)state;
    assert_int_equal(ask(FIX_EFI, NULL), UEFIBOOT_YES);
}

static void test_medium_windows_bios(void **state)
{
    family_id fam;

    (void)state;
    assert_int_equal(ask(FIX_WINBIOS, &fam), UEFIBOOT_NO);
    assert_int_equal(fam, FAMILY_WINDOWS);
}

/* 8.1 and 10 x86: a 0xEF entry holding only a 32-bit loader. A UEFI
 * medium, so not BIOS-only, though nothing on it runs on x86-64. */
static void test_medium_windows_efi(void **state)
{
    family_id fam;

    (void)state;
    assert_int_equal(ask(FIX_WINEFI, &fam), UEFIBOOT_YES);
    assert_int_equal(fam, FAMILY_WINDOWS);
}

/* A configuration whose kernel is neither a PE nor a bzImage. */
static void test_medium_unknown_kernel(void **state)
{
    (void)state;
    if (!have(FIX_RR))
        skip();
    assert_int_equal(ask(FIX_RR, NULL), UEFIBOOT_UNKNOWN);
}

/* Windows-shaped, but the family is not known: without the family there is
 * no kernel to judge and nothing is proved. */
static void test_medium_windows_needs_family(void **state)
{
    FILE *f = fopen(FIX_WINBIOS, "rb");
    long end;

    (void)state;
    assert_non_null(f);
    assert_int_equal(fseek(f, 0, SEEK_END), 0);
    end = ftell(f);
    assert_int_equal(medium_open(&g_m, img_read, f, (uint64_t)end),
                     MEDIUM_OK);
    assert_int_equal(uefiboot_medium(&g_m, FAMILY_UNKNOWN, &g_w, g_scratch,
                                     sizeof g_scratch),
                     UEFIBOOT_UNKNOWN);
    assert_int_equal(uefiboot_has_image(&g_m, &g_w), 0);
    fclose(f);
}

static void test_arg_errors(void **state)
{
    (void)state;
    assert_int_equal(uefiboot_medium(NULL, FAMILY_UNKNOWN, &g_w, g_scratch,
                                     sizeof g_scratch),
                     UEFIBOOT_UNKNOWN);
    assert_int_equal(uefiboot_has_image(NULL, &g_w), 0);
    memset(&g_m, 0, sizeof g_m);
    assert_int_equal(uefiboot_medium(&g_m, FAMILY_WINDOWS, &g_w, g_scratch,
                                     sizeof g_scratch),
                     UEFIBOOT_UNKNOWN);
}

int run_uefiboot_tests(void)
{
    const struct CMUnitTest kernel[] = {
        cmocka_unit_test(test_kernel_bzimage_no_stub),
        cmocka_unit_test(test_kernel_x64_stub),
        cmocka_unit_test(test_kernel_i386_stub),
        cmocka_unit_test(test_kernel_x64_pe32),
        cmocka_unit_test(test_kernel_mz_short),
        cmocka_unit_test(test_kernel_neither),
        cmocka_unit_test(test_arg_errors),
    };
    const struct CMUnitTest media[] = {
        cmocka_unit_test(test_medium_biosonly),
        cmocka_unit_test(test_medium_stub),
        cmocka_unit_test(test_medium_efi_image),
        cmocka_unit_test(test_medium_windows_bios),
        cmocka_unit_test(test_medium_windows_efi),
        cmocka_unit_test(test_medium_unknown_kernel),
        cmocka_unit_test(test_medium_windows_needs_family),
    };
    int failed = cmocka_run_group_tests_name("uefiboot", kernel, NULL, NULL);

    if (!have(FIX_BIOSONLY) || !have(FIX_STUB) || !have(FIX_EFI) ||
        !have(FIX_WINBIOS) || !have(FIX_WINEFI)) {
        printf("\n[  SKIPPED ] uefiboot media: build/test-uefi-*.iso not "
               "found.\n"
               "             Build them with: make test-uefiboot.\n");
        return failed != 0 ? failed : test_skip_rc();
    }
    return failed +
           cmocka_run_group_tests_name("uefiboot media", media, NULL, NULL);
}
