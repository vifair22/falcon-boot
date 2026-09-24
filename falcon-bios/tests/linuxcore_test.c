/* Tests for falcon-bios/src/linuxcore.c: the addresses the x86 boot protocol
 * is handed, and the padding between concatenated initrds.
 *
 * Every case here is a kernel this loader must refuse or an address it must
 * get exactly right, because none of them says anything when it is wrong: a
 * kernel placed past the identity map triple-faults, and an initrd segment
 * one byte short of alignment is reported by the kernel as a corrupt
 * archive. The padding case is a regression test - it shipped unpadded once,
 * and Arch booted while Debian did not, purely because Arch's initrd happens
 * to be a multiple of four.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <linuxcore.h>

#define MAP_END   0x100000000ull  /* biosdefs.h ADDR_END */
#define PREF_LOAD 0x01000000ull   /* biosdefs.h KERNEL_ADDR */
#define SETUP_CAP (65u * 512u)

static uint8_t g_setup[2048];

static void wr16(unsigned off, uint16_t v)
{
    g_setup[off] = (uint8_t)(v & 0xFFu);
    g_setup[off + 1u] = (uint8_t)(v >> 8);
}

static void wr32(unsigned off, uint32_t v)
{
    wr16(off, (uint16_t)(v & 0xFFFFu));
    wr16(off + 2u, (uint16_t)(v >> 16));
}

static void wr64(unsigned off, uint64_t v)
{
    wr32(off, (uint32_t)(v & 0xFFFFFFFFu));
    wr32(off + 4u, (uint32_t)(v >> 32));
}

/* A kernel this loader accepts: relocatable, 2.15, 64-bit entry, 2 MiB
 * alignment, 16 sectors of setup. */
static void good_setup(void)
{
    memset(g_setup, 0, sizeof g_setup);
    wr16(HDR_BOOT_FLAG, 0xAA55u);
    wr32(HDR_HEADER, HDRS_MAGIC);
    wr16(HDR_VERSION, 0x020Fu);
    wr16(HDR_XLOADFLAGS, XLF_KERNEL_64);
    g_setup[HDR_SETUP_SECTS] = 16;
    g_setup[HDR_RELOCATABLE] = 1;
    wr32(HDR_KERNEL_ALIGNMENT, 0x200000u);
    wr32(HDR_INIT_SIZE, 0x2000000u);
    wr32(HDR_CMDLINE_SIZE, 2047u);
    wr32(HDR_INITRD_ADDR_MAX, 0x7FFFFFFFu);
}

static linuxcore_status geom(uint64_t file_len, linux_geom *out)
{
    return linuxcore_geometry(g_setup, sizeof g_setup, file_len, PREF_LOAD,
                              MAP_END, SETUP_CAP, out);
}

static void test_geometry_ok(void **state)
{
    linux_geom g;

    (void)state;
    good_setup();
    /* Smaller than init_size, which is the usual shape: the kernel needs
     * room to decompress into that the image does not occupy. */
    assert_int_equal(geom(8u * 1024u * 1024u, &g), LINUXCORE_OK);
    /* (setup_sects + 1) sectors: the boot sector counts. */
    assert_int_equal(g.setup_len, 17u * 512u);
    assert_int_equal(g.pm_len, 8u * 1024u * 1024u - 17u * 512u);
    /* Relocatable, so the preferred load address rounded up to alignment. */
    assert_int_equal(g.load_at, 0x01000000ull);
    assert_int_equal(g.init_size, 0x2000000u);
    assert_int_equal(g.cmdline_cap, 2047u);
    assert_int_equal(g.initrd_max, 0x7FFFFFFFu >= MAP_END ? MAP_END - 1u
                                                          : 0x7FFFFFFFu);
    assert_int_equal(g.version, 0x020Fu);
}

/* setup_sects of 0 has always meant 4, and a kernel whose init_size is
 * smaller than the image needs the image's size instead. */
