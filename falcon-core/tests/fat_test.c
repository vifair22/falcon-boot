/* Tests for falcon-core/src/fs/fat.c against real mkfs.vfat volumes
 * (falcon-core/tools/mk-test-fat.sh, DESIGN Section 6, the ISO path).
 *
 * Real images rather than synthesised ones, because the things most likely to
 * be wrong here are the things a hand-built fixture would get wrong in the
 * same direction as the driver: the exact cluster-count thresholds that pick
 * the FAT width, the layout of an LFN run and its checksum, and the lower-case
 * flags Windows records for a name that is 8.3 but not upper case. mkfs.vfat
 * and mtools are the second opinion.
 *
 * Every assertion runs against all three widths, since FAT12, FAT16 and FAT32
 * differ only in how a chain is followed and a driver that picks the wrong
 * width does not fail, it reads the wrong clusters.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fs/fat.h>
#include <fs/fs.h>
#include <blk/rebl.h>

#include "kndev.h"

#define IMG12 "build/test-fat12.img"
#define IMG16 "build/test-fat16.img"
#define IMG32 "build/test-fat32.img"

#define BIG_SIZE (100u * 1024u)
#define FRAG_SIZE (60u * 1024u)

typedef struct {
    const char *path;
    fat_type    expect;
} fixture;

static const fixture g_fixtures[] = {
    { IMG12, FAT_TYPE_12 },
    { IMG16, FAT_TYPE_16 },
    { IMG32, FAT_TYPE_32 }
};

#define FIXTURE_N (sizeof g_fixtures / sizeof g_fixtures[0])

static FILE *g_fp;

static int img_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    FILE *f = ctx;

    if (fseek(f, (long)off, SEEK_SET) != 0)
        return -1;
    return fread(buf, 1, (size_t)len, f) == len ? 0 : -1;
}

/* Reads that land in the data region, counted. Walking a chain also reads
 * FAT sectors, and how many depends on the FAT cache rather than on the file
 * read being tested, so those are left out: what is asserted is how many
 * requests the file's own bytes cost. */
typedef struct {
    FILE    *fp;
    uint64_t data_off;
    unsigned data_reads;
} counted;

static int counted_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    counted *c = ctx;

    if (off >= c->data_off)
        c->data_reads++;
    return img_read(c->fp, off, len, buf);
}

static FILE *open_img(const char *path)
{
    FILE *f = fopen(path, "rb");

    assert_non_null(f);
    return f;
}

/* UCS-2 against ASCII, which is all these names are. */
static int name_is(const fat_dirent *e, const char *want)
{
    size_t i;

    for (i = 0; want[i] != '\0'; i++) {
        if (e->name[i] != (uint16_t)(unsigned char)want[i])
            return 0;
    }
    return e->name[i] == 0 && e->name_len == i;
}

static int find_in_dir(fat_vol *v, fat_dir *d, const char *want,
                       fat_dirent *out)
{
    fat_dirent e;
    int rc;

    (void)v;
    while ((rc = fat_dir_next(d, &e)) == 1) {
        if (name_is(&e, want)) {
            *out = e;
            return 1;
        }
    }
    assert_int_equal(rc, 0);
    return 0;
}

static int find_in_root(fat_vol *v, const char *want, fat_dirent *out)
{
    fat_dir d;

    assert_int_equal(fat_dir_open_root(v, &d), FAT_OK);
    return find_in_dir(v, &d, want, out);
}

/* The FAT32 fixture with a few byte ranges laid over it, for a structure
 * mkfs.vfat will not write. */
#define PATCH_MAX 3u

typedef struct {
    uint64_t off;
    uint32_t len;
    const uint8_t *bytes;
} patch;

typedef struct {
    FILE *fp;
    patch p[PATCH_MAX];
    size_t n;
} patched;

static int patched_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    patched *pt = ctx;
    size_t i;

    if (img_read(pt->fp, off, len, buf) != 0)
        return -1;
    for (i = 0; i < pt->n; i++) {
        const patch *p = &pt->p[i];
        uint64_t lo = p->off > off ? p->off : off;
        uint64_t hi = p->off + p->len < off + len ? p->off + p->len
                                                  : off + len;

        if (lo < hi)
            memcpy((uint8_t *)buf + (lo - off), p->bytes + (lo - p->off),
                   (size_t)(hi - lo));
    }
    return 0;
}

