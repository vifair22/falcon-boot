/* Synthetic-volume tests for exfat_file_extents, the call the injection
 * framework of docs/DESIGN.md Section 6.2 rebuilds an ISO's physical layout
 * from after ExitBootServices.
 *
 * What this group has to prove is narrower than "the driver reads the right
 * bytes" and harder to get from a real image: the extent map is a promise
 * about *where* a file lives, and the only way to falsify it is to build a
 * volume whose fragmentation is known exactly. mkfs.exfat plus a copy will
 * not produce a five-way fragmented file on demand, and it will never produce
 * a chain that ends early or links to a bad cluster, so everything here is
 * built byte by byte.
 *
 * Three contracts are pinned:
 *
 *   coalescing   fragmentation is a property of the FAT chain, not of the
 *                file: clusters that happen to be adjacent on disk merge, so
 *                a contiguous file is one extent however many clusters it
 *                spans, and a chained-but-adjacent file is one extent too.
 *
 *   rounding     the extents cover the valid data length EXACTLY. The tail of
 *                the final cluster is excluded, so the lengths sum to the
 *                size. A consumer that wants sector granularity rounds the
 *                last length up itself (exfat.h says so, and the slack is
 *                inside the allocation, so that is safe). A test here pins
 *                the unrounded value, because rounding in the driver would
 *                silently hand a device-mapper table bytes the file does not
 *                own.
 *
 *   sizing       out may be NULL when cap is 0, and *count always receives
 *                the true extent count, so the two-call sizing idiom works
 *                and a short array never truncates silently.
 *
 * Geometry: 512-byte sectors, 1024-byte clusters, 1024 clusters. The FAT is
 * therefore 9 sectors long, which matters for the IO-error test: the driver
 * caches one FAT sector, so a chain whose links live in different FAT sectors
 * is the only way to aim a failure at one specific hop.
 */

#include "synth.h"
#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <fs/exfat.h>

#define EX_BPS      9u      /* 512-byte sectors */
#define EX_SPC      1u      /* 1024-byte clusters */
#define EX_CLUSTERS 1024u
#define EX_CB       (1u << (EX_BPS + EX_SPC))

/* Entries for clusters 2, 520, 130, 390 and 260 live in FAT sectors 0, 4, 1,
 * 3 and 2, so no two consecutive hops of this chain share a cached FAT
 * sector. Deliberately not ascending: a driver that assumed the chain runs
 * forward would produce plausible-looking extents in the wrong order. */
static const uint32_t frag_chain[5] = { 2u, 520u, 130u, 390u, 260u };
#define FRAG_NCL 5u

/* Two adjacent runs and a lone cluster: the case that separates "one extent
 * per cluster" from "one extent per physically contiguous run". */
static const uint32_t mixed_chain[6] = { 10u, 11u, 12u, 40u, 41u, 7u };
#define MIXED_NCL 6u

/* Four clusters that are already neighbours, reached through the FAT rather
 * than through NoFatChain: must still collapse to a single extent. */
static const uint32_t adj_chain[4] = { 20u, 21u, 22u, 23u };
#define ADJ_NCL 4u

#define EXT_MAX 16u
#define BUF_MAX 16384u

/* Distinctive sentinel: a slot the driver must not touch is compared against
 * this, so "left alone" is asserted rather than assumed. */
#define SENTINEL 0xDEADBEEFCAFEF00Dull

/* --------------------------------------------------------------- helpers */

static void mount_vol(synth_vol *sv, exfat_vol *v)
{
    synth_init(sv, EX_BPS, EX_SPC, EX_CLUSTERS);
    assert_int_equal(exfat_mount(v, synth_read, sv), EXFAT_OK);
}

static void set_ent(exfat_dirent *e, uint32_t first, uint64_t size,
                    uint8_t no_fat_chain)
{
    memset(e, 0, sizeof(*e));
    e->is_dir = 0;
    e->no_fat_chain = no_fat_chain;
    e->first_cluster = first;
    e->size = size;
    e->valid_len = size;
}

/* Mirrors synth_cluster's pointer maths in volume-offset terms, so an
 * expected extent is built from the synthetic layout rather than from
 * whatever the driver happens to return. */
static uint64_t cluster_byte_off(const synth_vol *sv, uint32_t cl)
{
    return ((uint64_t)sv->heap_offset << sv->bps_shift) +
           ((uint64_t)(cl - 2u) << (sv->bps_shift + sv->spc_shift));
}

