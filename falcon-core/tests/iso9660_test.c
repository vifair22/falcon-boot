/* falcon-core/src/fs/iso9660.c on images built here, byte by byte.
 *
 * media_fs_test.c proves the driver agrees with xorriso. This file reaches
 * what xorriso never writes: records that overrun their block, Rock Ridge
 * continuation chains that loop, relocation records that point at nothing,
 * multi-extent files with the wrong number of parts, a descriptor set with no
 * primary. Each is a structure someone can put on p2, and each must come back
 * as a status.
 *
 * The builder writes just enough ISO9660 for the case at hand into a 64-block
 * buffer: descriptors from block 16, directories and data wherever the test
 * puts them. Both-endian fields get both halves, so a failure here is the
 * driver's and not a half-built record's.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <fs/iso9660.h>

#define B      ISO9660_BLOCK
#define NBLK   64u

static uint8_t img[NBLK * B];

/* Reads at or past fail_off fail, for the IO paths. */
static uint64_t fail_off = ~(uint64_t)0;

static int img_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    if (off + len > sizeof img || off + len > fail_off)
        return -1;
    memcpy(buf, img + off, (size_t)len);
    return 0;
}

/* img, then zeros to any offset: a volume far larger than the buffer, for
 * a walk that has to reach an offset the buffer does not cover. */
static int sparse_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    if (off >= sizeof img) {
        memset(buf, 0, (size_t)len);
        return 0;
    }
    return img_read(ctx, off, len, buf);
}

static void put16b(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put32b(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

/* One directory record at p; returns its length. su is appended after the
 * name and its padding byte. */
static size_t rec(uint8_t *p, uint32_t lba, uint32_t len, uint8_t flags,
                  const void *name, uint8_t nlen, const void *su, size_t slen)
{
    size_t n = 33u + nlen + ((nlen & 1u) ? 0u : 1u) + slen;

    memset(p, 0, n);
    p[0] = (uint8_t)n;
    put32b(p + 2, lba);
    put32b(p + 10, len);
    p[25] = flags;
    put16b(p + 28, 1);
    p[32] = nlen;
    memcpy(p + 33, name, nlen);
    if (slen)
        memcpy(p + 33 + nlen + ((nlen & 1u) ? 0u : 1u), su, slen);
    return n;
}

static const uint8_t DOT = 0, DOTDOT = 1;

/* Plain records totalling exactly `total` bytes (each 34..250; the bytes
 * past a record's name are a System Use area nothing reads without Rock
 * Ridge), for walking the directory offset to a chosen point. */
static uint8_t *filler(uint8_t *p, size_t total)
{
    while (total > 0) {
        size_t n = total > 250u ? 250u : total;

        if (total > 250u && total - 250u < 34u)
            n = total - 34u;
        rec(p, 30, 1, 0, "F", 1, NULL, 0);
        p[0] = (uint8_t)n;
        p += n;
        total -= n;
    }
    return p;
}

/* "." and ".." for a directory at lba; returns where the next record goes.
 * su goes on "." only, which is where Rock Ridge's SP lives. */
static uint8_t *dir_start(uint32_t lba, uint32_t len, const void *su,
                          size_t slen)
{
    uint8_t *p = img + (size_t)lba * B;

    p += rec(p, lba, len, 0x02, &DOT, 1, su, slen);
    p += rec(p, lba, len, 0x02, &DOTDOT, 1, NULL, 0);
    return p;
}

static void vd(unsigned blk, uint8_t type)
{
    uint8_t *d = img + (size_t)blk * B;

    memset(d, 0, B);
    d[0] = type;
    memcpy(d + 1, "CD001", 5);
    d[6] = 1;
}

static void pvd_at(unsigned blk, uint8_t type, uint32_t root, uint32_t rlen)
{
    uint8_t *d = img + (size_t)blk * B;

    vd(blk, type);
    memset(d + 40, ' ', 32);
    memcpy(d + 40, "SYNTH_VOL", 9);
    put16b(d + 128, B);
    rec(d + 156, root, rlen, 0x02, &DOT, 1, NULL, 0);
}

/* A fresh image: PVD at 16 naming a one-block root at 20, terminator at 17.
 * Returns where the root's third record goes. */
static uint8_t *fresh(const void *root_su, size_t slen)
{
    memset(img, 0, sizeof img);
    fail_off = ~(uint64_t)0;
    pvd_at(16, 1, 20, B);
    vd(17, 255);
    return dir_start(20, B, root_su, slen);
}

/* SUSP SP with LEN_SKP. */
static size_t sp(uint8_t *p, uint8_t skip)
{
    p[0] = 'S'; p[1] = 'P'; p[2] = 7; p[3] = 1;
    p[4] = 0xBE; p[5] = 0xEF; p[6] = skip;
    return 7;
}

static size_t nm(uint8_t *p, uint8_t flags, const char *s)
{
    size_t n = strlen(s);

    p[0] = 'N'; p[1] = 'M'; p[2] = (uint8_t)(5 + n); p[3] = 1; p[4] = flags;
    memcpy(p + 5, s, n);
    return 5 + n;
}

static size_t px(uint8_t *p, uint32_t mode)
{
    memset(p, 0, 36);
    p[0] = 'P'; p[1] = 'X'; p[2] = 36; p[3] = 1;
    put32b(p + 4, mode);
    return 36;
}

static size_t ce(uint8_t *p, uint32_t lba, uint32_t off, uint32_t len)
{
    memset(p, 0, 28);
    p[0] = 'C'; p[1] = 'E'; p[2] = 28; p[3] = 1;
    put32b(p + 4, lba);
    put32b(p + 12, off);
    put32b(p + 20, len);
    return 28;
}

static size_t simple(uint8_t *p, const char a, const char b)
{
    p[0] = (uint8_t)a; p[1] = (uint8_t)b; p[2] = 4; p[3] = 1;
    return 4;
}

static size_t cl(uint8_t *p, uint32_t lba)
{
    p[0] = 'C'; p[1] = 'L'; p[2] = 12; p[3] = 1;
    put32b(p + 4, lba);
    return 12;
}

static iso9660_vol v;
static iso9660_dir d;
static iso9660_file f;
static iso9660_dirent e;

static void name_is(const iso9660_dirent *x, const char *want)
{
    char got[ISO9660_NAME_MAX + 1];

    ucs2_to_ascii(x->name, got, sizeof got);
    assert_string_equal(got, want);
}

static void mount_root(void)
{
    assert_int_equal(iso9660_mount(&v, img_read, NULL), ISO9660_OK);
    assert_int_equal(iso9660_dir_open_root(&v, &d), ISO9660_OK);
}

/* --------------------------------------------------------------- mount */

static void test_iso_mount_plain(void **state)
{
    uint8_t *p = fresh(NULL, 0);
    char buf[8];

    (void)state;
    rec(p, 30, 5, 0, "HELLO.TXT;1", 11, NULL, 0);
    memcpy(img + 30 * B, "hello", 5);

    assert_int_equal(iso9660_probe(img_read, NULL), ISO9660_OK);
    mount_root();
    assert_int_equal(v.names, ISO9660_NAMES_PLAIN);
    assert_string_equal(v.label, "SYNTH_VOL");
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "HELLO.TXT");
    assert_int_equal(e.size, 5);
    assert_int_equal(iso9660_dir_next(&d, &e), 0);
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_OK);
    assert_int_equal(iso9660_file_read(&f, 0, 5, buf), ISO9660_OK);
    assert_memory_equal(buf, "hello", 5);
}

