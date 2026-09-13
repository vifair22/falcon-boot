/* Synthetic-volume tests for the file-read half of src/exfat/exfat.c:
 * exfat_file_open, exfat_file_read and the chain cursor behind them.
 *
 * Same approach as exfat_synth_test.c: the volume is built byte by byte in
 * memory, because a short FAT chain, a BAD link in the middle of a file, or a
 * contiguous allocation that runs off the end of the cluster heap are all
 * things mkfs.exfat will never write. What is new here is content: every file
 * is filled with synth_pattern_byte(logical offset), so a read that returns
 * the right number of bytes from the wrong place fails the compare instead of
 * passing on a buffer of zeroes.
 *
 * Two geometries are used.
 *
 *   contiguous volume  512-byte sectors, 2048-byte clusters (4 sectors), 32
 *                      clusters. Cluster size above sector size is what makes
 *                      the aligned/unaligned span split visible.
 *
 *   chained volume     512-byte sectors, 1024-byte clusters, 1024 clusters,
 *                      so the FAT is 9 sectors long. That matters: the driver
 *                      caches one FAT sector on the volume, so a chain whose
 *                      links sit in different FAT sectors is the only way to
 *                      make each hop cost a device read and therefore the only
 *                      way to count walks. CHAIN_CLUSTERS below is chosen for
 *                      exactly that, and is deliberately not ascending so a
 *                      driver that assumed contiguity would return wrong
 *                      bytes rather than merely wrong counts.
 */

#include "synth.h"
#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include <exfat/exfat.h>

#define CONTIG_BPS      9u
#define CONTIG_SPC      2u     /* 2048-byte clusters */
#define CONTIG_CLUSTERS 32u
#define CONTIG_FIRST    4u     /* clusters 4..7 hold the file */
#define CONTIG_NCL      4u

#define CHAIN_BPS       9u
#define CHAIN_SPC       1u     /* 1024-byte clusters */
#define CHAIN_CLUSTERS  1024u

#define SECTOR 512u

/* One cluster per FAT sector boundary: entries for 2, 520, 130, 390 and 260
 * live in FAT sectors 0, 4, 1, 3 and 2, so every hop of this chain misses the
 * driver's one-sector FAT cache and shows up in the read count. */
static const uint32_t chain_clusters[5] = { 2u, 520u, 130u, 390u, 260u };
#define CHAIN_NCL 5u

/* Largest single read any test issues. */
#define BUF_MAX 16384u

/* ------------------------------------------------------------- helpers */

static void mount_vol(synth_vol *sv, exfat_vol *v, unsigned bps, unsigned spc,
                      uint32_t clusters)
{
    synth_init(sv, bps, spc, clusters);
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
}

/* Volume byte offset of a cluster, mirroring synth_cluster's pointer maths so
 * a test can aim synth_vol::fail_off at one specific device read. */
static uint64_t cluster_byte_off(const synth_vol *sv, uint32_t cl)
{
    return ((uint64_t)sv->heap_offset << sv->bps_shift) +
           ((uint64_t)(cl - 2u) << (sv->bps_shift + sv->spc_shift));
}

static uint64_t fat_sector_off(const synth_vol *sv, unsigned sector)
{
    return ((uint64_t)sv->fat_offset << sv->bps_shift) +
           ((uint64_t)sector << sv->bps_shift);
}

/* Reads [off, off+len) and asserts every byte is the one the pattern puts at
 * that logical file offset. */
static void check_read(exfat_file *f, uint64_t off, uint64_t len)
{
    uint8_t got[BUF_MAX];
    uint8_t want[BUF_MAX];

    assert_true(len <= BUF_MAX);
    memset(got, 0xA5, sizeof(got));
    assert_int_equal(exfat_file_read(f, off, len, got), EXFAT_OK);
    synth_pattern_fill(want, off, len);
    assert_memory_equal(got, want, (size_t)len);
    /* The driver must not have written past the requested length. */
    if (len < BUF_MAX)
        assert_int_equal(got[len], 0xA5);
}

/* A contiguous file: clusters 4..7, 8192 bytes, FAT entries poisoned so any
 * consultation of the FAT is a failure rather than a silent success. */