/* Volume offset of the FAT sector holding cluster cl's link. */
static uint64_t fat_sector_off_for(const synth_vol *sv, uint32_t cl)
{
    uint64_t entry = ((uint64_t)sv->fat_offset << sv->bps_shift) +
                     (uint64_t)cl * 4u;
    uint64_t ss = (uint64_t)1 << sv->bps_shift;

    return entry & ~(ss - 1u);
}

static void fill_sentinel(exfat_extent *out, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        out[i].vol_off = SENTINEL;
        out[i].len = SENTINEL;
    }
}

static void assert_untouched(const exfat_extent *out, size_t from, size_t to)
{
    size_t i;

    for (i = from; i < to; i++) {
        assert_true(out[i].vol_off == SENTINEL);
        assert_true(out[i].len == SENTINEL);
    }
}

/* The full contract for one file in one place: the sizing call reports the
 * true count and refuses to write, an exactly-sized call succeeds and matches
 * the expected list offset for offset and length for length, an oversized
 * array is filled and nothing beyond it is touched, and the lengths sum to
 * the file's valid data length. */
static void check_extents(exfat_vol *v, const exfat_dirent *e,
                          const exfat_extent *want, size_t n)
{
    exfat_extent got[EXT_MAX];
    uint64_t total = 0;
    size_t count = 0;
    size_t i;

    assert_true(n + 2u <= EXT_MAX);

    /* Sizing call: no array at all. */
    count = 0;
    if (n == 0) {
        assert_int_equal(exfat_file_extents(v, e, NULL, 0, &count), EXFAT_OK);
    } else {
        assert_int_equal(exfat_file_extents(v, e, NULL, 0, &count),
                         EXFAT_ERR_NOSPACE);
    }
    assert_int_equal(count, n);

    /* Exactly-sized array. */
    fill_sentinel(got, EXT_MAX);
    count = 0;
    assert_int_equal(exfat_file_extents(v, e, got, n, &count), EXFAT_OK);
    assert_int_equal(count, n);
    for (i = 0; i < n; i++) {
        assert_true(got[i].vol_off == want[i].vol_off);
        assert_true(got[i].len == want[i].len);
        total += got[i].len;
    }
    assert_untouched(got, n, EXT_MAX);
    assert_true(total == e->size);

    /* Oversized array: same answer, nothing written past the count. */
    fill_sentinel(got, EXT_MAX);
    count = 0;
    assert_int_equal(exfat_file_extents(v, e, got, n + 2u, &count), EXFAT_OK);
    assert_int_equal(count, n);
    for (i = 0; i < n; i++) {
        assert_true(got[i].vol_off == want[i].vol_off);
        assert_true(got[i].len == want[i].len);
    }
    assert_untouched(got, n, EXT_MAX);
}

/* Reassembles the file from the extent list by reading the named volume
 * ranges through the same callback the driver uses, then compares that
 * against exfat_file_read. This is the test that would catch an extent map
 * that is internally consistent but points somewhere else: the two paths
 * share no code below the dirent. */
static void check_extent_bytes(synth_vol *sv, exfat_vol *v,
                               const exfat_dirent *e)
{
    exfat_extent ex[EXT_MAX];
    uint8_t from_extents[BUF_MAX];
    uint8_t from_read[BUF_MAX];
    exfat_file f;
    size_t count = 0;
    size_t i;
    uint64_t pos = 0;

    assert_true(e->size < BUF_MAX);
    memset(from_extents, 0x5A, sizeof(from_extents));
    memset(from_read, 0xA5, sizeof(from_read));

    assert_int_equal(exfat_file_extents(v, e, ex, EXT_MAX, &count), EXFAT_OK);
    for (i = 0; i < count; i++) {
        assert_true(pos + ex[i].len <= e->size);
        assert_int_equal(synth_read(sv, ex[i].vol_off, ex[i].len,
                                    from_extents + pos), 0);
        pos += ex[i].len;
    }
    assert_true(pos == e->size);

    assert_int_equal(exfat_file_open(v, &f, e), EXFAT_OK);
    assert_int_equal(exfat_file_read(&f, 0, e->size, from_read), EXFAT_OK);
    assert_memory_equal(from_extents, from_read, (size_t)e->size);

    /* And both must equal the pattern the volume was written with, so a pair
     * of paths that agree on the wrong bytes still fails. */
    assert_int_equal(from_extents[e->size], 0x5A);
    for (i = 0; i < (size_t)e->size; i++)
        assert_int_equal(from_extents[i], synth_pattern_byte((uint64_t)i));
}