static void test_iso_mount_refusals(void **state)
{
    (void)state;

    fresh(NULL, 0);
    memcpy(img + 16 * B + 1, "CDXXX", 5);
    assert_int_equal(iso9660_probe(img_read, NULL), ISO9660_ERR_BAD_SUPER);
    assert_int_equal(iso9660_mount(&v, img_read, NULL), ISO9660_ERR_BAD_SUPER);

    fresh(NULL, 0);
    put16b(img + 16 * B + 128, 512);
    assert_int_equal(iso9660_mount(&v, img_read, NULL),
                     ISO9660_ERR_UNSUPPORTED);

    fresh(NULL, 0);
    img[16 * B + 156] = 20;             /* root record shorter than 34 */
    assert_int_equal(iso9660_mount(&v, img_read, NULL), ISO9660_ERR_CORRUPT);

    fresh(NULL, 0);
    put32b(img + 16 * B + 156 + 10, 0); /* empty root */
    assert_int_equal(iso9660_mount(&v, img_read, NULL), ISO9660_ERR_CORRUPT);

    /* A set with only a supplementary descriptor has no primary. */
    fresh(NULL, 0);
    img[16 * B] = 2;
    assert_int_equal(iso9660_mount(&v, img_read, NULL), ISO9660_ERR_BAD_SUPER);

    /* No terminator: the set ends where CD001 stops, and that is fine. */
    fresh(NULL, 0);
    memset(img + 17 * B, 0, B);
    assert_int_equal(iso9660_mount(&v, img_read, NULL), ISO9660_OK);

    fresh(NULL, 0);
    fail_off = 16 * B;
    assert_int_equal(iso9660_probe(img_read, NULL), ISO9660_ERR_IO);
    assert_int_equal(iso9660_mount(&v, img_read, NULL), ISO9660_ERR_IO);
    fail_off = 20 * B;                  /* descriptors fine, root unreadable */
    assert_int_equal(iso9660_mount(&v, img_read, NULL), ISO9660_ERR_IO);

    assert_int_equal(iso9660_probe(NULL, NULL), ISO9660_ERR_BAD_ARG);
    assert_int_equal(iso9660_mount(NULL, img_read, NULL), ISO9660_ERR_BAD_ARG);
    assert_int_equal(iso9660_mount(&v, NULL, NULL), ISO9660_ERR_BAD_ARG);
}