static uint64_t open_contig(synth_vol *sv, exfat_vol *v, exfat_file *f)
{
    static const uint32_t cls[CONTIG_NCL] = { 4u, 5u, 6u, 7u };
    uint64_t size = (uint64_t)CONTIG_NCL * (1u << (CONTIG_BPS + CONTIG_SPC));
    exfat_dirent e;
    unsigned i;

    mount_vol(sv, v, CONTIG_BPS, CONTIG_SPC, CONTIG_CLUSTERS);
    synth_write_file(sv, cls, CONTIG_NCL, size, 0);
    for (i = 0; i < CONTIG_NCL; i++)
        synth_fat_set(sv, cls[i], 0xFFFFFFF7u);

    set_ent(&e, CONTIG_FIRST, size, 1);
    assert_int_equal(exfat_file_open(v, f, &e), EXFAT_OK);
    return size;
}

/* A FAT-chained file over chain_clusters, size bytes long (which may leave the
 * last cluster ragged). */
static void open_chain(synth_vol *sv, exfat_vol *v, exfat_file *f,
                       unsigned ncl, uint64_t size)
{
    exfat_dirent e;

    mount_vol(sv, v, CHAIN_BPS, CHAIN_SPC, CHAIN_CLUSTERS);
    synth_write_file(sv, chain_clusters, ncl, size, 1);
    set_ent(&e, chain_clusters[0], size, 0);
    assert_int_equal(exfat_file_open(v, f, &e), EXFAT_OK);
}

/* ----------------------------------------------------------- file open */

static void test_file_open_state(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    exfat_dirent e;

    (void)state;
    mount_vol(&sv, &v, CONTIG_BPS, CONTIG_SPC, CONTIG_CLUSTERS);
    set_ent(&e, 5, 1234, 1);
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);
    assert_ptr_equal(f.vol, &v);
    assert_int_equal(f.first_cluster, 5);
    assert_int_equal(f.no_fat_chain, 1);
    assert_true(f.size == 1234);
    /* The cursor starts on the first cluster, and the sector cache starts
     * invalid or the first read would serve a stale sector. */
    assert_int_equal(f.cur_cluster, 5);
    assert_true(f.cur_index == 0);
    assert_true(f.sec_buf_off == ~(uint64_t)0);
    /* Opening reads nothing: the dirent already carries everything needed. */
    assert_int_equal(sv.reads, 1); /* the mount's VBR read, and no more */
    synth_free(&sv);
}

static void test_file_open_bad_arg(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    exfat_dirent e;

    (void)state;
    mount_vol(&sv, &v, CONTIG_BPS, CONTIG_SPC, CONTIG_CLUSTERS);
    set_ent(&e, 4, 2048, 1);
    assert_int_equal(exfat_file_open(NULL, &f, &e), EXFAT_ERR_BAD_ARG);
    assert_int_equal(exfat_file_open(&v, NULL, &e), EXFAT_ERR_BAD_ARG);
    assert_int_equal(exfat_file_open(&v, &f, NULL), EXFAT_ERR_BAD_ARG);
    synth_free(&sv);
}

/* A directory is not a file, whatever the caller believes: the mirror image of
 * test_dir_open_bad_arg's file-as-directory rejection. */
static void test_file_open_rejects_directory(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    exfat_dirent e;

    (void)state;
    mount_vol(&sv, &v, CONTIG_BPS, CONTIG_SPC, CONTIG_CLUSTERS);
    set_ent(&e, 4, 2048, 1);
    e.is_dir = 1;
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_ERR_BAD_ARG);
    e.is_dir = 0;
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);
    synth_free(&sv);
}

static void test_file_open_cluster_out_of_range(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    exfat_dirent e;

    (void)state;
    mount_vol(&sv, &v, CONTIG_BPS, CONTIG_SPC, CONTIG_CLUSTERS);
    set_ent(&e, 0, 2048, 0); /* nonzero size with no allocation */
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_ERR_CORRUPT);
    e.first_cluster = 1; /* reserved */
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_ERR_CORRUPT);
    e.first_cluster = CONTIG_CLUSTERS + 2u; /* one past the heap */
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_ERR_CORRUPT);
    e.first_cluster = CONTIG_CLUSTERS + 1u; /* the last real cluster */
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);
    synth_free(&sv);
}

/* A zero-length file is legal and normally carries no allocation at all, so
 * the cluster check is skipped for it. Nothing can be read from it, but the
 * zero-length read still succeeds (see test_file_zero_length_read). */