static uint32_t le16_at(const uint8_t *b, size_t at)
{
    return (uint32_t)b[at] | (uint32_t)b[at + 1] << 8;
}

static uint32_t le32_at(const uint8_t *b, size_t at)
{
    return le16_at(b, at) | le16_at(b, at + 2) << 16;
}

/* ---- the tests, each run once per width ---- */

/* The width is derived from the cluster count, not recorded. If this is wrong
 * nothing else in the file can be right, so it goes first. */
static void test_fat_type_and_label(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        assert_int_equal((int)v.type, (int)g_fixtures[i].expect);
        assert_string_equal(v.label, "FALCON-FAT");
        assert_true(v.sector_size >= 512u);
        assert_true(v.cluster_size >= v.sector_size);
        assert_true(v.clusters > 0);
        fclose(g_fp);
    }
}

/* The four name shapes in the fixture, which between them cover the short
 * name path, the lower-case flags and a full LFN run. */
static void test_fat_names(void **state)
{
    static const char *want[] = {
        "README.TXT", "lower.txt", "a-long-file-name.txt", "NoExtension",
        "big.bin", "sub"
    };
    size_t i, k;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        for (k = 0; k < sizeof want / sizeof want[0]; k++) {
            fat_dirent e;

            assert_int_equal(find_in_root(&v, want[k], &e), 1);
        }
        fclose(g_fp);
    }
}

/* "." and ".." are this directory and its parent, and the volume label is the
 * volume's name; none of the three is a file and none may be produced. */
static void test_fat_hidden_entries(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;
        fat_dirent sub;
        fat_dir d;
        fat_dirent e;
        int rc;
        int saw = 0;

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);

        /* The root of a FAT12/16 volume carries the label entry. */
        assert_int_equal(fat_dir_open_root(&v, &d), FAT_OK);
        while ((rc = fat_dir_next(&d, &e)) == 1) {
            assert_false(name_is(&e, "FALCON-FAT"));
            assert_false(name_is(&e, "."));
            assert_false(name_is(&e, ".."));
        }
        assert_int_equal(rc, 0);

        /* A subdirectory carries "." and "..". */
        assert_int_equal(find_in_root(&v, "sub", &sub), 1);
        assert_true(sub.is_dir);
        assert_int_equal(fat_dir_open(&v, &d, &sub), FAT_OK);
        while ((rc = fat_dir_next(&d, &e)) == 1) {
            assert_false(name_is(&e, "."));
            assert_false(name_is(&e, ".."));
            if (name_is(&e, "nested.txt") || name_is(&e, "deeper"))
                saw++;
        }
        assert_int_equal(rc, 0);
        assert_int_equal(saw, 2);
        fclose(g_fp);
    }
}

static void test_fat_read_small_file(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;
        fat_file f;
        fat_dirent e;
        char buf[32];

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        assert_int_equal(find_in_root(&v, "a-long-file-name.txt", &e), 1);
        assert_false(e.is_dir);
        assert_true(e.is_regular);
        assert_int_equal(e.size, 19);
        assert_int_equal(fat_file_open(&v, &f, &e), FAT_OK);
        memset(buf, 0, sizeof buf);
        assert_int_equal(fat_file_read(&f, 0, e.size, buf), FAT_OK);
        assert_memory_equal(buf, "long name contents\n", 19);
        fclose(g_fp);
    }
}

/* 100 KiB spans several clusters on every width, so this is the chain walk.
 * Byte n is (n % 251), which makes a chain followed wrongly show up as a
 * value out of sequence rather than as a block of zeros. */
static void test_fat_read_multi_cluster(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;
        fat_file f;
        fat_dirent e;
        unsigned char *buf = malloc(BIG_SIZE);
        unsigned k;

        assert_non_null(buf);
        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        assert_int_equal(find_in_root(&v, "big.bin", &e), 1);
        assert_int_equal(e.size, BIG_SIZE);
        assert_int_equal(fat_file_open(&v, &f, &e), FAT_OK);
        memset(buf, 0xAA, BIG_SIZE);
        assert_int_equal(fat_file_read(&f, 0, BIG_SIZE, buf), FAT_OK);
        for (k = 0; k < BIG_SIZE; k++)
            assert_int_equal(buf[k], (unsigned char)(k % 251u));
        free(buf);
        fclose(g_fp);
    }
}

