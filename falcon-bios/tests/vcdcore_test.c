/* Tests for falcon-bios/src/iso/vcdcore.c: where a CD sector of a served ISO is
 * on the physical disk.
 *
 * This is the arithmetic the resident INT 13h handler performs, in the one
 * place it can be checked: the handler itself is 16-bit real-mode assembly
 * and no analyzer or test in this repository reaches it.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <iso/vcdcore.h>

#define PART_LBA 133120u

static void test_single_extent(void **state)
{
    vcd_extent ext = { 0, 8u * 1024u * 1024u };
    vcd_map m;
    uint64_t disk = 0;
    uint32_t n = 0;

    (void)state;
    assert_int_equal(vcd_build(&m, &ext, 1, PART_LBA, 512u,
                               8u * 1024u * 1024u, 0xE0u, 0x80u), VCD_OK);
    assert_int_equal(m.n_runs, 1);
    assert_int_equal(m.n_real, 4096);        /* 8 MiB / 2048 */
    assert_int_equal(m.drive, 0xE0u);
    assert_int_equal(m.phys, 0x80u);

    /* Sector 0 is the partition's first sector. */
    assert_int_equal(vcd_locate(&m, 0, 1, &disk, &n), VCD_OK);
    assert_int_equal((int)disk, PART_LBA);
    assert_int_equal(n, 1);

    /* Sector 16, the primary volume descriptor, is 64 disk sectors in. */
    assert_int_equal(vcd_locate(&m, 16, 1, &disk, &n), VCD_OK);
    assert_int_equal((int)disk, PART_LBA + 64u);

    /* A run is capped by what was asked for, not by the run's length. */
    assert_int_equal(vcd_locate(&m, 0, 8, &disk, &n), VCD_OK);
    assert_int_equal(n, 8);

    /* And by the run's length when that is shorter. */
    assert_int_equal(vcd_locate(&m, 4090, 64, &disk, &n), VCD_OK);
    assert_int_equal(n, 6);

    /* Past the end is not a sector. */
    assert_int_not_equal(vcd_locate(&m, 4096, 1, &disk, &n), VCD_OK);
}

static void test_contiguous_extents_merge(void **state)
{
    /* What a filesystem reports for a file laid down in one piece: many
     * extents, one run. */
    vcd_extent ext[3] = {
        { 0,          4u * 1024u * 1024u },
        { 4u << 20,   4u * 1024u * 1024u },
        { 8u << 20,   4u * 1024u * 1024u }
    };
    vcd_map m;

    (void)state;
    assert_int_equal(vcd_build(&m, ext, 3, PART_LBA, 512u, 12u << 20,
                               0xE0u, 0x80u), VCD_OK);
    assert_int_equal(m.n_runs, 1);
    assert_int_equal(m.n_real, (12u << 20) / 2048u);
}

static void test_fragmented(void **state)
{
    /* Two pieces, the second earlier on the disk than the first, which is
     * the case a naive implementation gets wrong by assuming order. */
    vcd_extent ext[2] = {
        { 16u << 20, 2u << 20 },
        { 4u << 20,  2u << 20 }
    };
    vcd_map m;
    uint64_t disk = 0;
    uint32_t n = 0;

    (void)state;
    assert_int_equal(vcd_build(&m, ext, 2, PART_LBA, 512u, 4u << 20,
                               0xE0u, 0x80u), VCD_OK);
    assert_int_equal(m.n_runs, 2);
    assert_int_equal(m.n_real, (4u << 20) / 2048u);

    /* The first CD sector is in the first extent, high on the disk. */
    assert_int_equal(vcd_locate(&m, 0, 1, &disk, &n), VCD_OK);
    assert_int_equal((int)(disk - PART_LBA), (16 << 20) / 512);

    /* The one after the join is in the second, low on the disk. */
    assert_int_equal(vcd_locate(&m, (2u << 20) / 2048u, 1, &disk, &n),
                     VCD_OK);
    assert_int_equal((int)(disk - PART_LBA), (4 << 20) / 512);

    /* A read that would cross the join is cut at it: the handler issues one
     * BIOS read per run and never a read that spans two. */
    assert_int_equal(vcd_locate(&m, (2u << 20) / 2048u - 4u, 64, &disk, &n),
                     VCD_OK);
    assert_int_equal(n, 4);
}