static void test_geometry_defaults(void **state)
{
    linux_geom g;

    (void)state;
    good_setup();
    g_setup[HDR_SETUP_SECTS] = 0;
    wr32(HDR_INIT_SIZE, 16u);
    wr32(HDR_CMDLINE_SIZE, 0);
    wr32(HDR_KERNEL_ALIGNMENT, 0);
    assert_int_equal(geom(8u * 1024u * 1024u, &g), LINUXCORE_OK);
    assert_int_equal(g.setup_len, 5u * 512u);
    assert_int_equal(g.init_size, g.pm_len);
    assert_int_equal(g.cmdline_cap, 4096u);
    /* No alignment named: 2 MiB, which is what the protocol has always
     * meant by a zero there. */
    assert_int_equal(g.load_at, 0x01000000ull);
}

/* A kernel that is not relocatable goes where it says and nowhere else. */
static void test_geometry_fixed_address(void **state)
{
    linux_geom g;

    (void)state;
    good_setup();
    g_setup[HDR_RELOCATABLE] = 0;
    wr64(HDR_PREF_ADDRESS, 0x04000000ull);
    assert_int_equal(geom(8u * 1024u * 1024u, &g), LINUXCORE_OK);
    assert_int_equal(g.load_at, 0x04000000ull);

    /* ... and one that is neither relocatable nor willing to say where is
     * refused rather than guessed at. */
    wr64(HDR_PREF_ADDRESS, 0);
    assert_int_equal(geom(8u * 1024u * 1024u, &g), LINUXCORE_ERR_NOT_RELOC);

    /* ... and one whose address is below the floor, here the classic
     * 1 MiB, is refused: that is where stage 3 runs. The floor itself is
     * the lowest address taken. */
    wr64(HDR_PREF_ADDRESS, 0x00100000ull);
    assert_int_equal(geom(8u * 1024u * 1024u, &g), LINUXCORE_ERR_TOO_LOW);
    wr64(HDR_PREF_ADDRESS, PREF_LOAD);
    assert_int_equal(geom(8u * 1024u * 1024u, &g), LINUXCORE_OK);
    assert_int_equal(g.load_at, PREF_LOAD);
}

/* The initrd ceiling is the kernel's, clamped to what the loader mapped.
 * Nothing may be written outside the identity map. */
static void test_geometry_initrd_ceiling(void **state)
{
    linux_geom g;

    (void)state;
    good_setup();
    wr32(HDR_INITRD_ADDR_MAX, 0);
    assert_int_equal(geom(8u * 1024u * 1024u, &g), LINUXCORE_OK);
    assert_int_equal(g.initrd_max, 0x37FFFFFFu);   /* the protocol default */

    wr32(HDR_INITRD_ADDR_MAX, 0xFFFFFFFFu);
    assert_int_equal(geom(8u * 1024u * 1024u, &g), LINUXCORE_OK);
    assert_int_equal(g.initrd_max, MAP_END - 1u);
}