/* ------------------------------------------------------------- bad args */

static void test_extents_bad_arg(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent out[EXT_MAX];
    size_t count = 99;

    (void)state;
    mount_vol(&sv, &v);
    set_ent(&e, 2u, EX_CB, 1u);

    assert_int_equal(exfat_file_extents(NULL, &e, out, EXT_MAX, &count),
                     EXFAT_ERR_BAD_ARG);
    assert_int_equal(exfat_file_extents(&v, NULL, out, EXT_MAX, &count),
                     EXFAT_ERR_BAD_ARG);
    assert_int_equal(exfat_file_extents(&v, &e, out, EXT_MAX, NULL),
                     EXFAT_ERR_BAD_ARG);
    /* A NULL array with a nonzero capacity is a caller bug, not a sizing
     * call: the sizing idiom is cap 0. */
    assert_int_equal(exfat_file_extents(&v, &e, NULL, EXT_MAX, &count),
                     EXFAT_ERR_BAD_ARG);
    synth_free(&sv);
}

/* A directory has an allocation, so the call would "work"; it is refused
 * because an extent map of a directory is not a thing any caller wants and
 * accepting it would hide a mixed-up dirent. */
static void test_extents_rejects_directory(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent out[EXT_MAX];
    size_t count = 99;

    (void)state;
    mount_vol(&sv, &v);
    set_ent(&e, 2u, EX_CB, 1u);
    e.is_dir = 1;
    assert_int_equal(exfat_file_extents(&v, &e, out, EXT_MAX, &count),
                     EXFAT_ERR_BAD_ARG);
    synth_free(&sv);
}

/* ------------------------------------------------------------ zero size */

static void test_extents_zero_size(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent out[EXT_MAX];
    size_t count = 99;

    (void)state;
    mount_vol(&sv, &v);

    /* An empty file has no allocation at all: zero extents is the answer, and
     * it is not an error, so a caller can feed an empty file through the
     * injection path and get an empty mapping rather than a failure. */
    set_ent(&e, 0u, 0u, 0u);
    fill_sentinel(out, EXT_MAX);
    assert_int_equal(exfat_file_extents(&v, &e, out, EXT_MAX, &count),
                     EXFAT_OK);
    assert_int_equal(count, 0);
    assert_untouched(out, 0, EXT_MAX);

    /* Sizing form of the same call: zero needed fits in zero capacity, so
     * this is OK and not NOSPACE. */
    count = 99;
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count), EXFAT_OK);
    assert_int_equal(count, 0);

    /* Zero size with a cluster still allocated (a truncated file): the size
     * is what the extents cover, so still nothing. */
    set_ent(&e, 5u, 0u, 1u);
    count = 99;
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count), EXFAT_OK);
    assert_int_equal(count, 0);
    synth_free(&sv);
}

/* ----------------------------------------------------------- contiguous */

static void test_extents_contig_single(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent want[1];
    uint64_t size = 4ull * EX_CB;

    (void)state;
    mount_vol(&sv, &v);
    /* Poison the FAT for these clusters: a contiguous file must be resolved
     * without consulting the chain at all, so a driver that walked it would
     * hit a bad link instead of quietly returning the right answer. */
    synth_fat_set(&sv, 30u, 0xFFFFFFF7u);
    synth_fat_set(&sv, 31u, 0xFFFFFFF7u);
    synth_fat_set(&sv, 32u, 0xFFFFFFF7u);

    set_ent(&e, 30u, size, 1u);
    want[0].vol_off = cluster_byte_off(&sv, 30u);
    want[0].len = size;
    check_extents(&v, &e, want, 1);
    synth_free(&sv);
}

/* The rounding contract: the last extent stops at the valid data length, it
 * does not run to the end of the final cluster. */
static void test_extents_contig_tail_slack_excluded(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent want[1];
    uint64_t size = 3ull * EX_CB + 7u;

    (void)state;
    mount_vol(&sv, &v);
    set_ent(&e, 30u, size, 1u);
    want[0].vol_off = cluster_byte_off(&sv, 30u);
    want[0].len = size;
    check_extents(&v, &e, want, 1);

    /* Stated the other way round, because this is the assertion that would
     * fail if anyone "helpfully" rounded up to the allocation. */
    assert_true(want[0].len != 4ull * EX_CB);
    synth_free(&sv);
}