static void test_tail_extent_is_clipped(void **state)
{
    /* The last extent covers whole clusters; only the file's bytes are
     * served, and the medium is a whole number of CD sectors. */
    vcd_extent ext = { 0, 4u << 20 };
    vcd_map m;

    (void)state;
    assert_int_equal(vcd_build(&m, &ext, 1, PART_LBA, 512u,
                               (3u << 20) + 2048u, 0xE0u, 0x80u), VCD_OK);
    assert_int_equal(m.n_real, ((3u << 20) + 2048u) / 2048u);
}

static void test_tail_padding(void **state)
{
    /* The medium says it is a whole number of VCD_PAD sectors and reads the
     * difference as zeros, because a BIOS caller reads in fixed-size chunks
     * and the last one runs past the end. FreeBSD's loader reads eight
     * sectors at a time and asked for one past the end of a 521 MB medium;
     * refusing that read is refusing the medium. */
    vcd_extent ext = { 0, 8u << 20 };
    vcd_map m;
    uint16_t seg = 0;
    uint32_t phys = 0;
    uint32_t n = 0;
    uint64_t iso = (uint64_t)1000u * 2048u + 2048u;   /* 1001 CD sectors */

    (void)state;
    assert_int_equal(vcd_build(&m, &ext, 1, PART_LBA, 512u, iso,
                               0xE0u, 0x80u), VCD_OK);
    assert_int_equal(m.n_real, 1001);
    assert_int_equal(m.n_cd, 1024);          /* rounded up to VCD_PAD */
    assert_int_equal(m.n_limit, 1024 + 64);  /* and one block beyond */

    /* A disk read stops at the image's end rather than running into the
     * padding, because the handler issues one BIOS call per pass. */
    assert_int_equal(vcd_span(&m, 996, 8, &seg, &phys, &n), VCD_OK);
    assert_int_equal(n, 5);

    /* And the padding reads as zeros. */
    assert_int_equal(vcd_span(&m, 1001, 8, &seg, &phys, &n), VCD_FROM_ZERO);
    assert_int_equal(n, 8);
    assert_int_equal(vcd_span(&m, 1020, 8, &seg, &phys, &n), VCD_FROM_ZERO);
    assert_int_equal(n, 8);

    /* The sector AT the declared size answers too, because that is the one
     * FreeBSD's loader asked for. */
    assert_int_equal(vcd_span(&m, 1024, 1, &seg, &phys, &n), VCD_FROM_ZERO);

    /* A block past that is not a sector at all. */
    assert_int_not_equal(vcd_span(&m, 1024 + 64, 1, &seg, &phys, &n),
                         VCD_FROM_ZERO);

    /* An override may not be placed in the padding: there is nothing there
     * to override. */
    assert_int_equal(vcd_override_add(&m, 1001, 1, 0x8000u),
                     VCD_ERR_BAD_ARG);
}

static void test_misaligned_extent_refused(void **state)
{
    /* An extent that does not start on a CD sector cannot be served without
     * splitting a read across two of them, and the handler does not. */
    vcd_extent ext[2] = { { 0, 1024u }, { 1024u, 4u << 20 } };
    vcd_map m;

    (void)state;
    assert_int_equal(vcd_build(&m, ext, 2, PART_LBA, 512u, 4u << 20,
                               0xE0u, 0x80u), VCD_ERR_ALIGN);
}

