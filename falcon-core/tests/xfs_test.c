/* Tests for falcon-core/src/fs/xfs.c, against the images
 * falcon-core/tools/mk-test-xfs.sh builds.
 *
 * What a fixture cannot be made to say (a bad checksum, a feature outside
 * the set) is laid over it as bytes, the superblock rewritten whole with
 * its checksum recomputed so the reader meets the one field under test.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>

#include <fs/crc32c.h>
#include <fs/fs.h>
#include <fs/xfs.h>

#define IMG      "build/test-xfs.img"
#define IMGFRAG  "build/test-xfs-frag.img"
#define IMGDIRTY "build/test-xfs-dirty.img"
#define IMGFAT   "build/test-fat32.img"

#define BIG_SIZE  307200u           /* 300 KiB */
#define FRAG_SIZE (299u * 8192u + 4096u)
#define PRE_SIZE  1048576u

/* ---- the fixture, and bytes laid over it ---- */

typedef struct {
    FILE    *fp;
    uint64_t off;
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

static xfs_vol g_v;
static xfs_dir g_d;
static xfs_file g_f;
static uint8_t g_buf[FRAG_SIZE > PRE_SIZE ? FRAG_SIZE : PRE_SIZE];

static void find(xfs_dir *d, const char *name, xfs_dirent *out)
{
    int rc;

    while ((rc = xfs_dir_next(d, out)) == 1) {
        if (strcmp((const char *)out->name, name) == 0)
            return;
    }
    fail_msg("no %s (dir_next %d)", name, rc);
}

static void root_find(const char *name, xfs_dirent *out)
{
    assert_int_equal(xfs_dir_open_root(&g_v, &g_d), XFS_OK);
    find(&g_d, name, out);
}

/* ---- the tests ---- */

static void test_xfs_probe(void **state)
{
    img m;

    (void)state;
    img_open(&m, IMG);
    assert_int_equal(xfs_probe(img_read, &m), XFS_OK);
    fclose(m.fp);
    img_open(&m, IMGFAT);
    assert_int_equal(xfs_probe(img_read, &m), XFS_ERR_NOT_XFS);
    assert_int_equal(xfs_mount(&g_v, img_read, &m), XFS_ERR_NOT_XFS);
    fclose(m.fp);
}

static void test_xfs_tree(void **state)
{
    xfs_dirent e;
    xfs_dir sub;
    char text[16];
    uint32_t i;
    img m;

    (void)state;
    img_open(&m, IMG);
    assert_int_equal(xfs_mount(&g_v, img_read, &m), XFS_OK);

    root_find("README.TXT", &e);
    assert_true(e.is_regular);
    assert_int_equal(xfs_file_open(&g_v, &g_f, &e), XFS_OK);
    assert_int_equal(g_f.inode.size, 7);
    memset(text, 0, sizeof text);
    assert_int_equal(xfs_file_read(&g_f, 0, 7, text), XFS_OK);
    assert_string_equal(text, "README\n");

    root_find("sub", &e);
    assert_true(e.is_dir);
    assert_int_equal(xfs_dir_open(&g_v, &sub, &e), XFS_OK);
    find(&sub, "deeper", &e);
    assert_int_equal(xfs_dir_open(&g_v, &sub, &e), XFS_OK);
    find(&sub, "leaf.txt", &e);
    assert_int_equal(xfs_file_open(&g_v, &g_f, &e), XFS_OK);
    memset(text, 0, sizeof text);
    assert_int_equal(xfs_file_read(&g_f, 0, 5, text), XFS_OK);
    assert_string_equal(text, "leaf\n");

    root_find("big.bin", &e);
    assert_int_equal(xfs_file_open(&g_v, &g_f, &e), XFS_OK);
    assert_int_equal(g_f.inode.size, BIG_SIZE);
    assert_int_equal(xfs_file_read(&g_f, 0, BIG_SIZE, g_buf), XFS_OK);
    for (i = 0; i < BIG_SIZE; i++)
        assert_int_equal(g_buf[i], i % 251u);
    assert_int_equal(xfs_file_read(&g_f, 4095, 3, g_buf), XFS_OK);
    assert_int_equal(g_buf[0], 4095u % 251u);
    assert_int_equal(g_buf[2], 4097u % 251u);
    assert_int_equal(xfs_file_read(&g_f, BIG_SIZE, 1, g_buf), XFS_ERR_RANGE);

    /* A directory is not a file. */
    root_find("sub", &e);
    assert_int_equal(xfs_file_open(&g_v, &g_f, &e), XFS_ERR_UNSUPPORTED);
    fclose(m.fp);
}

/* Every directory form: each entry once, "." and ".." never. */
static void test_xfs_dir_forms(void **state)
{
    static const struct { const char *name; unsigned n; uint8_t fmt; } dirs[] = {
        { "small", 3, 1 }, { "blockdir", 40, 2 }, { "mid", 300, 2 },
        { "many", 2000, 2 }
    };
    static uint8_t seen[2000];
    size_t k;
    img m;

    (void)state;
    img_open(&m, IMG);
    assert_int_equal(xfs_mount(&g_v, img_read, &m), XFS_OK);
    for (k = 0; k < sizeof dirs / sizeof dirs[0]; k++) {
        xfs_dirent e;
        xfs_dir d;
        unsigned n = 0;
        int rc;

        root_find(dirs[k].name, &e);
        assert_int_equal(xfs_dir_open(&g_v, &d, &e), XFS_OK);
        assert_int_equal(d.inode.format, dirs[k].fmt);
        memset(seen, 0, sizeof seen);
        while ((rc = xfs_dir_next(&d, &e)) == 1) {
            unsigned i = 0;
            unsigned c;

            assert_int_equal(e.name_len, 5);
            assert_int_equal(e.name[0], 'f');
            assert_true(e.is_regular);
            for (c = 1; c < 5; c++)
                i = i * 10u + (unsigned)(e.name[c] - '0');
            assert_true(i < dirs[k].n);
            assert_int_equal(seen[i], 0);
            seen[i] = 1;
            n++;
        }
        assert_int_equal(rc, 0);
        assert_int_equal(n, dirs[k].n);
    }
    fclose(m.fp);
}

/* A B+tree data fork, holes read as zeros; unwritten extents too. */
static void test_xfs_frag(void **state)
{
    xfs_dirent e;
    uint32_t i;
    img m;

    (void)state;
    img_open(&m, IMGFRAG);
    assert_int_equal(xfs_mount(&g_v, img_read, &m), XFS_OK);
    root_find("frag.bin", &e);
    assert_int_equal(xfs_file_open(&g_v, &g_f, &e), XFS_OK);
    assert_int_equal(g_f.inode.format, 3);
    assert_int_equal(g_f.inode.size, FRAG_SIZE);
    memset(g_buf, 0xAA, sizeof g_buf);
    assert_int_equal(xfs_file_read(&g_f, 0, FRAG_SIZE, g_buf), XFS_OK);
    for (i = 0; i < FRAG_SIZE; i++) {
        if ((i % 8192u) < 4096u)
            assert_int_equal(g_buf[i], i % 241u);
        else
            assert_int_equal(g_buf[i], 0);
    }

    root_find("prealloc.bin", &e);
    assert_int_equal(xfs_file_open(&g_v, &g_f, &e), XFS_OK);
    assert_int_equal(g_f.inode.size, PRE_SIZE);
    memset(g_buf, 0xAA, sizeof g_buf);
    assert_int_equal(xfs_file_read(&g_f, 0, PRE_SIZE, g_buf), XFS_OK);
    for (i = 0; i < PRE_SIZE; i++)
        assert_int_equal(g_buf[i], i < 4096u ? 0x5au : 0u);
    fclose(m.fp);
}

static void test_xfs_registry(void **state)
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
    assert_ptr_equal(fs_identify(img_read, &m, FS_HINT_SHELF), &fs_ops_xfs);
    assert_ptr_equal(fs_by_name("xfs"), &fs_ops_xfs);
    assert_int_equal(fs_mount_journal(&vol, &fs_ops_xfs, img_read, &m,
                                      (const fs_jmem *)0, &info), FS_OK);
    assert_int_equal(info.flags, 0);
    assert_int_equal(fs_lookup(&vol, "/SUB/deeper/LEAF.txt", &scratch, &e),
                     FS_OK);
    assert_int_equal(fs_file_open(&vol, &file, &e), FS_OK);
    assert_int_equal(fs_file_size(&file), 5);
    memset(text, 0, sizeof text);
    assert_int_equal(fs_file_read(&file, 0, 5, text), FS_OK);
    assert_string_equal(text, "leaf\n");
    assert_false(fs_has_extents(&vol));
    fclose(m.fp);
}

