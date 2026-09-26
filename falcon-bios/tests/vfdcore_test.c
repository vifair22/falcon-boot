/* Tests for falcon-bios/src/vfd/vfdcore.c: the geometry of an emulated floppy and
 * the CHS-to-LBA arithmetic the resident handler copies.
 *
 * The handler is 16-bit real-mode assembly no analyzer or test reaches
 * (docs/notes/bios-route.md Section 7.2), so what it must not get wrong is
 * worked out here.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <vfd/vfdcore.h>

static void test_geometry(void **state)
{
    vfd_geom g;

    (void)state;
    assert_int_equal(vfd_geometry(VFD_MEDIA_144M, 1474560u, &g), VFD_OK);
    assert_int_equal(g.cylinders, 80);
    assert_int_equal(g.heads, 2);
    assert_int_equal(g.sectors, 18);
    assert_int_equal(g.total_sectors, 2880u);
    assert_int_equal(g.bios_type, 3);

    assert_int_equal(vfd_geometry(VFD_MEDIA_288M, 2949120u, &g), VFD_OK);
    assert_int_equal(g.sectors, 36);
    assert_int_equal(g.total_sectors, 5760u);
    assert_int_equal(g.bios_type, 4);

    assert_int_equal(vfd_geometry(VFD_MEDIA_120M, 1228800u, &g), VFD_OK);
    assert_int_equal(g.sectors, 15);
    assert_int_equal(g.total_sectors, 2400u);
    assert_int_equal(g.bios_type, 1);

    /* A short image is fine: the tail reads as zeros. */
    assert_int_equal(vfd_geometry(VFD_MEDIA_144M, 512u, &g), VFD_OK);
}

static void test_geometry_refusals(void **state)
{
    vfd_geom g;

    (void)state;
    assert_int_equal(vfd_geometry(0u, 0u, &g), VFD_ERR_MEDIA);
    assert_int_equal(vfd_geometry(4u, 0u, &g), VFD_ERR_MEDIA);   /* HDD */
    assert_int_equal(vfd_geometry(VFD_MEDIA_144M, 0u, NULL),
                     VFD_ERR_BAD_ARG);
    /* One byte past what the geometry holds. */
    assert_int_equal(vfd_geometry(VFD_MEDIA_144M, 1474561u, &g),
                     VFD_ERR_SIZE);
}

static void test_chs_to_lba(void **state)
{
    vfd_geom g;
    uint32_t lba = 999u;

    (void)state;
    assert_int_equal(vfd_geometry(VFD_MEDIA_144M, 1474560u, &g), VFD_OK);

    /* The first sector: C0 H0 S1 is LBA 0. */
    assert_int_equal(vfd_chs_to_lba(&g, 0, 0, 1, &lba), VFD_OK);
    assert_int_equal(lba, 0u);

    /* The head advances by one track (18 sectors). */
    assert_int_equal(vfd_chs_to_lba(&g, 0, 1, 1, &lba), VFD_OK);
    assert_int_equal(lba, 18u);

    /* The cylinder advances by both heads (36 sectors). */
    assert_int_equal(vfd_chs_to_lba(&g, 1, 0, 1, &lba), VFD_OK);
    assert_int_equal(lba, 36u);

    /* Last sector of the medium: C79 H1 S18. */
    assert_int_equal(vfd_chs_to_lba(&g, 79, 1, 18, &lba), VFD_OK);
    assert_int_equal(lba, 2879u);

    /* Mid-track. */
    assert_int_equal(vfd_chs_to_lba(&g, 2, 1, 5, &lba), VFD_OK);
    assert_int_equal(lba, (2u * 2u + 1u) * 18u + 4u);
}

static void test_chs_refusals(void **state)
{
    vfd_geom g;
    uint32_t lba = 7u;

    (void)state;
    assert_int_equal(vfd_geometry(VFD_MEDIA_144M, 1474560u, &g), VFD_OK);
    assert_int_equal(vfd_chs_to_lba(&g, 80, 0, 1, &lba), VFD_ERR_RANGE);
    assert_int_equal(vfd_chs_to_lba(&g, 0, 2, 1, &lba), VFD_ERR_RANGE);
    assert_int_equal(vfd_chs_to_lba(&g, 0, 0, 0, &lba), VFD_ERR_RANGE);
    assert_int_equal(vfd_chs_to_lba(&g, 0, 0, 19, &lba), VFD_ERR_RANGE);
    assert_int_equal(vfd_chs_to_lba(&g, 0, 0, 1, NULL), VFD_ERR_BAD_ARG);
    assert_int_equal(vfd_chs_to_lba(NULL, 0, 0, 1, &lba), VFD_ERR_BAD_ARG);
    /* Nothing was written into lba on a refusal. */
    assert_int_equal(lba, 7u);
}