static void test_file_open_zero_size(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    exfat_dirent e;
    uint8_t buf[16];
    unsigned reads_before;

    (void)state;
    mount_vol(&sv, &v, CONTIG_BPS, CONTIG_SPC, CONTIG_CLUSTERS);
    set_ent(&e, 0, 0, 0);
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);
    assert_int_equal(f.first_cluster, 0);
    assert_true(f.size == 0);

    reads_before = sv.reads;
    assert_int_equal(exfat_file_read(&f, 0, 0, buf), EXFAT_OK);
    /* Every nonzero length is outside the file, so it is a range error and
     * never reaches the unallocated-file check. */
    assert_int_equal(exfat_file_read(&f, 0, 1, buf), EXFAT_ERR_RANGE);
    assert_int_equal(exfat_file_read(&f, 1, 0, buf), EXFAT_ERR_RANGE);
    assert_int_equal(sv.reads, reads_before);

    /* An out-of-range first cluster is not even looked at while the size is
     * zero; the entry describes no data. */
    set_ent(&e, CONTIG_CLUSTERS + 99u, 0, 0);
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);
    synth_free(&sv);
}

/* ------------------------------------------------- argument and range */

static void test_file_read_bad_arg(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    exfat_file zero;
    uint8_t buf[16];

    (void)state;
    (void)open_contig(&sv, &v, &f);
    memset(&zero, 0, sizeof(zero));

    assert_int_equal(exfat_file_read(NULL, 0, 1, buf), EXFAT_ERR_BAD_ARG);
    /* A zeroed exfat_file has no volume: caught before anything is read. */
    assert_int_equal(exfat_file_read(&zero, 0, 1, buf), EXFAT_ERR_BAD_ARG);
    /* A null destination is only an error when bytes were asked for. */
    assert_int_equal(exfat_file_read(&f, 0, 1, NULL), EXFAT_ERR_BAD_ARG);
    assert_int_equal(exfat_file_read(&f, 0, 0, NULL), EXFAT_OK);
    synth_free(&sv);
}

/* Wholly inside the file or nothing: the driver never reports a short read as
 * success, so every request that would cross the end is refused outright. */
static void test_file_read_range(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint8_t buf[BUF_MAX];
    uint64_t size;
    unsigned reads_before;

    (void)state;
    size = open_contig(&sv, &v, &f);
    reads_before = sv.reads;

    assert_int_equal(exfat_file_read(&f, size + 1u, 1, buf),
                     EXFAT_ERR_RANGE);
    assert_int_equal(exfat_file_read(&f, size, 1, buf), EXFAT_ERR_RANGE);
    assert_int_equal(exfat_file_read(&f, 0, size + 1u, buf),
                     EXFAT_ERR_RANGE);
    assert_int_equal(exfat_file_read(&f, size - 1u, 2, buf),
                     EXFAT_ERR_RANGE);
    /* off + len overflowing 64 bits must not wrap into an accepted range. */
    assert_int_equal(exfat_file_read(&f, 1, ~(uint64_t)0, buf),
                     EXFAT_ERR_RANGE);
    assert_int_equal(exfat_file_read(&f, ~(uint64_t)0, 1, buf),
                     EXFAT_ERR_RANGE);
    assert_int_equal(exfat_file_read(&f, ~(uint64_t)0, ~(uint64_t)0, buf),
                     EXFAT_ERR_RANGE);
    /* A rejected request touches the device not at all. */
    assert_int_equal(sv.reads, reads_before);

    /* The exact boundary is inside: the last byte, and the whole file. */
    check_read(&f, size - 1u, 1);
    check_read(&f, 0, size);
    synth_free(&sv);
}

/* Pinning the zero-length contract, which the header does not spell out: a
 * zero-length read succeeds anywhere up to and including the end of the file,
 * touches the device not at all, and is a range error only past the end. It
 * succeeds even on a file with no allocation, because the length check comes
 * before the allocation check. */
static void test_file_zero_length_read(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint8_t buf[16];
    uint64_t size;
    unsigned reads_before;

    (void)state;
    size = open_contig(&sv, &v, &f);
    reads_before = sv.reads;

    assert_int_equal(exfat_file_read(&f, 0, 0, buf), EXFAT_OK);
    assert_int_equal(exfat_file_read(&f, size / 2u, 0, buf), EXFAT_OK);
    assert_int_equal(exfat_file_read(&f, size, 0, buf), EXFAT_OK);
    assert_int_equal(exfat_file_read(&f, size + 1u, 0, buf),
                     EXFAT_ERR_RANGE);
    assert_int_equal(sv.reads, reads_before);
    /* The cursor is untouched, so a zero-length read is never a seek. */
    assert_true(f.cur_index == 0);
    assert_int_equal(f.cur_cluster, CONTIG_FIRST);
    synth_free(&sv);
}