/* One byte still occupies one cluster, and the extent is one byte long. */
static void test_extents_contig_one_byte(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent want[1];

    (void)state;
    mount_vol(&sv, &v);
    set_ent(&e, 100u, 1u, 1u);
    want[0].vol_off = cluster_byte_off(&sv, 100u);
    want[0].len = 1u;
    check_extents(&v, &e, want, 1);
    synth_free(&sv);
}

static void test_extents_contig_bytes_match_read(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    static const uint32_t cls[4] = { 30u, 31u, 32u, 33u };
    uint64_t size = 3ull * EX_CB + 511u;

    (void)state;
    mount_vol(&sv, &v);
    synth_write_file(&sv, cls, 4u, size, 0);
    set_ent(&e, 30u, size, 1u);
    check_extent_bytes(&sv, &v, &e);
    synth_free(&sv);
}

/* ---------------------------------------------------------- FAT chained */

static void test_extents_chain_adjacent_coalesces(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent want[1];
    uint64_t size = (uint64_t)ADJ_NCL * EX_CB;

    (void)state;
    mount_vol(&sv, &v);
    synth_write_file(&sv, adj_chain, ADJ_NCL, size, 1);
    set_ent(&e, adj_chain[0], size, 0u);

    /* Four clusters, four FAT hops, one extent: the file is fragmented in the
     * FAT and contiguous on disk, and it is the disk that the mapping is
     * about. */
    want[0].vol_off = cluster_byte_off(&sv, adj_chain[0]);
    want[0].len = size;
    check_extents(&v, &e, want, 1);
    check_extent_bytes(&sv, &v, &e);
    synth_free(&sv);
}

static void test_extents_chain_fragmented(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent want[FRAG_NCL];
    uint64_t size = (uint64_t)FRAG_NCL * EX_CB;
    unsigned i;

    (void)state;
    mount_vol(&sv, &v);
    synth_write_file(&sv, frag_chain, FRAG_NCL, size, 1);
    set_ent(&e, frag_chain[0], size, 0u);

    /* Built from the synthetic layout, not from the driver: every offset is
     * the cluster the test itself placed the data in, in chain order. */
    for (i = 0; i < FRAG_NCL; i++) {
        want[i].vol_off = cluster_byte_off(&sv, frag_chain[i]);
        want[i].len = EX_CB;
    }
    check_extents(&v, &e, want, FRAG_NCL);
    check_extent_bytes(&sv, &v, &e);
    synth_free(&sv);
}

static void test_extents_chain_mixed_runs(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent want[3];
    uint64_t size = (uint64_t)MIXED_NCL * EX_CB;

    (void)state;
    mount_vol(&sv, &v);
    synth_write_file(&sv, mixed_chain, MIXED_NCL, size, 1);
    set_ent(&e, mixed_chain[0], size, 0u);

    /* 10,11,12 then 40,41 then 7: three runs, not six clusters. */
    want[0].vol_off = cluster_byte_off(&sv, 10u);
    want[0].len = 3ull * EX_CB;
    want[1].vol_off = cluster_byte_off(&sv, 40u);
    want[1].len = 2ull * EX_CB;
    want[2].vol_off = cluster_byte_off(&sv, 7u);
    want[2].len = EX_CB;
    check_extents(&v, &e, want, 3);
    check_extent_bytes(&sv, &v, &e);
    synth_free(&sv);
}

/* The ragged tail on a fragmented file: the final run is short, the earlier
 * runs are not, and the sum is still the size. */
static void test_extents_chain_ragged_tail(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent want[3];
    uint64_t size = 5ull * EX_CB + 100u;

    (void)state;
    mount_vol(&sv, &v);
    synth_write_file(&sv, mixed_chain, MIXED_NCL, size, 1);
    set_ent(&e, mixed_chain[0], size, 0u);

    want[0].vol_off = cluster_byte_off(&sv, 10u);
    want[0].len = 3ull * EX_CB;
    want[1].vol_off = cluster_byte_off(&sv, 40u);
    want[1].len = 2ull * EX_CB;
    want[2].vol_off = cluster_byte_off(&sv, 7u);
    want[2].len = 100u;
    check_extents(&v, &e, want, 3);
    check_extent_bytes(&sv, &v, &e);
    synth_free(&sv);
}