/* Unaligned and partial reads, which is where the bounce buffer lives. Read
 * the same file in odd-sized pieces at odd offsets and it must agree with the
 * whole-file read byte for byte. */
static void test_fat_read_unaligned(void **state)
{
    static const uint32_t offs[] = { 1u, 511u, 512u, 513u, 4095u, 40000u };
    size_t i, k;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;
        fat_file f;
        fat_dirent e;
        unsigned char buf[1000];

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        assert_int_equal(find_in_root(&v, "big.bin", &e), 1);
        assert_int_equal(fat_file_open(&v, &f, &e), FAT_OK);
        for (k = 0; k < sizeof offs / sizeof offs[0]; k++) {
            uint32_t off = offs[k];
            uint32_t len = 997u;
            uint32_t j;

            memset(buf, 0, sizeof buf);
            assert_int_equal(fat_file_read(&f, off, len, buf), FAT_OK);
            for (j = 0; j < len; j++)
                assert_int_equal(buf[j], (unsigned char)((off + j) % 251u));
        }
        /* Wholly inside the file or nothing. */
        assert_int_equal(fat_file_read(&f, BIG_SIZE - 1u, 2, buf),
                         FAT_ERR_RANGE);
        assert_int_equal(fat_file_read(&f, BIG_SIZE + 1u, 1, buf),
                         FAT_ERR_RANGE);
        assert_int_equal(fat_file_read(&f, BIG_SIZE, 0, buf), FAT_OK);
        fclose(g_fp);
    }
}

/* One device read per run of clusters that sit together on disk, not one per
 * sector: the startup probe hashes a 1 MiB loader out of an El Torito image
 * this way, and on firmware Block I/O every request costs a millisecond or
 * more. frag.bin is built in two pieces (falcon-core/tools/mk-test-fat.sh),
 * so it also checks that a run stops at the gap rather than reading on into
 * whatever lies past it. */
static void test_fat_read_coalesced(void **state)
{
    static const struct {
        const char *name;
        uint32_t    size;
        unsigned    mod;
        size_t      min_extents;
    } files[] = {
        { "big.bin",  BIG_SIZE,  251u, 1u },
        { "frag.bin", FRAG_SIZE, 241u, 2u },
    };
    size_t i, k;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        for (k = 0; k < sizeof files / sizeof files[0]; k++) {
            counted c;
            fat_vol v;
            fat_file f;
            fat_dirent e;
            fat_extent ext[32];
            size_t n = 0;
            unsigned char *buf = malloc(files[k].size);
            uint32_t j;

            assert_non_null(buf);
            c.fp = open_img(g_fixtures[i].path);
            c.data_off = 0;
            c.data_reads = 0;
            assert_int_equal(fat_mount(&v, counted_read, &c), FAT_OK);
            c.data_off = (uint64_t)v.data_off_sectors * v.sector_size;
            assert_int_equal(find_in_root(&v, files[k].name, &e), 1);
            assert_int_equal(e.size, files[k].size);
            assert_int_equal(fat_file_extents(&v, &e, ext, 32, &n), FAT_OK);
            assert_true(n >= files[k].min_extents);
            assert_int_equal(fat_file_open(&v, &f, &e), FAT_OK);

            c.data_reads = 0;
            memset(buf, 0xAA, files[k].size);
            assert_int_equal(fat_file_read(&f, 0, files[k].size, buf),
                             FAT_OK);
            assert_int_equal(c.data_reads, n);
            for (j = 0; j < files[k].size; j++)
                assert_int_equal(buf[j], (unsigned char)(j % files[k].mod));
            free(buf);
            fclose(c.fp);
        }
    }
}

/* Reads that start or end inside a sector and cross frag.bin's gap: the
 * bounced edges and the coalesced middle have to meet without a byte lost or
 * repeated on either side of it. */