static void test_too_many_runs(void **state)
{
    /* A medium in more pieces than the handler will scan is one to copy
     * again, and the loader says so rather than serving it slowly. */
    static vcd_extent ext[VCD_MAX_RUNS + 4u];
    vcd_map m;
    size_t i;

    (void)state;
    for (i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        /* Deliberately non-adjacent, so nothing merges. */
        ext[i].off = (uint64_t)(i * 2u) * (1u << 20);
        ext[i].len = 1u << 20;
    }
    assert_int_equal(vcd_build(&m, ext, sizeof ext / sizeof ext[0], PART_LBA,
                               512u, (uint64_t)(sizeof ext / sizeof ext[0]) *
                               (1u << 20), 0xE0u, 0x80u),
                     VCD_ERR_TOO_MANY);
}

static void test_bad_args(void **state)
{
    vcd_extent ext = { 0, 4u << 20 };
    vcd_map m;
    uint64_t disk = 0;
    uint32_t n = 0;

    (void)state;
    assert_int_equal(vcd_build((vcd_map *)0, &ext, 1, PART_LBA, 512u,
                               4u << 20, 0xE0u, 0x80u), VCD_ERR_BAD_ARG);
    assert_int_equal(vcd_build(&m, &ext, 0, PART_LBA, 512u, 4u << 20,
                               0xE0u, 0x80u), VCD_ERR_EMPTY);
    assert_int_equal(vcd_build(&m, &ext, 1, PART_LBA, 512u, 0, 0xE0u, 0x80u),
                     VCD_ERR_EMPTY);
    /* A sector size a CD sector is not a whole number of. */
    assert_int_equal(vcd_build(&m, &ext, 1, PART_LBA, 3000u, 4u << 20,
                               0xE0u, 0x80u), VCD_ERR_BAD_ARG);

    assert_int_equal(vcd_build(&m, &ext, 1, PART_LBA, 512u, 4u << 20,
                               0xE0u, 0x80u), VCD_OK);
    assert_int_equal(vcd_locate(&m, 0, 0, &disk, &n), VCD_ERR_BAD_ARG);
    assert_int_equal(vcd_locate((const vcd_map *)0, 0, 1, &disk, &n),
                     VCD_ERR_BAD_ARG);
}

static void test_sector_size_is_the_disks_own(void **state)
{
    /* The disk address packet's LBA is in the drive's units, so a 2048-byte
     * drive steps one per CD sector rather than four. */
    vcd_extent ext = { 0, 4u << 20 };
    vcd_map m;
    uint64_t disk = 0;
    uint32_t n = 0;

    (void)state;
    assert_int_equal(vcd_build(&m, &ext, 1, 1000u, 2048u, 4u << 20,
                               0xE0u, 0x80u), VCD_OK);
    assert_int_equal((int)m.per_cd, 1);
    assert_int_equal((int)m.run[0].disk_lba, 1000);
    assert_int_equal(vcd_locate(&m, 2, 1, &disk, &n), VCD_OK);
    assert_int_equal((int)(disk - 1000u), 2);

    /* A drive whose sector is larger than a CD's cannot serve one. */
    assert_int_equal(vcd_build(&m, &ext, 1, 1000u, 4096u, 4u << 20,
                               0xE0u, 0x80u), VCD_ERR_BAD_ARG);
}