/* A log with no unmount record at its head. */
static void test_xfs_dirty(void **state)
{
    static fs_vol vol;
    img m;

    (void)state;
    img_open(&m, IMGDIRTY);
    assert_int_equal(xfs_mount(&g_v, img_read, &m), XFS_ERR_NEEDS_RECOVERY);
    assert_int_equal(fs_mount(&vol, &fs_ops_xfs, img_read, &m),
                     FS_ERR_NEEDS_RECOVERY);
    fclose(m.fp);
}

/* ---- the superblock rewritten ---- */

/* The checksum covers the superblock's sector, whose size it records. */
static uint8_t g_sb[4096];
static uint32_t g_sect;

static void sb_load(img *m)
{
    m->len = 0;
    assert_int_equal(img_read(m, 0, sizeof g_sb, g_sb), 0);
    g_sect = (uint32_t)g_sb[102] << 8 | g_sb[103];
    assert_true(g_sect >= 512u && g_sect <= sizeof g_sb);
}

static void sb_apply(img *m)
{
    uint32_t c;

    memset(g_sb + 224, 0, 4);
    c = crc32c(0xFFFFFFFFu, g_sb, g_sect) ^ 0xFFFFFFFFu;
    g_sb[224] = (uint8_t)c;
    g_sb[225] = (uint8_t)(c >> 8);
    g_sb[226] = (uint8_t)(c >> 16);
    g_sb[227] = (uint8_t)(c >> 24);
    m->off = 0;
    m->len = g_sect;
    m->bytes = g_sb;
}