/* exfat_file_read guards against a file that claims bytes but owns no
 * clusters. exfat_file_open already rejects that dirent, so the guard is
 * unreachable through the public path and needs a hand-built descriptor; the
 * test exists so the guard is known to work if a future caller (the extent
 * map, say) ever builds an exfat_file itself. */
static void test_file_read_unallocated_is_corrupt(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint8_t buf[16];

    (void)state;
    mount_vol(&sv, &v, CONTIG_BPS, CONTIG_SPC, CONTIG_CLUSTERS);
    memset(&f, 0, sizeof(f));
    f.vol = &v;
    f.size = 4096;
    f.first_cluster = 0;
    f.sec_buf_off = ~(uint64_t)0;
    assert_int_equal(exfat_file_read(&f, 0, 16, buf), EXFAT_ERR_CORRUPT);
    /* Zero length still short-circuits to success ahead of that check. */
    assert_int_equal(exfat_file_read(&f, 0, 0, buf), EXFAT_OK);
    synth_free(&sv);
}

/* --------------------------------------------------- contiguous reads */

/* A contiguous file is one span however many clusters it crosses, so the whole
 * file is a single device request. That is what makes the virtual CD's
 * sequential ISO reads cheap, so it is asserted rather than assumed. */
static void test_file_contig_whole_file(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint64_t size;
    unsigned reads_before;

    (void)state;
    size = open_contig(&sv, &v, &f);
    reads_before = sv.reads;
    check_read(&f, 0, size);
    assert_int_equal(sv.reads - reads_before, 1);
    synth_free(&sv);
}

static void test_file_contig_aligned(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint64_t size;

    (void)state;
    size = open_contig(&sv, &v, &f);
    check_read(&f, 0, SECTOR);            /* first sector */
    check_read(&f, SECTOR, SECTOR);       /* second sector */
    check_read(&f, 2048, 2048);           /* exactly the second cluster */
    check_read(&f, 1536, 1024);           /* aligned, across a cluster edge */
    check_read(&f, size - SECTOR, SECTOR);/* the last sector */
    synth_free(&sv);
}

static void test_file_contig_unaligned_start(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    unsigned reads_before;

    (void)state;
    (void)open_contig(&sv, &v, &f);
    reads_before = sv.reads;
    check_read(&f, 100, 2000);
    /* Three device reads: the bounce-buffered head sector, one direct
     * multi-sector transfer, and the bounce-buffered tail sector. */
    assert_int_equal(sv.reads - reads_before, 3);
    check_read(&f, 1, 3);
    check_read(&f, SECTOR - 1u, 2);
    synth_free(&sv);
}

static void test_file_contig_unaligned_end(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    unsigned reads_before;

    (void)state;
    (void)open_contig(&sv, &v, &f);
    reads_before = sv.reads;
    check_read(&f, 0, 1000);
    /* One direct sector plus the bounce-buffered remainder. */
    assert_int_equal(sv.reads - reads_before, 2);
    check_read(&f, 0, SECTOR + 1u);
    check_read(&f, 2048, 2049);
    synth_free(&sv);
}

static void test_file_contig_tiny_mid_sector(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    unsigned reads_before;

    (void)state;
    (void)open_contig(&sv, &v, &f);
    reads_before = sv.reads;
    check_read(&f, 1000, 5);
    assert_int_equal(sv.reads - reads_before, 1);
    check_read(&f, 4095, 1);
    check_read(&f, 4096, 1);
    synth_free(&sv);
}

/* The per-file sector cache must serve a second read from the same sector
 * without going back to the device. */
static void test_file_sector_cache_reused(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    unsigned reads_before;

    (void)state;
    (void)open_contig(&sv, &v, &f);
    check_read(&f, 600, 8);
    assert_true(f.sec_buf_off == cluster_byte_off(&sv, CONTIG_FIRST) + SECTOR);
    reads_before = sv.reads;
    check_read(&f, 700, 8);
    check_read(&f, 1000, 8);
    assert_int_equal(sv.reads - reads_before, 0);
    /* A different sector does cost a read. */
    check_read(&f, 1100, 8);
    assert_int_equal(sv.reads - reads_before, 1);
    synth_free(&sv);
}

/* Contiguity means the FAT is never consulted, which open_contig's poisoned
 * entries would turn into EXFAT_ERR_CORRUPT if it were. Reading the last
 * cluster of the file proves the whole walk stayed off the FAT. */