static void test_overrides(void **state)
{
    /* What makes a medium bootable that is not bootable as it lies on the
     * stick: a few sectors served from memory instead of the disk. */
    vcd_extent ext = { 0, 8u << 20 };
    vcd_map m;
    uint16_t seg = 0;
    uint32_t phys = 0;
    uint32_t n = 0;

    (void)state;
    assert_int_equal(vcd_build(&m, &ext, 1, PART_LBA, 512u, 8u << 20,
                               0xE0u, 0x80u), VCD_OK);
    assert_int_equal((int)m.n_over, 0);

    /* With none, a read is bounded only by what was asked for. */
    assert_int_equal(vcd_span(&m, 0, 64, &seg, &phys, &n), VCD_OK);
    assert_int_equal(n, 64);

    assert_int_equal(vcd_override_add(&m, 100, 2, 0x8000u), VCD_OK);

    /* A read that starts before one stops short of it, so the handler never
     * issues a BIOS call that spans the two. */
    assert_int_equal(vcd_span(&m, 90, 64, &seg, &phys, &n), VCD_OK);
    assert_int_equal(n, 10);

    /* A read that starts inside one comes from memory. */
    assert_int_equal(vcd_span(&m, 100, 64, &seg, &phys, &n), VCD_FROM_MEMORY);
    assert_int_equal(n, 2);
    assert_int_equal(seg, 0x8000);

    /* And from the right part of it: the second sector is 128 paragraphs
     * further in. */
    assert_int_equal(vcd_span(&m, 101, 64, &seg, &phys, &n), VCD_FROM_MEMORY);
    assert_int_equal(n, 1);
    assert_int_equal(seg, 0x8080);

    /* Past it is the disk again. */
    assert_int_equal(vcd_span(&m, 102, 64, &seg, &phys, &n), VCD_OK);
    assert_int_equal(n, 64);

    /* Overlapping is refused: two answers for one sector is not something
     * the handler can give. */
    assert_int_equal(vcd_override_add(&m, 101, 4, 0x9000u), VCD_ERR_BAD_ARG);
    /* So is one that runs off the end of the medium. */
    assert_int_equal(vcd_override_add(&m, m.n_real - 1u, 4, 0x9000u),
                     VCD_ERR_BAD_ARG);

    assert_int_equal(vcd_override_add(&m, 200, 1, 0x9000u), VCD_OK);
    assert_int_equal(vcd_override_add(&m, 300, 1, 0xA000u), VCD_OK);
    assert_int_equal(vcd_override_add(&m, 400, 1, 0xB000u), VCD_OK);
    assert_int_equal(vcd_override_add(&m, 500, 1, 0xC000u), VCD_OK);
    assert_int_equal(vcd_override_add(&m, 600, 1, 0xD000u), VCD_OK);
    assert_int_equal(vcd_override_add(&m, 700, 1, 0xE000u), VCD_OK);
    assert_int_equal(vcd_override_add(&m, 800, 1, 0xF000u), VCD_OK);
    assert_int_equal(vcd_override_add(&m, 900, 1, 0x1000u), VCD_ERR_TOO_MANY);

    /* The nearest override above is the one a read stops at, not the first
     * in the table. */
    assert_int_equal(vcd_span(&m, 250, 1000, &seg, &phys, &n), VCD_OK);
    assert_int_equal(n, 50);
}

static void test_extended_override_and_extend(void **state)
{
    /* What Windows needs: the medium grows past the image, and the sectors
     * it grew by are served from memory above a megabyte, which real mode
     * cannot address and the BIOS has to move. */
    vcd_extent ext = { 0, 8u << 20 };
    vcd_map m;
    uint16_t seg = 0;
    uint32_t phys = 0;
    uint32_t n = 0;

    (void)state;
    assert_int_equal(vcd_build(&m, &ext, 1, PART_LBA, 512u, 8u << 20,
                               0xE0u, 0x80u), VCD_OK);
    assert_int_equal(m.n_real, 4096);

    /* Nothing may be placed past the image until the medium says it is
     * there. */
    assert_int_equal(vcd_override_add_ext(&m, 4096, 8, 0x4000000u),
                     VCD_ERR_BAD_ARG);

    assert_int_equal(vcd_extend(&m, 8), VCD_OK);
    assert_int_equal(m.n_real, 4104);
    assert_int_equal(vcd_override_add_ext(&m, 4096, 8, 0x4000000u), VCD_OK);

    /* The appended sectors come from the physical address, offset by how
     * far into the override they are. */
    assert_int_equal(vcd_span(&m, 4096, 64, &seg, &phys, &n),
                     VCD_FROM_EXTENDED);
    assert_int_equal(n, 8);
    assert_int_equal((int)phys, 0x4000000);

    assert_int_equal(vcd_span(&m, 4098, 64, &seg, &phys, &n),
                     VCD_FROM_EXTENDED);
    assert_int_equal(n, 6);
    assert_int_equal((int)phys, 0x4000000 + 2 * 2048);

    /* A disk read below it still stops at its edge. */
    assert_int_equal(vcd_span(&m, 4090, 64, &seg, &phys, &n), VCD_OK);
    assert_int_equal(n, 6);

    /* And the padding past the extended medium still reads as zeros. */
    assert_int_equal(vcd_span(&m, 4104, 8, &seg, &phys, &n), VCD_FROM_ZERO);
}