/* A chain whose tail lands one cluster before its predecessor: the run must
 * break, because "adjacent" means the next cluster, not any neighbour. */
static void test_extents_chain_backward_neighbour_breaks_run(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent want[2];
    static const uint32_t cls[2] = { 51u, 50u };
    uint64_t size = 2ull * EX_CB;

    (void)state;
    mount_vol(&sv, &v);
    synth_write_file(&sv, cls, 2u, size, 1);
    set_ent(&e, cls[0], size, 0u);

    want[0].vol_off = cluster_byte_off(&sv, 51u);
    want[0].len = EX_CB;
    want[1].vol_off = cluster_byte_off(&sv, 50u);
    want[1].len = EX_CB;
    check_extents(&v, &e, want, 2);
    check_extent_bytes(&sv, &v, &e);
    synth_free(&sv);
}

/* A single-cluster chained file never enters the merge branch at all. */
static void test_extents_chain_single_cluster(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent want[1];

    (void)state;
    mount_vol(&sv, &v);
    set_ent(&e, 77u, 200u, 0u);
    want[0].vol_off = cluster_byte_off(&sv, 77u);
    want[0].len = 200u;
    check_extents(&v, &e, want, 1);
    synth_free(&sv);
}

/* ------------------------------------------------------- capacity rules */

static void test_extents_sizing_call(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent out[EXT_MAX];
    uint64_t size = (uint64_t)FRAG_NCL * EX_CB;
    size_t count = 99;

    (void)state;
    mount_vol(&sv, &v);
    synth_write_file(&sv, frag_chain, FRAG_NCL, size, 1);
    set_ent(&e, frag_chain[0], size, 0u);

    /* The documented two-call idiom: NULL/0 to learn the size. */
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_NOSPACE);
    assert_int_equal(count, FRAG_NCL);

    /* A real array of capacity 0 must behave identically to NULL/0. */
    fill_sentinel(out, EXT_MAX);
    count = 99;
    assert_int_equal(exfat_file_extents(&v, &e, out, 0, &count),
                     EXFAT_ERR_NOSPACE);
    assert_int_equal(count, FRAG_NCL);
    assert_untouched(out, 0, EXT_MAX);

    /* A contiguous file needs one, so cap 0 is still NOSPACE. */
    set_ent(&e, 30u, 4ull * EX_CB, 1u);
    count = 99;
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_NOSPACE);
    assert_int_equal(count, 1);
    synth_free(&sv);
}

static void test_extents_cap_too_small(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent out[EXT_MAX];
    uint64_t size = (uint64_t)FRAG_NCL * EX_CB;
    size_t cap;

    (void)state;
    mount_vol(&sv, &v);
    synth_write_file(&sv, frag_chain, FRAG_NCL, size, 1);
    set_ent(&e, frag_chain[0], size, 0u);

    /* Every short capacity: the prefix written is correct, the count is the
     * true one, and not one slot past cap is disturbed. */
    for (cap = 1; cap < FRAG_NCL; cap++) {
        size_t count = 99;
        size_t i;

        fill_sentinel(out, EXT_MAX);
        assert_int_equal(exfat_file_extents(&v, &e, out, cap, &count),
                         EXFAT_ERR_NOSPACE);
        assert_int_equal(count, FRAG_NCL);
        for (i = 0; i < cap; i++) {
            assert_true(out[i].vol_off == cluster_byte_off(&sv,
                                                           frag_chain[i]));
            assert_true(out[i].len == EX_CB);
        }
        assert_untouched(out, cap, EXT_MAX);
    }

    /* Exactly enough is the boundary: OK, not NOSPACE. */
    {
        size_t count = 99;

        fill_sentinel(out, EXT_MAX);
        assert_int_equal(exfat_file_extents(&v, &e, out, FRAG_NCL, &count),
                         EXFAT_OK);
        assert_int_equal(count, FRAG_NCL);
        assert_untouched(out, FRAG_NCL, EXT_MAX);
    }
    synth_free(&sv);
}

/* ------------------------------------------------------------ corruption */

