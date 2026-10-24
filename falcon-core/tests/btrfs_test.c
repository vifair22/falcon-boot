/* Tests for falcon-core/src/fs/btrfs.c, against the images
 * falcon-core/tools/mk-test-btrfs.sh builds with mkfs.btrfs --rootdir.
 *
 * What a fixture cannot be made to say (a log tree, a bad checksum, a
 * feature outside the set) is laid over it as bytes: the superblock is
 * rewritten whole with its checksum recomputed, so the reader meets the
 * one field under test and nothing else wrong.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>

#include <fs/btrfs.h>
#include <fs/crc32c.h>
#include <fs/fs.h>

#define IMG     "build/test-btrfs.img"
#define IMG4K   "build/test-btrfs-4k.img"
#define IMGZSTD "build/test-btrfs-zstd.img"
#define IMGFAT  "build/test-fat32.img"

#define BIG_SIZE    307200u     /* 300 KiB */
#define RUN         65536u      /* sparse.bin's data either side */
#define HOLE        1048576u    /* and its hole */
#define SPARSE_SIZE (RUN + HOLE + RUN)

/* ---- the fixture, and bytes laid over it ---- */

typedef struct {
    FILE    *fp;
    uint64_t off;           /* the patch, 0 bytes when len is 0 */
    uint32_t len;
    const uint8_t *bytes;
} img;

static int img_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    img *m = ctx;
    uint64_t lo;
    uint64_t hi;

    if (fseek(m->fp, (long)off, SEEK_SET) != 0 ||
        fread(buf, 1, (size_t)len, m->fp) != len)
        return -1;
    lo = m->off > off ? m->off : off;
    hi = m->off + m->len < off + len ? m->off + m->len : off + len;
    if (m->len > 0 && lo < hi)
        memcpy((uint8_t *)buf + (lo - off), m->bytes + (lo - m->off),
               (size_t)(hi - lo));
    return 0;
}

static void img_open(img *m, const char *path)
{
    memset(m, 0, sizeof *m);
    m->fp = fopen(path, "rb");
    assert_non_null(m->fp);
}

static void img_close(img *m)
{
    fclose(m->fp);
}

static btrfs_vol g_v;
static btrfs_dir g_d;
static btrfs_file g_f;
static uint8_t g_buf[SPARSE_SIZE];

/* The entry `name` in d, or fail. */
static void find(btrfs_dir *d, const char *name, btrfs_dirent *out)
{
    int rc;

    while ((rc = btrfs_dir_next(d, out)) == 1) {
        if (strcmp((const char *)out->name, name) == 0)
            return;
    }
    fail_msg("no %s (dir_next %d)", name, rc);
}

static void root_find(const char *name, btrfs_dirent *out)
{
    assert_int_equal(btrfs_dir_open_root(&g_v, &g_d), BTRFS_OK);
    find(&g_d, name, out);
}

/* ---- the tests ---- */

static void test_btrfs_probe(void **state)
{
    img m;

    (void)state;
    img_open(&m, IMG);
    assert_int_equal(btrfs_probe(img_read, &m), BTRFS_OK);
    img_close(&m);
    img_open(&m, IMGFAT);
    assert_int_equal(btrfs_probe(img_read, &m), BTRFS_ERR_NOT_BTRFS);
    assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_ERR_NOT_BTRFS);
    img_close(&m);
}