static void test_redirect_run(void **state)
{
    /* What an ISO 9660-only Windows medium needs: boot.wim is relocated to
     * sectors grown past the image, and those sectors are served from where
     * the original boot.wim already lies on the disk (vcd_add_run) rather than
     * copied into memory. Here the original file is cd 16 onward. */
    vcd_extent ext = { 0, 8u << 20 };
    vcd_map m;
    uint16_t seg = 0;
    uint32_t phys = 0;
    uint32_t n = 0;
    uint64_t disk = 0;

    (void)state;
    assert_int_equal(vcd_build(&m, &ext, 1, PART_LBA, 512u, 8u << 20,
                               0xE0u, 0x80u), VCD_OK);
    assert_int_equal(m.n_real, 4096);

    /* A run may not be placed past the image until the medium says it is
     * there, nor may it overlap the base ISO's own run. */
    assert_int_equal(vcd_add_run(&m, 4096, 100, PART_LBA + 64u),
                     VCD_ERR_BAD_ARG);
    assert_int_equal(vcd_extend(&m, 100), VCD_OK);
    assert_int_equal(vcd_add_run(&m, 0, 4, PART_LBA), VCD_ERR_BAD_ARG);

    /* Redirect the grown sectors to the disk location of cd 16 onward. */
    assert_int_equal(vcd_locate(&m, 16, 1, &disk, &n), VCD_OK);
    assert_int_equal((int)disk, PART_LBA + 64u);
    assert_int_equal(vcd_add_run(&m, 4096, 100, disk), VCD_OK);
    assert_int_equal(m.n_runs, 2);

    /* A read of the redirected region resolves to the original's disk sectors,
     * offset for how far in it is, exactly as a read of cd 16 onward does. */
    assert_int_equal(vcd_span(&m, 4096, 64, &seg, &phys, &n), VCD_OK);
    assert_int_equal(vcd_locate(&m, 4096, 64, &disk, &n), VCD_OK);
    assert_int_equal((int)disk, PART_LBA + 64u);
    assert_int_equal(vcd_locate(&m, 4106, 8, &disk, &n), VCD_OK);
    assert_int_equal((int)disk, PART_LBA + 64u + 10u * 4u);

    /* A memory override at the redirect's start wins over the run beneath it,
     * which is how the relocated file's patched header sits over the original
     * header the redirect otherwise serves. */
    assert_int_equal(vcd_override_add(&m, 4096, 1, 0xB000u), VCD_OK);
    assert_int_equal(vcd_span(&m, 4096, 64, &seg, &phys, &n), VCD_FROM_MEMORY);
    assert_int_equal(n, 1);
    assert_int_equal(vcd_span(&m, 4097, 64, &seg, &phys, &n), VCD_OK);
}

int run_vcdcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_single_extent),
        cmocka_unit_test(test_contiguous_extents_merge),
        cmocka_unit_test(test_fragmented),
        cmocka_unit_test(test_tail_extent_is_clipped),
        cmocka_unit_test(test_tail_padding),
        cmocka_unit_test(test_misaligned_extent_refused),
        cmocka_unit_test(test_too_many_runs),
        cmocka_unit_test(test_bad_args),
        cmocka_unit_test(test_sector_size_is_the_disks_own),
        cmocka_unit_test(test_overrides),
        cmocka_unit_test(test_extended_override_and_extend),
        cmocka_unit_test(test_redirect_run),
    };

    return cmocka_run_group_tests_name("falcon-bios", tests, NULL, NULL);
}