static void test_fat_read_across_gap(void **state)
{
    static const uint32_t before[] = { 1u, 700u, 2048u, 5000u };
    static const uint32_t lens[] = { 2u, 1500u, 4096u, 9001u };
    size_t i, a, b;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;
        fat_file f;
        fat_dirent e;
        fat_extent ext[32];
        size_t n = 0;
        uint32_t gap;
        unsigned char buf[9001];

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        assert_int_equal(find_in_root(&v, "frag.bin", &e), 1);
        assert_int_equal(fat_file_extents(&v, &e, ext, 32, &n), FAT_OK);
        assert_true(n >= 2u);
        gap = (uint32_t)ext[0].len;       /* file offset of the first gap */
        assert_int_equal(fat_file_open(&v, &f, &e), FAT_OK);

        for (a = 0; a < sizeof before / sizeof before[0]; a++) {
            for (b = 0; b < sizeof lens / sizeof lens[0]; b++) {
                uint32_t off = gap - before[a];
                uint32_t len = lens[b];
                uint32_t j;

                if (off + len > FRAG_SIZE)
                    continue;
                memset(buf, 0, sizeof buf);
                assert_int_equal(fat_file_read(&f, off, len, buf), FAT_OK);
                for (j = 0; j < len; j++)
                    assert_int_equal(buf[j],
                                     (unsigned char)((off + j) % 241u));
            }
        }
        fclose(g_fp);
    }
}

static void test_fat_nested_directory(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;
        fat_dir d;
        fat_dirent sub, deeper, leaf;
        fat_file f;
        char buf[16];

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        assert_int_equal(find_in_root(&v, "sub", &sub), 1);
        assert_int_equal(fat_dir_open(&v, &d, &sub), FAT_OK);
        assert_int_equal(find_in_dir(&v, &d, "deeper", &deeper), 1);
        assert_true(deeper.is_dir);
        assert_int_equal(fat_dir_open(&v, &d, &deeper), FAT_OK);
        assert_int_equal(find_in_dir(&v, &d, "leaf.txt", &leaf), 1);
        assert_int_equal(leaf.size, 5);
        assert_int_equal(fat_file_open(&v, &f, &leaf), FAT_OK);
        memset(buf, 0, sizeof buf);
        assert_int_equal(fat_file_read(&f, 0, leaf.size, buf), FAT_OK);
        assert_memory_equal(buf, "leaf\n", 5);
        fclose(g_fp);
    }
}

/* The extents cover the file's size exactly, and each run is inside the
 * volume. mkfs.vfat writes these files contiguously, so one extent is the
 * expected answer, but the assertion is on the total rather than the count. */
static void test_fat_extents(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        fat_vol v;
        fat_dirent e;
        fat_extent ext[32];
        size_t n = 0;
        size_t k;
        uint64_t total = 0;

        g_fp = open_img(g_fixtures[i].path);
        assert_int_equal(fat_mount(&v, img_read, g_fp), FAT_OK);
        assert_int_equal(find_in_root(&v, "big.bin", &e), 1);
        assert_int_equal(fat_file_extents(&v, &e, ext, 32, &n), FAT_OK);
        assert_true(n > 0);
        for (k = 0; k < n; k++) {
            assert_true(ext[k].len > 0);
            assert_true(ext[k].vol_off >=
                        (uint64_t)v.data_off_sectors * v.sector_size);
            total += ext[k].len;
        }
        assert_int_equal(total, BIG_SIZE);

        /* A cap too small reports what was needed rather than truncating
         * silently, same contract as every other driver. */
        assert_int_equal(fat_file_extents(&v, &e, ext, 0, &n),
                         FAT_ERR_NOSPACE);
        assert_true(n > 0);
        fclose(g_fp);
    }
}

/* Through the registry, since that is how every consumer reaches a driver:
 * fs_identify must pick FAT for these images without being told. */
static void test_fat_through_registry(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        const fs_ops *ops;
        fs_vol vol;
        fs_file file;
        fs_dirent ent;
        fs_dir scratch;
        char buf[8];

        g_fp = open_img(g_fixtures[i].path);
        ops = fs_identify(img_read, g_fp, FS_HINT_NONE);
        assert_non_null(ops);
        assert_string_equal(ops->name, "fat");
        assert_int_equal(fs_mount(&vol, ops, img_read, g_fp), FS_OK);
        assert_string_equal(fs_vol_label(&vol), "FALCON-FAT");

        /* A path the registry resolves, which exercises dir_open through the
         * vtable rather than the driver's own entry points. */
        assert_int_equal(fs_lookup(&vol, "/sub/deeper/leaf.txt", &scratch, &ent),
                         FS_OK);
        assert_false(ent.is_dir);
        assert_int_equal(ent.size, 5);
        assert_int_equal(fs_file_open(&vol, &file, &ent), FS_OK);
        assert_int_equal(fs_file_size(&file), 5);
        memset(buf, 0, sizeof buf);
        assert_int_equal(fs_file_read(&file, 0, 5, buf), FS_OK);
        assert_memory_equal(buf, "leaf\n", 5);
        fclose(g_fp);
    }
}