/* --------------------------------------------------------------- records */

static void test_iso_record_shapes(void **state)
{
    uint8_t *p;

    (void)state;

    /* Zero padding ends a block early; the walk resumes at the next one. */
    p = fresh(NULL, 0);
    put32b(img + 16 * B + 156 + 10, 2 * B);
    put32b(img + 20 * B + 10, 2 * B);
    rec(p, 30, 1, 0, "A", 1, NULL, 0);
    rec(img + 21 * B, 31, 1, 0x01, "B", 1, NULL, 0);    /* hidden: listed */
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "A");
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "B");
    assert_int_equal(iso9660_dir_next(&d, &e), 0);

    /* An associated file (a resource fork) is not produced. */
    p = fresh(NULL, 0);
    p += rec(p, 30, 1, 0x04, "R", 1, NULL, 0);
    rec(p, 31, 1, 0, "S", 1, NULL, 0);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "S");

    /* A record that runs off its block: filler records walk the offset to
     * 30 bytes short of the block's end, and the next record claims 60. */
    p = fresh(NULL, 0);
    p = filler(p, (size_t)(img + 21 * B - 30 - p));
    rec(p, 30, 1, 0, "X", 1, NULL, 0);
    p[0] = 60;
    mount_root();
    {
        int rc;

        do {
            rc = iso9660_dir_next(&d, &e);
        } while (rc == 1);
        assert_int_equal(rc, ISO9660_ERR_CORRUPT);
    }

    /* Shorter than the smallest record. */
    p = fresh(NULL, 0);
    rec(p, 30, 1, 0, "Z", 1, NULL, 0);
    p[0] = 20;
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_CORRUPT);

    /* A name longer than its record. */
    p = fresh(NULL, 0);
    rec(p, 30, 1, 0, "Z", 1, NULL, 0);
    p[32] = 200;
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_CORRUPT);

    /* The directory itself unreadable. */
    p = fresh(NULL, 0);
    rec(p, 30, 1, 0, "Z", 1, NULL, 0);
    mount_root();
    fail_off = 20 * B;
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_IO);
    fail_off = ~(uint64_t)0;

    /* A directory extent a block short of 4 GiB, on a volume big enough to
     * hold it, with its last block zero. The step past that block reaches
     * 2^32, where a 32-bit offset wrapped to the directory's start and the
     * walk began again with "A", forever. Started at the last block rather
     * than walked to it, which would be two million reads. */
    p = fresh(NULL, 0);
    rec(p, 30, 1, 0, "A", 1, NULL, 0);
    mount_root();
    v.read = sparse_read;
    d.len = 0xFFFFFFFFu;
    d.pos = (uint64_t)0xFFFFFFFFu / B * B;
    assert_int_equal(iso9660_dir_next(&d, &e), 0);
}

static void test_iso_names(void **state)
{
    uint8_t *p = fresh(NULL, 0);

    (void)state;
    p += rec(p, 30, 1, 0, "NOEXT.;1", 8, NULL, 0);
    p += rec(p, 30, 1, 0, ";1", 2, NULL, 0);
    p += rec(p, 30, 1, 0, "A.B", 3, NULL, 0);
    rec(p, 30, 1, 0, ".", 1, NULL, 0);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "NOEXT");               /* version and empty extension gone */
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, ";1");                  /* nothing left to strip it to */
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "A.B");
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, ".");
}

/* ------------------------------------------------------------- Joliet */

static void joliet_svd(uint32_t root, uint8_t esc)
{
    pvd_at(17, 2, root, B);
    img[17 * B + 88] = '%';
    img[17 * B + 89] = '/';
    img[17 * B + 90] = esc;
    vd(18, 255);
}