static void test_file_contig_ignores_fat(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint64_t size;

    (void)state;
    size = open_contig(&sv, &v, &f);
    check_read(&f, size - 2048u, 2048);
    assert_true(v.fat_cache_off == ~(uint64_t)0); /* never populated */
    synth_free(&sv);
}

/* A contiguous allocation whose far end falls outside the cluster heap. The
 * driver validates the first and last cluster of each request, so a read that
 * reaches past the heap is corrupt... */
static void test_file_contig_past_heap_is_corrupt(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    exfat_dirent e;
    uint8_t buf[BUF_MAX];

    (void)state;
    mount_vol(&sv, &v, CONTIG_BPS, CONTIG_SPC, CONTIG_CLUSTERS);
    /* Last valid cluster is 33; this claims 33, 34 and 35. */
    set_ent(&e, CONTIG_CLUSTERS + 1u, 3u * 2048u, 1);
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);
    assert_int_equal(exfat_file_read(&f, 0, 3u * 2048u, buf),
                     EXFAT_ERR_CORRUPT);
    assert_int_equal(exfat_file_read(&f, 2048, 1, buf), EXFAT_ERR_CORRUPT);
    assert_int_equal(exfat_file_read(&f, 2047, 2, buf), EXFAT_ERR_CORRUPT);
    synth_free(&sv);
}

/* ...but a request confined to the clusters that do exist still succeeds, even
 * though the file as a whole is malformed. Pinned because it is a deliberate
 * consequence of validating the request rather than the allocation: the loader
 * would rather serve the readable part of a damaged ISO than refuse it. */
static void test_file_contig_partial_overrun_still_reads(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    exfat_dirent e;
    uint8_t buf[2048];
    static const uint32_t cls[1] = { CONTIG_CLUSTERS + 1u };

    (void)state;
    mount_vol(&sv, &v, CONTIG_BPS, CONTIG_SPC, CONTIG_CLUSTERS);
    synth_write_file(&sv, cls, 1, 2048, 0);
    set_ent(&e, CONTIG_CLUSTERS + 1u, 3u * 2048u, 1);
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);
    assert_int_equal(exfat_file_read(&f, 0, 2048, buf), EXFAT_OK);
    assert_int_equal(memcmp(buf, synth_cluster(&sv, CONTIG_CLUSTERS + 1u),
                            2048), 0);
    synth_free(&sv);
}

/* first_cluster + index overflowing 32 bits. exfat_file_open cannot produce
 * such a descriptor (it rejects any first cluster outside the heap), so this
 * builds one directly to prove the overflow check itself holds. */
static void test_file_contig_cluster_index_overflow(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint8_t buf[16];

    (void)state;
    mount_vol(&sv, &v, CONTIG_BPS, CONTIG_SPC, CONTIG_CLUSTERS);
    memset(&f, 0, sizeof(f));
    f.vol = &v;
    f.no_fat_chain = 1;
    f.first_cluster = 0xFFFFFF00u;
    f.cur_cluster = 0xFFFFFF00u;
    f.size = (uint64_t)1 << 40;
    f.sec_buf_off = ~(uint64_t)0;
    /* Cluster index 0x100 puts the sum one past 0xFFFFFFFF. */
    assert_int_equal(exfat_file_read(&f, 0x100ull * 2048ull, 16, buf),
                     EXFAT_ERR_CORRUPT);
    synth_free(&sv);
}

/* -------------------------------------------------------- chain reads */

static void test_file_chain_whole_file(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint64_t size = (uint64_t)CHAIN_NCL * 1024u;

    (void)state;
    open_chain(&sv, &v, &f, CHAIN_NCL, size);
    check_read(&f, 0, size);
    /* The cursor ends on the last cluster of the chain. */
    assert_true(f.cur_index == CHAIN_NCL - 1u);
    assert_int_equal(f.cur_cluster, chain_clusters[CHAIN_NCL - 1u]);
    synth_free(&sv);
}

/* A single request crossing a cluster boundary has to follow the FAT, and the
 * clusters are not in ascending order, so a contiguity assumption would return
 * the wrong bytes rather than merely the wrong count of them. */
static void test_file_chain_spans_cluster_boundary(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;

    (void)state;
    open_chain(&sv, &v, &f, CHAIN_NCL, (uint64_t)CHAIN_NCL * 1024u);
    check_read(&f, 900, 300);            /* clusters 0 and 1 */
    check_read(&f, 1023, 2);             /* the boundary byte pair */
    check_read(&f, 512, 2048);           /* three clusters, aligned */
    check_read(&f, 100, 3000);           /* unaligned both ends */
    synth_free(&sv);
}