/* A volume that is not FAT must be declined by probe rather than mounted into
 * nonsense. The image's own middle is a convenient non-BPB. */
static void test_fat_rejects_non_fat(void **state)
{
    fat_vol v;
    uint8_t zero[512];
    FILE *f;

    (void)state;
    memset(zero, 0, sizeof zero);
    f = tmpfile();
    assert_non_null(f);
    assert_int_equal(fwrite(zero, 1, sizeof zero, f), sizeof zero);
    fflush(f);
    assert_int_equal(fat_probe(img_read, f), FAT_ERR_NOT_FAT);
    assert_int_equal(fat_mount(&v, img_read, f), FAT_ERR_NOT_FAT);
    fclose(f);
}

/* A FAT32 root directory whose one cluster names itself as the next, full
 * of deleted entries so the walk never meets an end marker. The chain is a
 * loop, and before the cap the walk followed it forever: at mount, since
 * the label is read from the root, with nothing on screen. */
static void test_fat_looping_directory(void **state)
{
    static uint8_t deleted[FAT_MAX_SECTOR];
    uint8_t bpb[512];
    uint8_t link[4];
    uint32_t bps, spc, fat_sz, root, i;
    uint64_t fat_off, data_off;
    patched pt;
    fat_vol v;
    fat_dir d;
    fat_dirent e;
    int rc;

    (void)state;
    memset(&pt, 0, sizeof pt);
    pt.fp = open_img(IMG32);
    assert_int_equal(img_read(pt.fp, 0, sizeof bpb, bpb), 0);
    bps = le16_at(bpb, 0x0B);
    spc = bpb[0x0D];
    fat_sz = le32_at(bpb, 0x24);
    root = le32_at(bpb, 0x2C);
    assert_true(bps * spc <= sizeof deleted);
    fat_off = (uint64_t)le16_at(bpb, 0x0E) * bps;
    data_off = fat_off + (uint64_t)bpb[0x10] * fat_sz * bps;

    for (i = 0; i < 4; i++)
        link[i] = (uint8_t)(root >> (8u * i));
    for (i = 0; i < bps * spc; i += 32u)
        deleted[i] = 0xE5;

    pt.p[0] = (patch){ fat_off + (uint64_t)root * 4u, 4u, link };
    pt.p[1] = (patch){ fat_off + (uint64_t)fat_sz * bps + (uint64_t)root * 4u,
                       4u, link };
    pt.p[2] = (patch){ data_off + (uint64_t)(root - 2u) * spc * bps,
                       bps * spc, deleted };
    pt.n = 3;

    assert_int_equal(fat_mount(&v, patched_read, &pt), FAT_OK);
    assert_int_equal(fat_dir_open_root(&v, &d), FAT_OK);
    while ((rc = fat_dir_next(&d, &e)) == 1)
        ;
    assert_int_equal(rc, FAT_ERR_CORRUPT);
    fclose(pt.fp);
}

static void test_fat_arg_errors(void **state)
{
    fat_vol v;
    fat_dir d;
    fat_file fl;
    fat_dirent e;

    (void)state;
    memset(&e, 0, sizeof e);
    assert_int_equal(fat_mount(NULL, img_read, NULL), FAT_ERR_BAD_ARG);
    assert_int_equal(fat_mount(&v, NULL, NULL), FAT_ERR_BAD_ARG);
    assert_int_equal(fat_probe(NULL, NULL), FAT_ERR_BAD_ARG);
    assert_int_equal(fat_dir_open_root(NULL, &d), FAT_ERR_BAD_ARG);
    assert_int_equal(fat_dir_next(NULL, &e), FAT_ERR_BAD_ARG);
    /* A directory is not a byte stream and a file is not a directory. */
    e.is_dir = 1;
    assert_int_equal(fat_file_open(&v, &fl, &e), FAT_ERR_BAD_ARG);
    e.is_dir = 0;
    assert_int_equal(fat_dir_open(&v, &d, &e), FAT_ERR_BAD_ARG);
}