static void test_iso_joliet(void **state)
{
    static const uint8_t jname[] = { 0, 'a', 0, '.', 0, 't', 0, 'x',
                                     0, 't', 0, ';', 0, '1' };
    static const uint8_t wide[] = { 0x04, 0x10, 0, 'z' };
    uint8_t *p;

    (void)state;
    p = fresh(NULL, 0);
    rec(p, 30, 1, 0, "PLAIN", 5, NULL, 0);
    joliet_svd(25, 'E');
    p = dir_start(25, B, NULL, 0);
    p += rec(p, 30, 1, 0, jname, sizeof jname, NULL, 0);
    rec(p, 30, 1, 0, wide, sizeof wide, NULL, 0);
    mount_root();
    assert_int_equal(v.names, ISO9660_NAMES_JOLIET);
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "a.txt");
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    assert_int_equal(e.name[0], 0x0410);  /* UCS-2 kept whole */

    /* An odd-length Joliet name is not UCS-2. */
    p = fresh(NULL, 0);
    joliet_svd(25, '@');
    p = dir_start(25, B, NULL, 0);
    rec(p, 30, 1, 0, jname, 3, NULL, 0);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_CORRUPT);

    /* A supplementary descriptor with an odd block size is ignored. */
    p = fresh(NULL, 0);
    joliet_svd(25, 'C');
    put16b(img + 17 * B + 128, 512);
    mount_root();
    assert_int_equal(v.names, ISO9660_NAMES_PLAIN);
}

/* -------------------------------------------------------- multi-extent */

static void test_iso_multi_extent(void **state)
{
    static uint8_t want[3 * B], got[3 * B];
    iso9660_extent ext[4];
    size_t n = 0, i;
    uint8_t *p = fresh(NULL, 0);

    (void)state;
    /* Three parts: blocks 30 and 31 back to back, then 40. */
    p += rec(p, 30, B, 0x80, "BIG", 3, NULL, 0);
    p += rec(p, 31, B, 0x80, "BIG", 3, NULL, 0);
    rec(p, 40, 100, 0, "BIG", 3, NULL, 0);
    for (i = 0; i < 2 * B; i++)
        img[30 * B + i] = (uint8_t)(i * 7u + 1u);
    for (i = 0; i < 100; i++)
        img[40 * B + i] = (uint8_t)(i + 3u);
    memcpy(want, img + 30 * B, 2 * B);
    memcpy(want + 2 * B, img + 40 * B, 100);

    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    assert_int_equal(e.parts, 3);
    assert_int_equal(e.size, 2 * B + 100);
    assert_int_equal(iso9660_dir_next(&d, &e), 0);

    iso9660_dir_open_root(&v, &d);
    iso9660_dir_next(&d, &e);
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_OK);
    assert_int_equal(iso9660_file_read(&f, 0, e.size, got), ISO9660_OK);
    assert_memory_equal(got, want, e.size);
    /* Across the part boundary at 2 * B, unaligned. */
    assert_int_equal(iso9660_file_read(&f, 2 * B - 3, 10, got), ISO9660_OK);
    assert_memory_equal(got, want + 2 * B - 3, 10);
    assert_int_equal(iso9660_file_read(&f, 1, e.size, got),
                     ISO9660_ERR_RANGE);

    assert_int_equal(iso9660_file_extents(&v, &e, ext, 4, &n), ISO9660_OK);
    assert_int_equal(n, 2);             /* 30+31 coalesce */
    assert_int_equal(ext[0].vol_off, 30 * B);
    assert_int_equal(ext[0].len, 2 * B);
    assert_int_equal(ext[1].vol_off, 40 * B);
    assert_int_equal(ext[1].len, 100);
    assert_int_equal(iso9660_file_extents(&v, &e, ext, 1, &n),
                     ISO9660_ERR_NOSPACE);
    assert_int_equal(n, 2);
    assert_int_equal(iso9660_file_extents(&v, &e, NULL, 1, &n),
                     ISO9660_ERR_BAD_ARG);
    assert_int_equal(iso9660_file_extents(&v, &e, ext, 1, NULL),
                     ISO9660_ERR_BAD_ARG);

    /* A dirent claiming the wrong number of parts is not trusted. */
    e.parts = 2;
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_ERR_CORRUPT);
    e.parts = 3;
    e.size += 1;
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_ERR_CORRUPT);
    e.size -= 1;
    fail_off = 20 * B;
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_ERR_IO);
    fail_off = ~(uint64_t)0;
}