/* The chain cursor: a forward-moving read resumes where the last one stopped.
 * Counted rather than asserted on state alone, because the whole point is the
 * device reads a re-walk would cost. */
static void test_file_chain_forward_read_resumes(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    exfat_file fresh;
    exfat_dirent e;
    uint64_t size = (uint64_t)CHAIN_NCL * 1024u;
    unsigned resumed;
    unsigned from_scratch;
    unsigned mark;

    (void)state;
    open_chain(&sv, &v, &f, CHAIN_NCL, size);

    /* Cluster 0: no walk at all, one device read. */
    mark = sv.reads;
    check_read(&f, 0, 1024);
    assert_int_equal(sv.reads - mark, 1);
    assert_true(f.cur_index == 0);

    /* Jump to cluster 4: four hops, each in its own FAT sector, plus the
     * data read. */
    mark = sv.reads;
    check_read(&f, 4u * 1024u, 1024);
    assert_int_equal(sv.reads - mark, 5);
    assert_true(f.cur_index == 4);
    assert_int_equal(f.cur_cluster, chain_clusters[4]);

    /* Second half of the same cluster: the cursor is already there, so this
     * costs one data read and no FAT traffic. */
    mark = sv.reads;
    check_read(&f, 4u * 1024u + 512u, 512);
    resumed = sv.reads - mark;
    assert_int_equal(resumed, 1);
    assert_true(f.cur_index == 4);

    /* The control: the identical read on a freshly opened descriptor has to
     * walk the chain from cluster 0 again. */
    set_ent(&e, chain_clusters[0], size, 0);
    assert_int_equal(exfat_file_open(&v, &fresh, &e), EXFAT_OK);
    mark = sv.reads;
    check_read(&fresh, 4u * 1024u + 512u, 512);
    from_scratch = sv.reads - mark;
    assert_int_equal(from_scratch, 5);
    assert_int_equal(from_scratch - resumed, 4);
    synth_free(&sv);
}

/* A backward seek is the one case that restarts the walk from the first
 * cluster; after it, a forward read walks again from there. */
static void test_file_chain_backward_seek_restarts(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint64_t size = (uint64_t)CHAIN_NCL * 1024u;
    unsigned mark;

    (void)state;
    open_chain(&sv, &v, &f, CHAIN_NCL, size);

    check_read(&f, 4u * 1024u, 1024);
    assert_true(f.cur_index == 4);

    /* Back to the start: the cursor resets without consulting the FAT, since
     * the first cluster is known from the descriptor. */
    mark = sv.reads;
    check_read(&f, 0, 512);
    assert_int_equal(sv.reads - mark, 1);
    assert_true(f.cur_index == 0);
    assert_int_equal(f.cur_cluster, chain_clusters[0]);

    /* Forward again from the reset position: two hops this time, not four. */
    mark = sv.reads;
    check_read(&f, 2u * 1024u, 512);
    assert_int_equal(sv.reads - mark, 3);
    assert_true(f.cur_index == 2);
    assert_int_equal(f.cur_cluster, chain_clusters[2]);

    /* One cluster back is still a backward seek, so it restarts and re-walks
     * to index 1: two hops rather than the one a reverse step would cost. */
    mark = sv.reads;
    check_read(&f, 1024, 512);
    assert_int_equal(sv.reads - mark, 2);
    assert_true(f.cur_index == 1);
    synth_free(&sv);
}

/* A file whose size does not fill its last cluster. Reads have to stop at the
 * valid data length, not at the cluster edge. */
static void test_file_chain_ragged_final_cluster(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint64_t size = 2u * 1024u + 300u;
    uint8_t buf[BUF_MAX];

    (void)state;
    open_chain(&sv, &v, &f, 3u, size);
    check_read(&f, 0, size);            /* the whole ragged file */
    check_read(&f, 2048, 300);          /* exactly the ragged tail */
    check_read(&f, size - 1u, 1);       /* its last byte */
    check_read(&f, 2000, 348);          /* across the last boundary */
    /* One byte past the valid data, still inside the allocated cluster. */
    assert_int_equal(exfat_file_read(&f, size, 1, buf), EXFAT_ERR_RANGE);
    assert_int_equal(exfat_file_read(&f, size - 1u, 2, buf),
                     EXFAT_ERR_RANGE);
    assert_int_equal(exfat_file_read(&f, size, 0, buf), EXFAT_OK);
    synth_free(&sv);
}