static void test_geometry_refusals(void **state)
{
    linux_geom g;

    (void)state;
    assert_int_equal(linuxcore_geometry(NULL, 1024, 1 << 20, PREF_LOAD,
                                        MAP_END, SETUP_CAP, &g),
                     LINUXCORE_ERR_ARG);
    good_setup();
    assert_int_equal(linuxcore_geometry(g_setup, sizeof g_setup, 1024,
                                        PREF_LOAD, MAP_END, SETUP_CAP, NULL),
                     LINUXCORE_ERR_ARG);
    /* Under two sectors: "HdrS" is at 0x202 and would be off the end. */
    assert_int_equal(linuxcore_geometry(g_setup, 512, 1 << 20, PREF_LOAD,
                                        MAP_END, SETUP_CAP, &g),
                     LINUXCORE_ERR_TOO_SMALL);
    assert_int_equal(geom(600, &g), LINUXCORE_ERR_TOO_SMALL);

    good_setup();
    wr16(HDR_BOOT_FLAG, 0);
    assert_int_equal(geom(1 << 20, &g), LINUXCORE_ERR_NOT_BZIMAGE);
    good_setup();
    wr32(HDR_HEADER, 0);
    assert_int_equal(geom(1 << 20, &g), LINUXCORE_ERR_NOT_BZIMAGE);

    /* 2.11 and older have no xloadflags to ask. */
    good_setup();
    wr16(HDR_VERSION, 0x020Bu);
    assert_int_equal(geom(1 << 20, &g), LINUXCORE_ERR_VERSION);

    /* A 32-bit kernel is not a refusal: it takes the same entry, and the
     * geometry only says which width it is. */
    good_setup();
    wr16(HDR_XLOADFLAGS, 0);
    assert_int_equal(geom(1 << 20, &g), LINUXCORE_OK);
    assert_int_equal(g.is_64, 0);
    good_setup();
    assert_int_equal(geom(1 << 20, &g), LINUXCORE_OK);
    assert_int_equal(g.is_64, 1);

    /* Setup longer than the buffer that will hold it. */
    good_setup();
    g_setup[HDR_SETUP_SECTS] = 255;
    assert_int_equal(linuxcore_geometry(g_setup, sizeof g_setup, 1 << 20,
                                        PREF_LOAD, MAP_END, 4096u, &g),
                     LINUXCORE_ERR_SETUP_LONG);

    /* All setup and no kernel, including the exact boundary. */
    good_setup();
    assert_int_equal(geom(17u * 512u, &g), LINUXCORE_ERR_NO_KERNEL);
    assert_int_equal(geom(17u * 512u + 1u, &g), LINUXCORE_OK);

    /* Wants more than the loader maps: init_size is the number that
     * matters, not the image's length. The kernel lands at 16 MiB and the
     * map ends at 1 GiB, so 0x3F000000 reaches the last mapped byte exactly
     * and one more does not fit. */
    good_setup();
    wr32(HDR_INIT_SIZE, 0x3F000000u);
    assert_int_equal(geom(1 << 20, &g), LINUXCORE_OK);
    wr32(HDR_INIT_SIZE, 0x3F000001u);
    assert_int_equal(geom(1 << 20, &g), LINUXCORE_ERR_NO_ROOM);
}

/* THE regression test. Every segment starts on a 4-byte boundary. */
static void test_initrd_padding(void **state)
{
    /* Debian's, which is not a multiple of four, then the hook. */
    static const uint64_t debian[] = { 113383147ull, 4096ull };
    /* Arch's, which is - the reason that medium booted and Debian did not. */
    static const uint64_t arch[] = { 113383148ull, 4096ull };
    uint64_t pad = 99;

    (void)state;
    assert_int_equal(linuxcore_align4(0), 0);
    assert_int_equal(linuxcore_align4(1), 4);
    assert_int_equal(linuxcore_align4(3), 4);
    assert_int_equal(linuxcore_align4(4), 4);
    assert_int_equal(linuxcore_align4(5), 8);

    assert_int_equal(linuxcore_initrd_len(debian, 2), 113383148ull + 4096ull);
    assert_int_equal(linuxcore_initrd_at(debian, 2, 0, &pad), 0);
    assert_int_equal(pad, 1);
    /* The second archive starts on the boundary, one byte past where an
     * unpadded concatenation would have put it. */
    assert_int_equal(linuxcore_initrd_at(debian, 2, 1, &pad), 113383148ull);
    assert_int_equal(pad, 0);

    assert_int_equal(linuxcore_initrd_len(arch, 2), 113383148ull + 4096ull);
    assert_int_equal(linuxcore_initrd_at(arch, 2, 1, &pad), 113383148ull);
    assert_int_equal(pad, 0);

    assert_int_equal(linuxcore_initrd_len(NULL, 2), 0);
    assert_int_equal(linuxcore_initrd_len(debian, 0), 0);
    assert_int_equal(linuxcore_initrd_at(debian, 2, 2, &pad), 0);
    assert_int_equal(pad, 0);
    assert_int_equal(linuxcore_initrd_at(debian, 2, 1, NULL), 113383148ull);
}

int run_linuxcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_geometry_ok),
        cmocka_unit_test(test_geometry_defaults),
        cmocka_unit_test(test_geometry_fixed_address),
        cmocka_unit_test(test_geometry_initrd_ceiling),
        cmocka_unit_test(test_geometry_refusals),
        cmocka_unit_test(test_initrd_padding),
    };

    return cmocka_run_group_tests_name("bios linuxcore", tests, NULL, NULL);
}