static void test_run_on_track(void **state)
{
    vfd_geom g;
    uint32_t n = 0;

    (void)state;
    assert_int_equal(vfd_geometry(VFD_MEDIA_144M, 1474560u, &g), VFD_OK);

    /* A whole track from its start. */
    assert_int_equal(vfd_run_on_track(&g, 0, 18u, &n), VFD_OK);
    assert_int_equal(n, 18u);

    /* Asking for more than a track stops at the track boundary. */
    assert_int_equal(vfd_run_on_track(&g, 0, 40u, &n), VFD_OK);
    assert_int_equal(n, 18u);

    /* From the middle of a track, only its remainder. */
    assert_int_equal(vfd_run_on_track(&g, 15u, 18u, &n), VFD_OK);
    assert_int_equal(n, 3u);

    /* Asking for less than the track has. */
    assert_int_equal(vfd_run_on_track(&g, 0, 1u, &n), VFD_OK);
    assert_int_equal(n, 1u);

    /* At the very end, capped by the medium as well as the track. */
    assert_int_equal(vfd_run_on_track(&g, 2879u, 18u, &n), VFD_OK);
    assert_int_equal(n, 1u);

    assert_int_equal(vfd_run_on_track(&g, 2880u, 1u, &n), VFD_ERR_RANGE);
    assert_int_equal(vfd_run_on_track(&g, 0, 0u, &n), VFD_ERR_BAD_ARG);
}

/* The inverse the floppy reader uses: every sector of a 1.44 MB floppy goes
 * to a CHS that vfd_chs_to_lba takes straight back to it. */
static void test_lba_to_chs_round_trip(void **state)
{
    vfd_geom g;
    uint32_t lba;

    (void)state;
    assert_int_equal(vfd_geometry(VFD_MEDIA_144M, 1474560u, &g), VFD_OK);
    for (lba = 0; lba < g.total_sectors; lba++) {
        uint16_t c;
        uint8_t h;
        uint8_t s;
        uint32_t back;

        assert_int_equal(vfd_lba_to_chs(&g, lba, &c, &h, &s), VFD_OK);
        assert_int_equal(vfd_chs_to_lba(&g, c, h, s, &back), VFD_OK);
        assert_int_equal(back, lba);
    }
}

static void test_lba_to_chs_points(void **state)
{
    vfd_geom g;
    uint16_t c;
    uint8_t h;
    uint8_t s;

    (void)state;
    assert_int_equal(vfd_geometry(VFD_MEDIA_144M, 1474560u, &g), VFD_OK);
    /* Stage 2's first sector (STAGE2_LBA 34): cylinder 0, head 1, sector 17. */
    assert_int_equal(vfd_lba_to_chs(&g, 34u, &c, &h, &s), VFD_OK);
    assert_int_equal(c, 0);
    assert_int_equal(h, 1);
    assert_int_equal(s, 17);
    /* The last sector. */
    assert_int_equal(vfd_lba_to_chs(&g, 2879u, &c, &h, &s), VFD_OK);
    assert_int_equal(c, 79);
    assert_int_equal(h, 1);
    assert_int_equal(s, 18);
    assert_int_equal(vfd_lba_to_chs(&g, 2880u, &c, &h, &s), VFD_ERR_RANGE);
    assert_int_equal(vfd_lba_to_chs((const vfd_geom *)0, 0, &c, &h, &s),
                     VFD_ERR_BAD_ARG);
    g.sectors = 0;
    assert_int_equal(vfd_lba_to_chs(&g, 0, &c, &h, &s), VFD_ERR_BAD_ARG);
}

/* What SeaBIOS and a real 1.44 MB drive answer AH=08h with: CH 79 (the last
 * cylinder), CL 18, DH 1. And a disk whose cylinder count needs CL's top
 * bits. */
static void test_geometry_int13(void **state)
{
    vfd_geom g;

    (void)state;
    assert_int_equal(vfd_geometry_int13(79u, 18u, 1u, &g), VFD_OK);
    assert_int_equal(g.cylinders, 80);
    assert_int_equal(g.heads, 2);
    assert_int_equal(g.sectors, 18);
    assert_int_equal(g.total_sectors, 2880u);
    assert_int_equal(g.media, 0);

    /* Cylinder index 0x3FF: CH 0xFF, CL's top bits 11. */
    assert_int_equal(vfd_geometry_int13(0xFFu, 0xC0u | 63u, 254u, &g), VFD_OK);
    assert_int_equal(g.cylinders, 1024);
    assert_int_equal(g.heads, 255);
    assert_int_equal(g.sectors, 63);

    assert_int_equal(vfd_geometry_int13(79u, 0u, 1u, &g), VFD_ERR_RANGE);
    assert_int_equal(vfd_geometry_int13(79u, 18u, 0xFFu, &g), VFD_ERR_RANGE);
    assert_int_equal(vfd_geometry_int13(79u, 18u, 1u, (vfd_geom *)0),
                     VFD_ERR_BAD_ARG);
}

int run_vfdcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_geometry),
        cmocka_unit_test(test_geometry_refusals),
        cmocka_unit_test(test_chs_to_lba),
        cmocka_unit_test(test_chs_refusals),
        cmocka_unit_test(test_run_on_track),
        cmocka_unit_test(test_lba_to_chs_round_trip),
        cmocka_unit_test(test_lba_to_chs_points),
        cmocka_unit_test(test_geometry_int13),
    };
    return cmocka_run_group_tests_name("vfdcore", tests, NULL, NULL);
}