/* The chain ends before the size says it should. Nothing in the entry itself
 * reveals that, so it only surfaces when a read reaches the missing cluster. */
static void test_file_chain_short_is_corrupt(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    exfat_dirent e;
    uint8_t buf[BUF_MAX];

    (void)state;
    mount_vol(&sv, &v, CHAIN_BPS, CHAIN_SPC, CHAIN_CLUSTERS);
    /* Two clusters of chain, an entry claiming five. */
    synth_write_file(&sv, chain_clusters, 2u, 2u * 1024u, 1);
    set_ent(&e, chain_clusters[0], (uint64_t)CHAIN_NCL * 1024u, 0);
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);

    /* The part the chain does cover reads fine. */
    check_read(&f, 0, 2048);
    /* The first byte past the end of the chain does not. */
    assert_int_equal(exfat_file_read(&f, 2048, 1, buf), EXFAT_ERR_CORRUPT);
    assert_int_equal(exfat_file_read(&f, 0, 4096, buf), EXFAT_ERR_CORRUPT);
    synth_free(&sv);
}

static void test_file_chain_bad_cluster_is_corrupt(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint64_t size = (uint64_t)CHAIN_NCL * 1024u;
    uint8_t buf[BUF_MAX];

    (void)state;
    open_chain(&sv, &v, &f, CHAIN_NCL, size);
    /* 0xFFFFFFF7 marks a bad cluster and must never be followed. */
    synth_fat_set(&sv, chain_clusters[1], 0xFFFFFFF7u);
    check_read(&f, 0, 2048);  /* clusters 0 and 1 are still reachable */
    assert_int_equal(exfat_file_read(&f, 2u * 1024u, 16, buf),
                     EXFAT_ERR_CORRUPT);

    /* A link pointing outside the heap is equally corrupt. */
    synth_fat_set(&sv, chain_clusters[1], CHAIN_CLUSTERS + 2u);
    assert_int_equal(exfat_file_read(&f, 2u * 1024u, 16, buf),
                     EXFAT_ERR_CORRUPT);
    /* So is a link to the free-cluster marker's neighbour, cluster 1. */
    synth_fat_set(&sv, chain_clusters[1], 1u);
    assert_int_equal(exfat_file_read(&f, 2u * 1024u, 16, buf),
                     EXFAT_ERR_CORRUPT);
    synth_free(&sv);
}

/* next_cluster refuses to look up a cluster number that is not in the heap.
 * Every caller inside the driver validates before it asks, so this too needs a
 * hand-built descriptor; it is the guard that keeps a corrupt cursor from
 * indexing the FAT out of bounds. */
static void test_file_chain_invalid_cursor_is_corrupt(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint8_t buf[16];

    (void)state;
    mount_vol(&sv, &v, CHAIN_BPS, CHAIN_SPC, CHAIN_CLUSTERS);
    memset(&f, 0, sizeof(f));
    f.vol = &v;
    f.first_cluster = 1; /* reserved: never a real cluster */
    f.cur_cluster = 1;
    f.size = 4096;
    f.sec_buf_off = ~(uint64_t)0;
    /* Offset 1024 is cluster index 1, so the cursor has to hop and the hop
     * starts from the bad cluster. */
    assert_int_equal(exfat_file_read(&f, 1024, 16, buf), EXFAT_ERR_CORRUPT);
    assert_int_equal(sv.reads, 1); /* only the mount's VBR read */
    synth_free(&sv);
}

/* ---------------------------------------------------------- I/O faults */

/* A failure in the direct, sector-aligned transfer path. */
static void test_file_read_io_error_aligned(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint8_t buf[BUF_MAX];

    (void)state;
    (void)open_contig(&sv, &v, &f);
    sv.fail_off = (int64_t)cluster_byte_off(&sv, CONTIG_FIRST);
    assert_int_equal(exfat_file_read(&f, 0, 1024, buf), EXFAT_ERR_IO);
    synth_free(&sv);
}

/* A failure while filling the bounce buffer, which must also leave the sector
 * cache marked invalid rather than advertising a sector it never read. */