static void test_iso_multi_extent_edges(void **state)
{
    uint8_t *p;
    unsigned i;

    (void)state;

    /* The flag on the last record of the directory: parts missing. */
    p = fresh(NULL, 0);
    rec(p, 30, B, 0x80, "BIG", 3, NULL, 0);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_CORRUPT);

    /* A directory cannot be multi-extent. */
    p = fresh(NULL, 0);
    p += rec(p, 30, B, 0x82, "DIR", 3, NULL, 0);
    rec(p, 31, B, 0x02, "DIR", 3, NULL, 0);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_CORRUPT);

    /* More parts than the driver carries. */
    p = fresh(NULL, 0);
    for (i = 0; i < ISO9660_MAX_PARTS; i++)
        p += rec(p, 30, 1, 0x80, "M", 1, NULL, 0);
    rec(p, 30, 1, 0, "M", 1, NULL, 0);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_UNSUPPORTED);

    /* A skipped (associated) multi-extent file still owns its parts. */
    p = fresh(NULL, 0);
    p += rec(p, 30, B, 0x84, "R", 1, NULL, 0);
    p += rec(p, 31, B, 0x04, "R", 1, NULL, 0);
    rec(p, 32, 1, 0, "N", 1, NULL, 0);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "N");
    p = fresh(NULL, 0);
    rec(p, 30, B, 0x84, "R", 1, NULL, 0);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_CORRUPT);

    /* The parts straddle a directory block: padding, then the rest. */
    p = fresh(NULL, 0);
    put32b(img + 16 * B + 156 + 10, 2 * B);
    put32b(img + 20 * B + 10, 2 * B);
    rec(p, 30, B, 0x80, "S", 1, NULL, 0);
    rec(img + 21 * B, 31, 5, 0, "S", 1, NULL, 0);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    assert_int_equal(e.size, B + 5);
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_OK);

    /* Open re-reads parts that have since become malformed. */
    img[21 * B] = 10;
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_ERR_CORRUPT);
    img[21 * B] = 0;                    /* now the rest is all padding */
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_ERR_CORRUPT);
}

/* ------------------------------------------------------------ files */

static void test_iso_file_paths(void **state)
{
    static uint8_t got[3 * B];
    uint8_t *p = fresh(NULL, 0);
    unsigned i;

    (void)state;
    p += rec(p, 30, 3 * B - 5, 0, "F", 1, NULL, 0);
    p += rec(p, 30, 1, 0x02, "D", 1, NULL, 0);
    rec(p, 30, 1, 0, "I", 1, NULL, 0);
    p[26] = 1;                           /* interleaved */
    for (i = 0; i < 3 * B; i++)
        img[30 * B + i] = (uint8_t)(i ^ 0x5A);

    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_OK);
    /* Aligned whole blocks straight through, then a ragged tail. */
    assert_int_equal(iso9660_file_read(&f, 0, 3 * B - 5, got), ISO9660_OK);
    assert_memory_equal(got, img + 30 * B, 3 * B - 5);
    assert_int_equal(iso9660_file_read(&f, 0, 0, NULL), ISO9660_OK);
    assert_int_equal(iso9660_file_read(&f, 0, 1, NULL), ISO9660_ERR_BAD_ARG);
    assert_int_equal(iso9660_file_read(NULL, 0, 1, got), ISO9660_ERR_BAD_ARG);
    fail_off = 30 * B;
    assert_int_equal(iso9660_file_read(&f, 0, B, got), ISO9660_ERR_IO);
    /* The ragged tail went through the cache on the full read above; drop
     * it, or this read is answered without touching the device. */
    f.buf_off = ~(uint64_t)0;
    assert_int_equal(iso9660_file_read(&f, 2 * B + 1, 3, got), ISO9660_ERR_IO);
    fail_off = ~(uint64_t)0;
    assert_int_equal(iso9660_dir_open(&v, &d, &e), ISO9660_ERR_BAD_ARG);

    iso9660_dir_open_root(&v, &d);
    iso9660_dir_next(&d, &e);
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    assert_true(e.is_dir);
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_ERR_BAD_ARG);
    {
        size_t n;

        assert_int_equal(iso9660_file_extents(&v, &e, NULL, 0, &n),
                         ISO9660_ERR_BAD_ARG);
    }
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_ERR_UNSUPPORTED);
    e.parts = 0;
    e.compressed = 0;
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_ERR_BAD_ARG);

    assert_int_equal(iso9660_dir_open_root(NULL, &d), ISO9660_ERR_BAD_ARG);
    assert_int_equal(iso9660_dir_open(&v, NULL, &e), ISO9660_ERR_BAD_ARG);
    assert_int_equal(iso9660_dir_next(NULL, &e), ISO9660_ERR_BAD_ARG);
    assert_int_equal(iso9660_dir_next(&d, NULL), ISO9660_ERR_BAD_ARG);
    assert_int_equal(iso9660_file_open(NULL, &f, &e), ISO9660_ERR_BAD_ARG);
}