static void test_xfs_refused(void **state)
{
    img m;

    (void)state;
    img_open(&m, IMG);

    /* needsrepair. */
    sb_load(&m);
    g_sb[219] |= 0x10u;
    sb_apply(&m);
    assert_int_equal(xfs_mount(&g_v, img_read, &m), XFS_ERR_UNSUPPORTED);

    /* v4. */
    sb_load(&m);
    g_sb[101] = (uint8_t)((g_sb[101] & 0xF0u) | 4u);
    sb_apply(&m);
    assert_int_equal(xfs_mount(&g_v, img_read, &m), XFS_ERR_UNSUPPORTED);

    /* Directory blocks of two filesystem blocks. */
    sb_load(&m);
    g_sb[192] = 1;
    sb_apply(&m);
    assert_int_equal(xfs_mount(&g_v, img_read, &m), XFS_ERR_UNSUPPORTED);

    /* A block size that is not what its log says. */
    sb_load(&m);
    g_sb[120]++;
    sb_apply(&m);
    assert_int_equal(xfs_mount(&g_v, img_read, &m), XFS_ERR_CORRUPT);
    fclose(m.fp);
}

static void test_xfs_checksums(void **state)
{
    static const uint8_t flip = 0x5a;
    uint64_t root;
    img m;

    (void)state;
    img_open(&m, IMG);

    m.off = 300;
    m.len = 1;
    m.bytes = &flip;
    assert_int_equal(xfs_mount(&g_v, img_read, &m), XFS_ERR_CSUM);

    /* A byte of the root directory's inode, past its core. */
    m.len = 0;
    assert_int_equal(xfs_mount(&g_v, img_read, &m), XFS_OK);
    root = g_v.rootino;
    m.off = ((root >> g_v.inopblog) << g_v.blocklog) +
            (root & ((1u << g_v.inopblog) - 1u)) * g_v.inodesize + 200u;
    m.len = 1;
    g_v.win_off = ~(uint64_t)0;
    assert_int_equal(xfs_dir_open_root(&g_v, &g_d), XFS_ERR_CSUM);
    fclose(m.fp);
}

int run_xfs_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_xfs_probe),
        cmocka_unit_test(test_xfs_tree),
        cmocka_unit_test(test_xfs_dir_forms),
        cmocka_unit_test(test_xfs_frag),
        cmocka_unit_test(test_xfs_registry),
        cmocka_unit_test(test_xfs_dirty),
        cmocka_unit_test(test_xfs_refused),
        cmocka_unit_test(test_xfs_checksums),
    };

    return cmocka_run_group_tests_name("xfs", tests, NULL, NULL);
}