static void test_file_read_io_error_bounce(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint8_t buf[BUF_MAX];

    (void)state;
    (void)open_contig(&sv, &v, &f);
    /* Serve the first sector, then fail the second, which the unaligned tail
     * of this request needs. */
    sv.fail_off = (int64_t)(cluster_byte_off(&sv, CONTIG_FIRST) + SECTOR);
    assert_int_equal(exfat_file_read(&f, 600, 8, buf), EXFAT_ERR_IO);
    assert_true(f.sec_buf_off == ~(uint64_t)0);

    /* And once the device recovers, the same read succeeds: the invalidated
     * cache did not poison the descriptor. */
    sv.fail_off = -1;
    check_read(&f, 600, 8);
    synth_free(&sv);
}

/* A failure reading the FAT sector a chain hop needs. */
static void test_file_chain_fat_read_io_error(void **state)
{
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    uint8_t buf[BUF_MAX];

    (void)state;
    open_chain(&sv, &v, &f, CHAIN_NCL, (uint64_t)CHAIN_NCL * 1024u);
    check_read(&f, 0, 512);
    /* The link from cluster 2 lives in FAT sector 0. */
    sv.fail_off = (int64_t)fat_sector_off(&sv, 0);
    assert_int_equal(exfat_file_read(&f, 1024, 16, buf), EXFAT_ERR_IO);
    assert_true(f.cur_index == 0); /* the cursor did not move past the fault */
    synth_free(&sv);
}

/* ----------------------------------------------------- 4 KB sectors */

/* The same arithmetic with the spec's largest sector, where one sector is one
 * whole cluster and exfat_file::sec_buf is filled to EXFAT_MAX_SECTOR. */
static void test_file_large_sector_reads(void **state)
{
    static const uint32_t cls[3] = { 4u, 9u, 6u };
    synth_vol sv;
    exfat_vol v;
    exfat_file f;
    exfat_dirent e;
    uint64_t size = 3u * 4096u;

    (void)state;
    mount_vol(&sv, &v, 12u, 0u, CONTIG_CLUSTERS);
    synth_write_file(&sv, cls, 3u, size, 1);
    set_ent(&e, cls[0], size, 0);
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);

    check_read(&f, 0, size);
    check_read(&f, 4095, 2);      /* across the first cluster boundary */
    check_read(&f, 5000, 17);     /* bounce buffer, mid cluster */
    check_read(&f, 8192, 4096);   /* the last cluster, aligned */
    assert_true(f.sec_buf_off != ~(uint64_t)0);
    synth_free(&sv);
}

/* ------------------------------------------------------------------ run */

int run_file_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_file_open_state),
        cmocka_unit_test(test_file_open_bad_arg),
        cmocka_unit_test(test_file_open_rejects_directory),
        cmocka_unit_test(test_file_open_cluster_out_of_range),
        cmocka_unit_test(test_file_open_zero_size),
        cmocka_unit_test(test_file_read_bad_arg),
        cmocka_unit_test(test_file_read_range),
        cmocka_unit_test(test_file_zero_length_read),
        cmocka_unit_test(test_file_read_unallocated_is_corrupt),
        cmocka_unit_test(test_file_contig_whole_file),
        cmocka_unit_test(test_file_contig_aligned),
        cmocka_unit_test(test_file_contig_unaligned_start),
        cmocka_unit_test(test_file_contig_unaligned_end),
        cmocka_unit_test(test_file_contig_tiny_mid_sector),
        cmocka_unit_test(test_file_sector_cache_reused),
        cmocka_unit_test(test_file_contig_ignores_fat),
        cmocka_unit_test(test_file_contig_past_heap_is_corrupt),
        cmocka_unit_test(test_file_contig_partial_overrun_still_reads),
        cmocka_unit_test(test_file_contig_cluster_index_overflow),
        cmocka_unit_test(test_file_chain_whole_file),
        cmocka_unit_test(test_file_chain_spans_cluster_boundary),
        cmocka_unit_test(test_file_chain_forward_read_resumes),
        cmocka_unit_test(test_file_chain_backward_seek_restarts),
        cmocka_unit_test(test_file_chain_ragged_final_cluster),
        cmocka_unit_test(test_file_chain_short_is_corrupt),
        cmocka_unit_test(test_file_chain_bad_cluster_is_corrupt),
        cmocka_unit_test(test_file_chain_invalid_cursor_is_corrupt),
        cmocka_unit_test(test_file_read_io_error_aligned),
        cmocka_unit_test(test_file_read_io_error_bounce),
        cmocka_unit_test(test_file_chain_fat_read_io_error),
        cmocka_unit_test(test_file_large_sector_reads),
    };

    return cmocka_run_group_tests_name("exfat file read", tests, NULL, NULL);
}