/* ---------------------------------------------------------- Rock Ridge */

static void test_iso_rr_basic(void **state)
{
    uint8_t su[256], root_su[16];
    size_t n = 0;
    uint8_t *p;

    (void)state;
    p = fresh(root_su, sp(root_su, 0));
    n += nm(su, 0, "Readme.md");
    n += px(su + n, 0100644);
    p += rec(p, 30, 1, 0, "README.MD;1", 11, su, n);
    n = nm(su, 0, "link");
    n += px(su + n, 0120777);
    p += rec(p, 30, 0, 0, "LINK.;1", 7, su, n);
    n = nm(su, 0, "rr_moved");
    n += simple(su + n, 'R', 'E');
    p += rec(p, 30, B, 0x02, "RR_MOVED", 8, su, n);
    n = simple(su, 'S', 'T');
    n += nm(su + n, 0, "ignored");
    p += rec(p, 30, 1, 0, "AFTERST.;1", 10, su, n);
    n = nm(su, 0, "packed");
    n += simple(su + n, 'Z', 'F');
    p += rec(p, 30, 1, 0, "PACKED.;1", 9, su, n);
    n = nm(su, 0x02, "");                /* CURRENT: not a name */
    p += rec(p, 30, 1, 0, "CUR.;1", 6, su, n);
    n = nm(su, 0, "first");
    n += nm(su + n, 0, "second");        /* no CONTINUE: the second wins */
    rec(p, 30, 1, 0, "TWO.;1", 6, su, n);

    mount_root();
    assert_int_equal(v.names, ISO9660_NAMES_RRIP);
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "Readme.md");
    assert_true(e.is_regular);
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "link");
    assert_false(e.is_regular);
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_ERR_SYMLINK);
    assert_int_equal(iso9660_dir_next(&d, &e), 1);   /* RE hidden */
    name_is(&e, "AFTERST");              /* ST ended the entries first */
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "packed");
    assert_int_equal(iso9660_file_open(&v, &f, &e), ISO9660_ERR_UNSUPPORTED);
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "CUR");
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "second");
    assert_int_equal(iso9660_dir_next(&d, &e), 0);
}

/* SP's LEN_SKP moves every System Use area after it. */
static void test_iso_rr_skip(void **state)
{
    uint8_t su[64], root_su[16];
    size_t n;
    uint8_t *p = fresh(root_su, sp(root_su, 4));

    (void)state;
    memset(su, 0xEE, 4);                 /* four bytes of someone else's */
    n = 4 + nm(su + 4, 0, "skipped");
    rec(p, 30, 1, 0, "S.;1", 4, su, n);
    mount_root();
    assert_int_equal(v.su_skip, 4);
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "skipped");
}

static void test_iso_rr_continuation(void **state)
{
    uint8_t su[128], root_su[16];
    uint8_t *p = fresh(root_su, sp(root_su, 0));
    uint8_t *area = img + 40 * B + 100;
    size_t n, a;

    (void)state;
    /* The name starts in the record and finishes in a continuation area. */
    n = nm(su, 0x01, "long-");
    n += ce(su + n, 40, 100, 0);
    a = nm(area, 0, "name.txt");
    a += px(area + a, 0100444);
    su[n - 28 + 20] = (uint8_t)a;        /* CE's length, little half */
    su[n - 28 + 27] = (uint8_t)a;        /* and big */
    rec(p, 30, 1, 0, "LONG.;1", 7, su, n);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "long-name.txt");

    /* The continuation area unreadable. */
    iso9660_dir_open_root(&v, &d);
    v.cache_off = ~(uint64_t)0;
    fail_off = 40 * B;
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_IO);
    fail_off = ~(uint64_t)0;

    /* A continuation area that points at itself, forever. */
    p = fresh(root_su, sp(root_su, 0));
    n = ce(su, 40, 0, 28);
    rec(p, 30, 1, 0, "LOOP.;1", 7, su, n);
    ce(img + 40 * B, 40, 0, 28);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_CORRUPT);

    /* One that claims to run past the end of its block. */
    p = fresh(root_su, sp(root_su, 0));
    n = ce(su, 40, B - 10, 20);
    rec(p, 30, 1, 0, "PAST.;1", 7, su, n);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_CORRUPT);
    p = fresh(root_su, sp(root_su, 0));
    n = ce(su, 40, B, 1);
    rec(p, 30, 1, 0, "OFF.;1", 6, su, n);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_CORRUPT);
}