/* Both node sizes: the names in the root, the inline file, the nested one. */
static void test_btrfs_tree(void **state)
{
    static const char *const imgs[] = { IMG, IMG4K };
    size_t i;

    (void)state;
    for (i = 0; i < 2; i++) {
        btrfs_dirent e;
        btrfs_dir sub;
        char text[16];
        img m;

        img_open(&m, imgs[i]);
        assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_OK);
        assert_int_equal(g_v.nodesize, i == 0 ? 16384u : 4096u);
        assert_int_equal(g_v.flags, 0);

        root_find("README.TXT", &e);
        assert_true(e.is_regular);
        assert_int_equal(btrfs_file_open(&g_v, &g_f, &e), BTRFS_OK);
        assert_int_equal(g_f.size, 7);
        memset(text, 0, sizeof text);
        assert_int_equal(btrfs_file_read(&g_f, 0, 7, text), BTRFS_OK);
        assert_string_equal(text, "README\n");

        root_find("sub", &e);
        assert_true(e.is_dir);
        assert_int_equal(btrfs_dir_open(&g_v, &sub, &e), BTRFS_OK);
        find(&sub, "deeper", &e);
        assert_int_equal(btrfs_dir_open(&g_v, &sub, &e), BTRFS_OK);
        find(&sub, "leaf.txt", &e);
        assert_int_equal(btrfs_file_open(&g_v, &g_f, &e), BTRFS_OK);
        memset(text, 0, sizeof text);
        assert_int_equal(btrfs_file_read(&g_f, 0, 5, text), BTRFS_OK);
        assert_string_equal(text, "leaf\n");
        /* A directory is not a file. */
        assert_int_equal(btrfs_file_open(&g_v, &g_f, &(btrfs_dirent){
                             .ino = g_v.root_dir }), BTRFS_ERR_UNSUPPORTED);
        img_close(&m);
    }
}

/* A regular extent, whole and in ragged pieces. */
static void test_btrfs_big(void **state)
{
    btrfs_dirent e;
    uint64_t off;
    uint32_t i;
    img m;

    (void)state;
    img_open(&m, IMG);
    assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_OK);
    root_find("big.bin", &e);
    assert_int_equal(btrfs_file_open(&g_v, &g_f, &e), BTRFS_OK);
    assert_int_equal(g_f.size, BIG_SIZE);
    assert_int_equal(btrfs_file_read(&g_f, 0, BIG_SIZE, g_buf), BTRFS_OK);
    for (i = 0; i < BIG_SIZE; i++)
        assert_int_equal(g_buf[i], i % 251u);
    for (off = 1; off < BIG_SIZE; off += 4093u) {
        uint64_t n = BIG_SIZE - off < 777u ? BIG_SIZE - off : 777u;

        assert_int_equal(btrfs_file_read(&g_f, off, n, g_buf), BTRFS_OK);
        for (i = 0; i < n; i++)
            assert_int_equal(g_buf[i], (off + i) % 251u);
    }
    assert_int_equal(btrfs_file_read(&g_f, BIG_SIZE, 1, g_buf),
                     BTRFS_ERR_RANGE);
    assert_int_equal(btrfs_file_read(&g_f, BIG_SIZE, 0, g_buf), BTRFS_OK);
    img_close(&m);
}

/* Data, a hole read as zeros, data. */
static void test_btrfs_sparse(void **state)
{
    btrfs_dirent e;
    uint32_t i;
    img m;

    (void)state;
    img_open(&m, IMG);
    assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_OK);
    root_find("sparse.bin", &e);
    assert_int_equal(btrfs_file_open(&g_v, &g_f, &e), BTRFS_OK);
    assert_int_equal(g_f.size, SPARSE_SIZE);
    memset(g_buf, 0xAA, sizeof g_buf);
    assert_int_equal(btrfs_file_read(&g_f, 0, SPARSE_SIZE, g_buf), BTRFS_OK);
    for (i = 0; i < RUN; i++)
        assert_int_equal(g_buf[i], i % 239u);
    for (; i < RUN + HOLE; i++)
        assert_int_equal(g_buf[i], 0);
    for (; i < SPARSE_SIZE; i++)
        assert_int_equal(g_buf[i], (i - RUN - HOLE) % 233u);
    img_close(&m);
}