static void test_extents_first_cluster_invalid(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    size_t count = 99;

    (void)state;
    mount_vol(&sv, &v);

    /* Size claims data, first cluster says there is none. */
    set_ent(&e, 0u, EX_CB, 0u);
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_CORRUPT);

    /* Clusters 0 and 1 are reserved, never data. */
    set_ent(&e, 1u, EX_CB, 0u);
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_CORRUPT);

    /* Past the end of the heap. */
    set_ent(&e, EX_CLUSTERS + 2u, EX_CB, 0u);
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_CORRUPT);
    synth_free(&sv);
}

static void test_extents_chain_bad_link(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    static const uint32_t cls[3] = { 10u, 11u, 12u };
    uint64_t size = 3ull * EX_CB;
    size_t count = 99;

    (void)state;
    mount_vol(&sv, &v);
    synth_write_file(&sv, cls, 3u, size, 1);
    /* 0xFFFFFFF7 is the spec's BAD marker: a file must never be mapped
     * through it, however plausible the rest of the chain looks. */
    synth_fat_set(&sv, 11u, 0xFFFFFFF7u);
    set_ent(&e, cls[0], size, 0u);
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_CORRUPT);
    synth_free(&sv);
}

static void test_extents_chain_out_of_range_link(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    static const uint32_t cls[3] = { 10u, 11u, 12u };
    uint64_t size = 3ull * EX_CB;
    size_t count = 99;

    (void)state;
    mount_vol(&sv, &v);
    synth_write_file(&sv, cls, 3u, size, 1);

    /* Beyond the heap. */
    synth_fat_set(&sv, 11u, EX_CLUSTERS + 100u);
    set_ent(&e, cls[0], size, 0u);
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_CORRUPT);

    /* Into the reserved entries. */
    synth_fat_set(&sv, 11u, 1u);
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_CORRUPT);

    /* A link to cluster 0 is the spec's "free" marker, which mid-chain means
     * the chain is shorter than the size claims. */
    synth_fat_set(&sv, 11u, 0u);
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_CORRUPT);
    synth_free(&sv);
}

static void test_extents_chain_short(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    static const uint32_t cls[3] = { 10u, 11u, 12u };
    size_t count = 99;

    (void)state;
    mount_vol(&sv, &v);
    /* Three clusters chained and end-of-chain marked, but the dirent claims
     * five clusters' worth of data. Handing back a four-fifths map would be
     * worse than an error: the consumer would build a device-mapper table
     * that silently reads someone else's data. */
    synth_write_file(&sv, cls, 3u, 3ull * EX_CB, 1);
    set_ent(&e, cls[0], 5ull * EX_CB, 0u);
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_CORRUPT);

    /* One byte past the chain is enough to trip it. */
    set_ent(&e, cls[0], 3ull * EX_CB + 1u, 0u);
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_CORRUPT);

    /* Exactly the chain's worth is fine, which is what makes the two above
     * assertions about the size rather than about the chain. */
    set_ent(&e, cls[0], 3ull * EX_CB, 0u);
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_NOSPACE);
    assert_int_equal(count, 1);
    synth_free(&sv);
}

/* A NoFatChain file whose contiguous span leaves the cluster heap. There is
 * no chain to walk, so the far end is the only thing that can be checked and
 * it must be. */
static void test_extents_nofat_past_heap(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    size_t count = 99;

    (void)state;
    mount_vol(&sv, &v);

    /* Starts inside the heap, ends past its last cluster. */
    set_ent(&e, EX_CLUSTERS - 4u, 10ull * EX_CB, 1u);
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_CORRUPT);

    /* One cluster too far. */
    set_ent(&e, EX_CLUSTERS + 1u, 2ull * EX_CB, 1u);
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_CORRUPT);

    /* Ending on the very last cluster of the heap (cluster_count + 1) is
     * legal, which is what makes the above a boundary test rather than an
     * off-by-one. */
    set_ent(&e, EX_CLUSTERS, 2ull * EX_CB, 1u);
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_NOSPACE);
    assert_int_equal(count, 1);
    synth_free(&sv);
}

/* IO failure while walking the chain must surface as EXFAT_ERR_IO and not as
 * a truncated map. */