static void test_iso_rr_malformed(void **state)
{
    uint8_t su[512], root_su[16];
    uint8_t *p;
    size_t n;
    unsigned i;

    (void)state;
    /* Each of these is one bad entry; each must be CORRUPT, not a guess. */
    for (i = 0; i < 6; i++) {
        p = fresh(root_su, sp(root_su, 0));
        memset(su, 0, sizeof su);
        switch (i) {
        case 0: n = simple(su, 'N', 'M'); su[2] = 3; break;     /* < 4 */
        case 1: n = nm(su, 0, "abc"); su[2] = 60; break;        /* overrun */
        case 2: n = simple(su, 'C', 'E'); break;                /* short CE */
        case 3: n = simple(su, 'P', 'X'); break;                /* short PX */
        case 4: n = simple(su, 'C', 'L'); break;                /* short CL */
        default: n = simple(su, 'N', 'M'); break;               /* short NM */
        }
        rec(p, 30, 1, 0, "BAD.;1", 6, su, n);
        mount_root();
        if (iso9660_dir_next(&d, &e) != ISO9660_ERR_CORRUPT)
            fail_msg("malformed entry %u was not refused", i);
    }

    /* A name that grows past 255 bytes through CONTINUE entries. */
    p = fresh(root_su, sp(root_su, 0));
    {
        char chunk[101];

        memset(chunk, 'n', 100);
        chunk[100] = '\0';
        n = nm(su, 0x01, chunk);
        n += nm(su + n, 0x01, chunk);
        n += nm(su + n, 0x00, chunk);
    }
    /* Too long for one record, so the tail goes in a continuation area. */
    n = nm(su, 0x01, "x");
    n += ce(su + n, 40, 0, 0);
    {
        uint8_t *a = img + 40 * B;
        size_t k = 0;
        char chunk[101];

        memset(chunk, 'n', 100);
        chunk[100] = '\0';
        k += nm(a + k, 0x01, chunk);
        k += nm(a + k, 0x01, chunk);
        k += nm(a + k, 0x00, chunk);
        su[n - 28 + 20] = (uint8_t)k;
        su[n - 28 + 21] = (uint8_t)(k >> 8);
    }
    rec(p, 30, 1, 0, "HUGE.;1", 7, su, n);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_CORRUPT);

    /* Zero padding inside an area ends it quietly. */
    p = fresh(root_su, sp(root_su, 0));
    n = nm(su, 0, "padded");
    su[n] = 0;
    su[n + 1] = 0;
    su[n + 2] = 0;
    su[n + 3] = 0;
    rec(p, 30, 1, 0, "PAD.;1", 6, su, n + 4);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "padded");
}

/* Deep directory relocation (RRIP 4.1.5): a CL record stands in for the
 * moved directory, whose own "." record gives its extent and length. */
static void test_iso_rr_relocation(void **state)
{
    uint8_t su[64], root_su[16];
    uint8_t *p;
    size_t n;

    (void)state;
    p = fresh(root_su, sp(root_su, 0));
    n = nm(su, 0, "deep");
    n += cl(su + n, 45);
    rec(p, 0, 0, 0, "DEEP.;1", 7, su, n);
    p = dir_start(45, B, NULL, 0);
    rec(p, 46, 3, 0, "LEAF.;1", 7, NULL, 0);
    memcpy(img + 46 * B, "abc", 3);
    mount_root();
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "deep");
    assert_true(e.is_dir);
    assert_int_equal(e.lba, 45);
    assert_int_equal(e.size, B);
    assert_int_equal(iso9660_dir_open(&v, &d, &e), ISO9660_OK);
    assert_int_equal(iso9660_dir_next(&d, &e), 1);
    name_is(&e, "LEAF");

    /* A CL target that is not a directory's "." record. */
    iso9660_dir_open_root(&v, &d);
    img[45 * B + 33] = 7;
    v.cache_off = ~(uint64_t)0;
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_CORRUPT);
    img[45 * B + 33] = 0;
    iso9660_dir_open_root(&v, &d);
    v.cache_off = ~(uint64_t)0;
    fail_off = 45 * B;
    assert_int_equal(iso9660_dir_next(&d, &e), ISO9660_ERR_IO);
}