/* 300 entries span leaves; every one comes back once. */
static void test_btrfs_many(void **state)
{
    static const char *const imgs[] = { IMG, IMG4K };
    size_t k;

    (void)state;
    for (k = 0; k < 2; k++) {
        uint8_t seen[300];
        btrfs_dirent e;
        btrfs_dir many;
        unsigned n = 0;
        int rc;
        img m;

        img_open(&m, imgs[k]);
        assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_OK);
        root_find("many", &e);
        assert_int_equal(btrfs_dir_open(&g_v, &many, &e), BTRFS_OK);
        memset(seen, 0, sizeof seen);
        while ((rc = btrfs_dir_next(&many, &e)) == 1) {
            unsigned i;

            assert_int_equal(e.name_len, 4);
            assert_int_equal(e.name[0], 'f');
            i = (unsigned)(e.name[1] - '0') * 100u +
                (unsigned)(e.name[2] - '0') * 10u + (unsigned)(e.name[3] - '0');
            assert_true(i < 300u);
            assert_int_equal(seen[i], 0);
            seen[i] = 1;
            n++;
        }
        assert_int_equal(rc, 0);
        assert_int_equal(n, 300);
        img_close(&m);
    }
}

/* Through the registry, as a shelf is read. */
static void test_btrfs_registry(void **state)
{
    static fs_vol vol;
    static fs_dir scratch;
    static fs_file file;
    fs_dirent e;
    fs_jinfo info;
    char text[8];
    img m;

    (void)state;
    img_open(&m, IMG);
    assert_ptr_equal(fs_identify(img_read, &m, FS_HINT_SHELF), &fs_ops_btrfs);
    assert_ptr_equal(fs_by_name("btrfs"), &fs_ops_btrfs);
    assert_int_equal(fs_mount_journal(&vol, &fs_ops_btrfs, img_read, &m,
                                      (const fs_jmem *)0, &info), FS_OK);
    assert_int_equal(info.flags, 0);
    assert_int_equal(info.need, 0);
    assert_int_equal(fs_lookup(&vol, "/SUB/deeper/LEAF.txt", &scratch, &e),
                     FS_OK);
    assert_int_equal(fs_file_open(&vol, &file, &e), FS_OK);
    assert_int_equal(fs_file_size(&file), 5);
    memset(text, 0, sizeof text);
    assert_int_equal(fs_file_read(&file, 0, 5, text), FS_OK);
    assert_string_equal(text, "leaf\n");
    assert_int_equal(fs_lookup(&vol, "/nothere", &scratch, &e),
                     FS_ERR_NOT_FOUND);
    assert_false(fs_has_extents(&vol));
    img_close(&m);
}

/* A compressed extent is refused where it is read; the volume mounts. */
static void test_btrfs_compressed(void **state)
{
    btrfs_dirent e;
    img m;

    (void)state;
    img_open(&m, IMGZSTD);
    assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_OK);
    root_find("zeros.bin", &e);
    assert_int_equal(btrfs_file_open(&g_v, &g_f, &e), BTRFS_OK);
    assert_int_equal(btrfs_file_read(&g_f, 0, 4096, g_buf),
                     BTRFS_ERR_UNSUPPORTED);
    img_close(&m);
}

/* ---- the superblock rewritten ---- */

static uint8_t g_sb[4096];

static void sb_load(img *m)
{
    m->len = 0;
    assert_int_equal(img_read(m, BTRFS_SUPER_OFF, sizeof g_sb, g_sb), 0);
}

static void sb_put64(size_t at, uint64_t v)
{
    size_t i;

    for (i = 0; i < 8; i++)
        g_sb[at + i] = (uint8_t)(v >> (8 * i));
}