/* Every width on a 4096-byte-sector disk (docs/DESIGN.md Section 3.4):
 * whole 4096-byte sectors only, under the core's re-blocking layer as the
 * loaders hand it to the drivers. The volumes keep their 512-byte sectors,
 * so the boot sector, every FAT sector and every directory sector is a
 * piece of a device sector, and the FAT12 floppy image is the jump
 * floppy's shape on a disk it would never be written to. */
static void test_fat_on_4kn(void **state)
{
    size_t i;

    (void)state;
    for (i = 0; i < FIXTURE_N; i++) {
        kn_dev k;
        const fs_ops *ops;
        fs_vol vol;
        fs_file file;
        fs_dirent ent;
        fs_dir scratch;
        fat_vol v;
        unsigned char *buf = malloc(BIG_SIZE);
        unsigned n;

        assert_non_null(buf);
        g_fp = open_img(g_fixtures[i].path);
        kn_init(&k, img_read, g_fp);

        ops = fs_identify(blk_rebl_read, &k.rebl, FS_HINT_NONE);
        assert_ptr_equal(ops, &fs_ops_fat);
        assert_int_equal(fs_mount(&vol, ops, blk_rebl_read, &k.rebl), FS_OK);
        assert_string_equal(fs_vol_label(&vol), "FALCON-FAT");

        assert_int_equal(fs_lookup(&vol, "/sub/deeper/leaf.txt", &scratch,
                                   &ent), FS_OK);
        assert_int_equal(fs_file_open(&vol, &file, &ent), FS_OK);
        memset(buf, 0, 8);
        assert_int_equal(fs_file_read(&file, 0, 5, buf), FS_OK);
        assert_memory_equal(buf, "leaf\n", 5);

        assert_int_equal(fs_lookup(&vol, "/big.bin", &scratch, &ent), FS_OK);
        assert_int_equal(fs_file_open(&vol, &file, &ent), FS_OK);
        memset(buf, 0xAA, BIG_SIZE);
        assert_int_equal(fs_file_read(&file, 0, BIG_SIZE, buf), FS_OK);
        for (n = 0; n < BIG_SIZE; n++)
            assert_int_equal(buf[n], (unsigned char)(n % 251u));
        assert_int_equal(fs_file_read(&file, 4095u, 5000u, buf), FS_OK);
        for (n = 0; n < 5000u; n++)
            assert_int_equal(buf[n], (unsigned char)((4095u + n) % 251u));
        assert_int_equal(k.strict.refused, 0);

        /* Without the layer, the 512-byte boot sector read is refused. */
        assert_int_equal(fat_mount(&v, kn_strict_read, &k.strict),
                         FAT_ERR_IO);
        assert_int_equal(k.strict.refused, 1);
        free(buf);
        fclose(g_fp);
    }
}

int run_fat_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_fat_type_and_label),
        cmocka_unit_test(test_fat_names),
        cmocka_unit_test(test_fat_hidden_entries),
        cmocka_unit_test(test_fat_read_small_file),
        cmocka_unit_test(test_fat_read_multi_cluster),
        cmocka_unit_test(test_fat_read_unaligned),
        cmocka_unit_test(test_fat_read_coalesced),
        cmocka_unit_test(test_fat_read_across_gap),
        cmocka_unit_test(test_fat_nested_directory),
        cmocka_unit_test(test_fat_extents),
        cmocka_unit_test(test_fat_through_registry),
        cmocka_unit_test(test_fat_on_4kn),
        cmocka_unit_test(test_fat_rejects_non_fat),
        cmocka_unit_test(test_fat_arg_errors),
        cmocka_unit_test(test_fat_looping_directory),
    };
    FILE *f = fopen(IMG12, "rb");

    if (f == NULL) {
        printf("\n[  SKIPPED ] fat fixtures: %s not found.\n"
               "             Build them with: make test-fat "
               "(needs mkfs.vfat and mtools).\n", IMG12);
        return test_skip_rc();
    }
    fclose(f);

    return cmocka_run_group_tests_name("fat", tests, NULL, NULL);
}