static void test_iso_append_record(void **state)
{
    uint8_t *p;
    uint8_t out[B];
    uint32_t blk = 0;
    int found = 0, count = 0;

    (void)state;
    /* A one-block root at 20 with two files, in sorted order. */
    p = fresh(NULL, 0);
    p += rec(p, 30, 100, 0, "AAA.TXT;1", 9, NULL, 0);
    p += rec(p, 31, 200, 0, "BBB.TXT;1", 9, NULL, 0);
    (void)p;
    assert_int_equal(iso9660_mount(&v, img_read, NULL), ISO9660_OK);

    /* Append a file whose name sorts last; it lands in the block's slack. */
    assert_int_equal(iso9660_append_record(&v, 20, B, "_Z.SYS;1",
                                           500, 4096u, out, &blk), ISO9660_OK);
    assert_int_equal(blk, 20);

    /* Overlay the edited block and read it back: the new file is there, with
     * its extent, and the two existing files still are. */
    memcpy(img + (size_t)20 * B, out, B);
    v.cache_off = ~(uint64_t)0;
    assert_int_equal(iso9660_dir_open_root(&v, &d), ISO9660_OK);
    while (iso9660_dir_next(&d, &e) == 1) {
        count++;
        if (e.name_len == 6u && e.name[0] == '_' && e.name[1] == 'Z') {
            found = 1;
            assert_int_equal(e.lba, 500);
            assert_int_equal((int)e.size, 4096);
        }
    }
    assert_int_equal(count, 3);
    assert_true(found);

    /* A block with no room refuses. Fill the root to within a few bytes. */
    p = fresh(NULL, 0);
    filler(p, (size_t)(img + 21u * B) - (size_t)p - 8u);
    assert_int_equal(iso9660_mount(&v, img_read, NULL), ISO9660_OK);
    assert_int_equal(iso9660_append_record(&v, 20, B, "_Z.SYS;1",
                                           500, 4096u, out, &blk),
                     ISO9660_ERR_NOSPACE);
}

/* A directory whose recorded data length is not a multiple of the block size:
 * whole blocks of records, then a last block only partly filled. Windows XP's
 * \i386 is exactly this shape (263642 bytes: 128 full blocks and a 129th with
 * 1498 bytes of records and 550 free), and the driver-injection route appends
 * _FALCON.SYS into that free space. A floor of the data length would pick the
 * last full block, not the partly-filled one, and overflow. */
static void test_iso_append_record_unaligned(void **state)
{
    uint8_t *q;
    uint8_t out[B];
    uint32_t blk = 0;
    size_t used21;
    const uint8_t *r;

    (void)state;
    (void)fresh(NULL, 0);
    q = img + 21u * B;
    q += rec(q, 40, 10, 0, "AAA.TXT;1", 9, NULL, 0);
    q += rec(q, 41, 20, 0, "_DEFAULT.PI_;1", 14, NULL, 0);
    used21 = (size_t)(q - (img + 21u * B));
    assert_int_equal(iso9660_mount(&v, img_read, NULL), ISO9660_OK);

    assert_int_equal(iso9660_append_record(&v, 20, (uint32_t)(B + used21),
                                           "_FALCON.SYS;1", 777, 21020u,
                                           out, &blk), ISO9660_OK);
    assert_int_equal(blk, 21u);

    /* It sits right after the block's existing records, naming its extent. */
    r = out + used21;
    assert_int_equal(r[0], 46u);              /* 33 + 13-char name, even */
    assert_int_equal((uint32_t)r[2] | ((uint32_t)r[3] << 8) |
                     ((uint32_t)r[4] << 16) | ((uint32_t)r[5] << 24), 777u);
    assert_int_equal((uint32_t)r[10] | ((uint32_t)r[11] << 8) |
                     ((uint32_t)r[12] << 16) | ((uint32_t)r[13] << 24), 21020u);
    assert_int_equal(r[32], 13u);
    assert_memory_equal(r + 33, "_FALCON.SYS;1", 13u);
}

int run_iso9660_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_iso_mount_plain),
        cmocka_unit_test(test_iso_mount_refusals),
        cmocka_unit_test(test_iso_record_shapes),
        cmocka_unit_test(test_iso_names),
        cmocka_unit_test(test_iso_joliet),
        cmocka_unit_test(test_iso_multi_extent),
        cmocka_unit_test(test_iso_multi_extent_edges),
        cmocka_unit_test(test_iso_append_record),
        cmocka_unit_test(test_iso_append_record_unaligned),
        cmocka_unit_test(test_iso_file_paths),
        cmocka_unit_test(test_iso_rr_basic),
        cmocka_unit_test(test_iso_rr_skip),
        cmocka_unit_test(test_iso_rr_continuation),
        cmocka_unit_test(test_iso_rr_malformed),
        cmocka_unit_test(test_iso_rr_relocation),
    };

    return cmocka_run_group_tests_name("iso9660", tests, NULL, NULL);
}