static void test_extents_chain_io_error(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent out[EXT_MAX];
    uint64_t size = (uint64_t)FRAG_NCL * EX_CB;
    size_t count = 99;

    (void)state;
    mount_vol(&sv, &v);
    synth_write_file(&sv, frag_chain, FRAG_NCL, size, 1);
    set_ent(&e, frag_chain[0], size, 0u);

    /* The FAT sector holding cluster 520's link, which is the second hop and
     * a different sector from the first, so the failure lands mid-walk. */
    sv.fail_off = (int64_t)fat_sector_off_for(&sv, 520u);
    fill_sentinel(out, EXT_MAX);
    assert_int_equal(exfat_file_extents(&v, &e, out, EXT_MAX, &count),
                     EXFAT_ERR_IO);
    /* Partial results are not a mapping. The count is left as the caller had
     * it rather than reporting a prefix that would look authoritative. */
    assert_int_equal(count, 0);
    synth_free(&sv);
}

/* DEFECT, pinned rather than endorsed (reported, not fixed here: this suite
 * does not edit src/).
 *
 * The NoFatChain path computes its cluster count as (size + cb - 1) / cb.
 * With a ValidDataLength near UINT64_MAX that addition wraps, the count comes
 * out as 0, the "last cluster" lands *before* the first one, the heap check
 * passes, and the call returns EXFAT_OK with a single extent whose length is
 * the bogus size: a mapping that runs off the end of the volume. The size
 * comes straight off the disk, so a corrupt or hostile volume can set it.
 *
 * exfat_file_read is not affected: it validates the far end per request and
 * refuses. Only the extent map hands the value on.
 *
 * The assertions below record what the code does TODAY so that a fix trips
 * this test and it gets rewritten to assert EXFAT_ERR_CORRUPT. */
static void test_extents_nofat_size_overflow_rejected(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_dirent e;
    exfat_extent out[EXT_MAX];
    size_t count = 99;

    (void)state;
    mount_vol(&sv, &v);

    /* A hostile ValidDataLength near 2^64 once wrapped the round-up in the
     * NoFatChain path (clusters == 0, bogus in-range last cluster) and was
     * handed on as an extent longer than the volume. The division-first
     * round-up rejects it as corruption regardless of first_cluster. */
    set_ent(&e, 3u, ~(uint64_t)0, 1u);
    fill_sentinel(out, EXT_MAX);
    assert_int_equal(exfat_file_extents(&v, &e, out, EXT_MAX, &count),
                     EXFAT_ERR_CORRUPT);
    assert_true(out[0].vol_off == SENTINEL && out[0].len == SENTINEL);

    set_ent(&e, 2u, ~(uint64_t)0, 1u);
    count = 99;
    assert_int_equal(exfat_file_extents(&v, &e, NULL, 0, &count),
                     EXFAT_ERR_CORRUPT);

    /* Largest size that must still be accepted: the file exactly filling
     * the heap from its first cluster. */
    set_ent(&e, 2u, (uint64_t)sv.cluster_count * synth_cluster_bytes(&sv),
            1u);
    count = 99;
    assert_int_equal(exfat_file_extents(&v, &e, out, EXT_MAX, &count),
                     EXFAT_OK);
    assert_int_equal(count, 1);
    synth_free(&sv);
}

/* ------------------------------------------------------------------ run */

int run_extent_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_extents_bad_arg),
        cmocka_unit_test(test_extents_rejects_directory),
        cmocka_unit_test(test_extents_zero_size),
        cmocka_unit_test(test_extents_contig_single),
        cmocka_unit_test(test_extents_contig_tail_slack_excluded),
        cmocka_unit_test(test_extents_contig_one_byte),
        cmocka_unit_test(test_extents_contig_bytes_match_read),
        cmocka_unit_test(test_extents_chain_adjacent_coalesces),
        cmocka_unit_test(test_extents_chain_fragmented),
        cmocka_unit_test(test_extents_chain_mixed_runs),
        cmocka_unit_test(test_extents_chain_ragged_tail),
        cmocka_unit_test(test_extents_chain_backward_neighbour_breaks_run),
        cmocka_unit_test(test_extents_chain_single_cluster),
        cmocka_unit_test(test_extents_sizing_call),
        cmocka_unit_test(test_extents_cap_too_small),
        cmocka_unit_test(test_extents_first_cluster_invalid),
        cmocka_unit_test(test_extents_chain_bad_link),
        cmocka_unit_test(test_extents_chain_out_of_range_link),
        cmocka_unit_test(test_extents_chain_short),
        cmocka_unit_test(test_extents_nofat_past_heap),
        cmocka_unit_test(test_extents_chain_io_error),
        cmocka_unit_test(test_extents_nofat_size_overflow_rejected),
    };

    return cmocka_run_group_tests_name("exfat extents", tests, NULL, NULL);
}