/* Lay the edited superblock over the image, its checksum made good. */
static void sb_apply(img *m)
{
    uint32_t c = crc32c(0xFFFFFFFFu, g_sb + 32, sizeof g_sb - 32) ^
                 0xFFFFFFFFu;

    memset(g_sb, 0, 32);
    g_sb[0] = (uint8_t)c;
    g_sb[1] = (uint8_t)(c >> 8);
    g_sb[2] = (uint8_t)(c >> 16);
    g_sb[3] = (uint8_t)(c >> 24);
    m->off = BTRFS_SUPER_OFF;
    m->len = sizeof g_sb;
    m->bytes = g_sb;
}

/* A log tree is a commit's fsyncs since; the volume is read without it,
 * and the registry says so as it does of ext4's fast commits. */
static void test_btrfs_log_tree(void **state)
{
    static fs_vol vol;
    fs_jinfo info;
    img m;

    (void)state;
    img_open(&m, IMG);
    sb_load(&m);
    sb_put64(0x60, 0x1234000u);
    sb_apply(&m);
    assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_OK);
    assert_int_equal(g_v.flags, BTRFS_LOG_SKIPPED);
    assert_int_equal(fs_mount_journal(&vol, &fs_ops_btrfs, img_read, &m,
                                      (const fs_jmem *)0, &info), FS_OK);
    assert_int_equal(info.flags, FS_JNL_FC_SKIPPED);
    img_close(&m);
}

static void test_btrfs_refused(void **state)
{
    uint64_t incompat;
    img m;

    (void)state;
    img_open(&m, IMG);

    /* A feature bit outside the set. */
    sb_load(&m);
    incompat = (uint64_t)g_sb[0xbc] | (uint64_t)g_sb[0xbd] << 8 |
               (uint64_t)g_sb[0xbe] << 16;
    sb_put64(0xbc, incompat | 0x1000u);          /* zoned */
    sb_apply(&m);
    assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_ERR_UNSUPPORTED);

    /* A second device. */
    sb_load(&m);
    sb_put64(0x88, 2);
    sb_apply(&m);
    assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_ERR_UNSUPPORTED);

    /* xxhash64 checksums. */
    sb_load(&m);
    g_sb[0xc4] = 1;
    sb_apply(&m);
    assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_ERR_UNSUPPORTED);

    /* A node size that is not a power of two. */
    sb_load(&m);
    g_sb[0x94] = 0x01;
    sb_apply(&m);
    assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_ERR_CORRUPT);
    img_close(&m);
}

static void test_btrfs_checksums(void **state)
{
    static const uint8_t flip = 0x5a;
    uint64_t phys = 0;
    uint32_t i;
    img m;

    (void)state;
    img_open(&m, IMG);

    /* The superblock's own, with no recomputation. */
    m.off = BTRFS_SUPER_OFF + 0x300u;
    m.len = 1;
    m.bytes = &flip;
    assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_ERR_CSUM);

    /* A byte inside the root tree's root node. */
    m.len = 0;
    assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_OK);
    for (i = 0; i < g_v.nchunks; i++) {
        const btrfs_chunk *c = &g_v.chunks[i];

        if (g_v.root_tree >= c->logical &&
            g_v.root_tree - c->logical < c->length)
            phys = c->physical + (g_v.root_tree - c->logical);
    }
    assert_true(phys != 0);
    m.off = phys + 4000u;
    m.len = 1;
    assert_int_equal(btrfs_mount(&g_v, img_read, &m), BTRFS_ERR_CSUM);
    img_close(&m);
}

int run_btrfs_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_btrfs_probe),
        cmocka_unit_test(test_btrfs_tree),
        cmocka_unit_test(test_btrfs_big),
        cmocka_unit_test(test_btrfs_sparse),
        cmocka_unit_test(test_btrfs_many),
        cmocka_unit_test(test_btrfs_registry),
        cmocka_unit_test(test_btrfs_compressed),
        cmocka_unit_test(test_btrfs_log_tree),
        cmocka_unit_test(test_btrfs_refused),
        cmocka_unit_test(test_btrfs_checksums),
    };

    return cmocka_run_group_tests_name("btrfs", tests, NULL, NULL);
}
