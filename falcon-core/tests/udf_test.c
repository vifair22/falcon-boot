/* falcon-core/src/fs/udf.c on images built here, descriptor by descriptor.
 *
 * media_fs_test.c proves the driver agrees with mkudffs and the kernel's
 * writer; this file reaches the structures those never produce and the ones
 * they produce only by accident: allocation extent chains, unrecorded
 * extents, Extended File Entries, a descriptor sequence moved by a pointer,
 * and every way a tag can be wrong.
 *
 * The builder writes a minimal volume: the recognition sequence at block 16,
 * the anchor at 256, the descriptor sequence at 32, and a partition starting
 * at block 40 holding the file set, the root and whatever the test adds.
 * Every descriptor gets a real tag (checksum, location, CRC), so each test
 * that breaks one thing breaks only that thing.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <fs/udf.h>

#define BS        UDF_BLOCK
#define NBLK      300u
#define PSTART    40u
#define PLEN      200u
#define VDS       32u

static uint8_t img[NBLK * BS];
static uint64_t fail_off = ~(uint64_t)0;

static int img_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    if (off + len > sizeof img || off + len > fail_off)
        return -1;
    memcpy(buf, img + off, (size_t)len);
    return 0;
}

static void le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
    le16(p, (uint16_t)v);
    le16(p + 2, (uint16_t)(v >> 16));
}

static void le64(uint8_t *p, uint64_t v)
{
    le32(p, (uint32_t)v);
    le32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t crc16(const uint8_t *p, size_t n)
{
    unsigned crc = 0;
    size_t i;
    int b;

    for (i = 0; i < n; i++) {
        crc ^= (unsigned)p[i] << 8;
        for (b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? ((crc << 1) ^ 0x1021u) : (crc << 1);
    }
    return (uint16_t)crc;
}

/* Finishes a descriptor tag over `body` bytes after the 16-byte tag. */
static void tag(uint8_t *d, uint16_t id, uint32_t loc, size_t body)
{
    uint8_t sum = 0;
    int i;

    le16(d, id);
    le16(d + 2, 2);
    le16(d + 6, 1);
    le16(d + 10, (uint16_t)body);
    le32(d + 12, loc);
    le16(d + 8, crc16(d + 16, body));
    d[4] = 0;
    for (i = 0; i < 16; i++) {
        if (i != 4)
            sum = (uint8_t)(sum + d[i]);
    }
    d[4] = sum;
}

static uint8_t *blk(unsigned b)
{
    return img + (size_t)b * BS;
}

static uint8_t *pblk(unsigned lbn)
{
    return blk(PSTART + lbn);
}

static void dstring(uint8_t *d, unsigned field, const char *s, int wide)
{
    size_t n = strlen(s), i;

    memset(d, 0, field);
    if (n == 0)
        return;
    d[0] = wide ? 16 : 8;
    for (i = 0; i < n; i++) {
        if (wide) {
            d[1 + 2 * i] = 0;
            d[2 + 2 * i] = (uint8_t)s[i];
        } else {
            d[1 + i] = (uint8_t)s[i];
        }
    }
    d[field - 1] = (uint8_t)(1 + (wide ? 2 * n : n));
}

/* ---- the volume ---- */

static void vrs(void)
{
    static const char *const ids[] = { "BEA01", "NSR02", "TEA01" };
    int i;

    for (i = 0; i < 3; i++) {
        memset(blk(16u + (unsigned)i), 0, BS);
        memcpy(blk(16u + (unsigned)i) + 1, ids[i], 5);
    }
}

static void anchor(uint32_t loc, uint32_t len)
{
    uint8_t *a = blk(256);

    memset(a, 0, BS);
    le32(a + 16, len);
    le32(a + 20, loc);
    tag(a, 2, 256, 496);
}

static void pvd(unsigned b, const char *label)
{
    uint8_t *d = blk(b);

    memset(d, 0, BS);
    dstring(d + 24, 32, label, 0);
    tag(d, 1, b, 496);
}

static void pd(unsigned b, uint16_t num, uint32_t start, uint32_t len)
{
    uint8_t *d = blk(b);

    memset(d, 0, BS);
    le16(d + 22, num);
    le32(d + 188, start);
    le32(d + 192, len);
    tag(d, 5, b, 496);
}

static void lvd(unsigned b, const char *label, int wide)
{
    uint8_t *d = blk(b);

    memset(d, 0, BS);
    dstring(d + 84, 128, label, wide);
    le32(d + 212, BS);
    le32(d + 248, BS);          /* FSD long_ad: length */
    le32(d + 252, 0);           /* lbn 0 */
    le32(d + 264, 6);
    le32(d + 268, 1);
    d[440] = 1;
    d[441] = 6;
    le16(d + 442, 1);
    le16(d + 444, 0);           /* partition number 0 */
    tag(d, 6, b, 440 + 6 - 16);
}

static void td(unsigned b)
{
    memset(blk(b), 0, BS);
    tag(blk(b), 8, b, 496);
}

static void fsd(uint32_t root_lbn)
{
    uint8_t *d = pblk(0);

    memset(d, 0, BS);
    le32(d + 400, BS);
    le32(d + 404, root_lbn);
    tag(d, 256, 0, 496);
}

/* A File Entry at lbn: type, size, allocation descriptor kind (0 short,
 * 1 long, 3 embedded), and the descriptor area. efe picks the Extended File
 * Entry layout. */
static void fe(unsigned lbn, uint8_t ftype, uint64_t size, unsigned kind,
               const uint8_t *ad, uint32_t l_ad, int efe)
{
    uint8_t *d = pblk(lbn);
    uint32_t base = efe ? 216u : 176u;

    memset(d, 0, BS);
    le16(d + 20, 4);
    d[27] = ftype;
    le16(d + 34, (uint16_t)kind);
    le64(d + 56, size);
    le32(d + (efe ? 208 : 168), 0);
    le32(d + (efe ? 212 : 172), l_ad);
    memcpy(d + base, ad, l_ad);
    tag(d, efe ? 266 : 261, lbn, base + l_ad - 16);
}

static size_t short_ad(uint8_t *p, uint32_t len, unsigned type, uint32_t lbn)
{
    le32(p, len | ((uint32_t)type << 30));
    le32(p + 4, lbn);
    return 8;
}

static size_t long_ad(uint8_t *p, uint32_t len, uint32_t lbn, uint16_t part)
{
    memset(p, 0, 16);
    le32(p, len);
    le32(p + 4, lbn);
    le16(p + 8, part);
    return 16;
}

/* One File Identifier Descriptor at p; returns its padded length. */
static size_t fid(uint8_t *p, uint8_t fc, uint32_t icb, const char *name,
                  int wide)
{
    size_t nl = strlen(name), i;
    uint8_t l_fi = (uint8_t)(nl == 0 ? 0 : 1 + (wide ? 2 * nl : nl));
    size_t total = (38u + l_fi + 3u) & ~(size_t)3u;

    memset(p, 0, total);
    le16(p + 16, 1);
    p[18] = fc;
    p[19] = l_fi;
    long_ad(p + 20, BS, icb, 0);
    if (nl) {
        p[38] = wide ? 16 : 8;
        for (i = 0; i < nl; i++) {
            if (wide) {
                p[39 + 2 * i] = 0;
                p[40 + 2 * i] = (uint8_t)name[i];
            } else {
                p[39 + i] = (uint8_t)name[i];
            }
        }
    }
    tag(p, 257, 0, total - 16);
    return total;
}

/* The standard image: root at lbn 1 whose FIDs live at lbn 2, holding
 *   hello.txt  5 bytes at lbn 10, short_ad
 *   sub/       a directory at lbn 4, its FIDs at lbn 5, holding "w" (16-bit
 *              name) at lbn 6: 3 bytes, Extended File Entry, long_ad, data
 *              at lbn 11
 *   tiny       embedded, 4 bytes
 *   link       a symlink
 * Returns the root directory's FID length, for tests that extend it. */
static uint8_t g_dir[BS];
static size_t g_dir_len;

static void root_fids(void)
{
    uint8_t ad[8];

    memcpy(pblk(2), g_dir, g_dir_len);
    short_ad(ad, (uint32_t)g_dir_len, 0, 2);
    fe(1, 4, g_dir_len, 0, ad, 8, 0);
}

static void fresh(void)
{
    uint8_t ad[16];
    uint8_t sub[256];
    size_t n = 0;

    memset(img, 0, sizeof img);
    fail_off = ~(uint64_t)0;
    vrs();
    anchor(VDS, 4 * BS);
    pvd(VDS, "PVD_LABEL");
    pd(VDS + 1, 0, PSTART, PLEN);
    lvd(VDS + 2, "SYNTH_UDF", 0);
    td(VDS + 3);
    fsd(1);

    g_dir_len = 0;
    g_dir_len += fid(g_dir + g_dir_len, 0x0A, 1, "", 0);   /* parent */
    g_dir_len += fid(g_dir + g_dir_len, 0, 3, "hello.txt", 0);
    g_dir_len += fid(g_dir + g_dir_len, 0x02, 4, "sub", 0);
    g_dir_len += fid(g_dir + g_dir_len, 0, 7, "tiny", 0);
    g_dir_len += fid(g_dir + g_dir_len, 0, 8, "link", 0);
    root_fids();

    short_ad(ad, 5, 0, 10);
    fe(3, 5, 5, 0, ad, 8, 0);
    memcpy(pblk(10), "hello", 5);

    n += fid(sub + n, 0x0A, 1, "", 0);
    n += fid(sub + n, 0, 6, "w", 1);
    memcpy(pblk(5), sub, n);
    short_ad(ad, (uint32_t)n, 0, 5);
    fe(4, 4, n, 0, ad, 8, 0);
    long_ad(ad, 3, 11, 0);
    fe(6, 5, 3, 1, ad, 16, 1);
    memcpy(pblk(11), "abc", 3);

    fe(7, 5, 4, 3, (const uint8_t *)"TINY", 4, 0);
    short_ad(ad, 6, 0, 12);
    fe(8, 12, 6, 0, ad, 8, 0);
}

static udf_vol v;
static udf_dir d;
static udf_file f;
static udf_dirent e;

static void name_is(const udf_dirent *x, const char *want)
{
    char got[UDF_NAME_MAX + 1];

    ucs2_to_ascii(x->name, got, sizeof got);
    assert_string_equal(got, want);
}

static void mount_root(void)
{
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_OK);
    assert_int_equal(udf_dir_open_root(&v, &d), UDF_OK);
}

static void next_named(const char *want)
{
    assert_int_equal(udf_dir_next(&d, &e), 1);
    name_is(&e, want);
}

/* ------------------------------------------------------------- happy */

static void test_udf_basic(void **state)
{
    char buf[8];
    udf_dir sd;

    (void)state;
    fresh();
    assert_int_equal(udf_probe(img_read, NULL), UDF_OK);
    mount_root();
    assert_string_equal(v.label, "SYNTH_UDF");

    next_named("hello.txt");
    assert_true(e.is_regular);
    assert_int_equal(e.size, 5);
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_OK);
    assert_int_equal(udf_file_read(&f, 0, 5, buf), UDF_OK);
    assert_memory_equal(buf, "hello", 5);
    assert_int_equal(udf_file_read(&f, 1, 5, buf), UDF_ERR_RANGE);

    next_named("sub");
    assert_true(e.is_dir);
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_BAD_ARG);
    assert_int_equal(udf_dir_open(&v, &sd, &e), UDF_OK);
    assert_int_equal(udf_dir_next(&sd, &e), 1);
    name_is(&e, "w");
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_OK);
    assert_int_equal(udf_file_read(&f, 0, 3, buf), UDF_OK);
    assert_memory_equal(buf, "abc", 3);
    assert_int_equal(udf_dir_open(&v, &sd, &e), UDF_ERR_BAD_ARG);

    next_named("tiny");
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_OK);
    assert_int_equal(udf_file_read(&f, 1, 3, buf), UDF_OK);
    assert_memory_equal(buf, "INY", 3);
    {
        udf_extent x[2];
        size_t n;

        assert_int_equal(udf_file_extents(&v, &e, x, 2, &n),
                         UDF_ERR_UNSUPPORTED);
    }

    next_named("link");
    assert_false(e.is_regular);
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_SYMLINK);
    assert_int_equal(udf_dir_next(&d, &e), 0);
}

/* The logical volume identifier is the label; UCS-2 is narrowed; an empty
 * one falls back to the primary descriptor's. */
static void test_udf_labels(void **state)
{
    (void)state;
    fresh();
    lvd(VDS + 2, "WIDE", 1);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_OK);
    assert_string_equal(v.label, "WIDE");

    fresh();
    lvd(VDS + 2, "", 0);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_OK);
    assert_string_equal(v.label, "PVD_LABEL");

    /* A dstring whose used length overruns its field yields no label. */
    fresh();
    lvd(VDS + 2, "", 0);
    blk(VDS)[24 + 31] = 40;
    tag(blk(VDS), 1, VDS, 496);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_OK);
    assert_string_equal(v.label, "");
}

/* ------------------------------------------------------------ volume */

static void test_udf_recognition(void **state)
{
    (void)state;
    fresh();
    memcpy(blk(16) + 1, "XXXXX", 5);     /* nothing we know: stop */
    assert_int_equal(udf_probe(img_read, NULL), UDF_ERR_BAD_SUPER);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_BAD_SUPER);

    fresh();
    memcpy(blk(16) + 1, "CD001", 5);     /* ISO9660 in front, then no BEA */
    assert_int_equal(udf_probe(img_read, NULL), UDF_ERR_BAD_SUPER);

    fresh();
    memcpy(blk(17) + 1, "TEA01", 5);     /* extended area with no NSR */
    assert_int_equal(udf_probe(img_read, NULL), UDF_ERR_BAD_SUPER);

    fresh();
    memcpy(blk(17) + 1, "NSR03", 5);
    assert_int_equal(udf_probe(img_read, NULL), UDF_OK);

    fresh();
    fail_off = 16 * BS;
    assert_int_equal(udf_probe(img_read, NULL), UDF_ERR_IO);
    fail_off = 256 * BS;
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_IO);

    assert_int_equal(udf_probe(NULL, NULL), UDF_ERR_BAD_ARG);
    assert_int_equal(udf_mount(NULL, img_read, NULL), UDF_ERR_BAD_ARG);
}

static void test_udf_anchor_tags(void **state)
{
    (void)state;
    fresh();
    blk(256)[4] ^= 1;                    /* checksum */
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_BAD_SUPER);

    fresh();
    blk(256)[100] ^= 1;                  /* CRC */
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_BAD_SUPER);

    fresh();
    le32(blk(256) + 12, 257);            /* recorded elsewhere */
    tag(blk(256), 2, 257, 496);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_BAD_SUPER);

    fresh();
    tag(blk(256), 2, 256, 3000);         /* CRC length past the block */
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_BAD_SUPER);
}

static void test_udf_sequence(void **state)
{
    uint8_t *b;

    (void)state;
    fresh();
    blk(VDS + 1)[4] ^= 1;
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_CORRUPT);

    fresh();
    fail_off = (uint64_t)(VDS + 1) * BS;
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_IO);

    /* Block size, map count, map type, map table length. */
    fresh();
    le32(blk(VDS + 2) + 212, 512);
    tag(blk(VDS + 2), 6, VDS + 2, 430);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_UNSUPPORTED);
    fresh();
    le32(blk(VDS + 2) + 268, 2);
    tag(blk(VDS + 2), 6, VDS + 2, 430);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_UNSUPPORTED);
    fresh();
    blk(VDS + 2)[440] = 2;
    tag(blk(VDS + 2), 6, VDS + 2, 430);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_UNSUPPORTED);
    fresh();
    le32(blk(VDS + 2) + 264, BS);
    tag(blk(VDS + 2), 6, VDS + 2, 430);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_UNSUPPORTED);

    /* The FSD's long_ad naming a second partition. */
    fresh();
    le16(blk(VDS + 2) + 256, 1);
    tag(blk(VDS + 2), 6, VDS + 2, 430);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_CORRUPT);

    /* No partition descriptor; a map naming another partition; an empty
     * partition. */
    fresh();
    td(VDS + 1);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_CORRUPT);
    fresh();
    pd(VDS + 1, 3, PSTART, PLEN);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_CORRUPT);
    fresh();
    pd(VDS + 1, 0, PSTART, 0);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_CORRUPT);

    /* The sequence may end at its extent's end rather than at a TD. */
    fresh();
    anchor(VDS, 3 * BS);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_OK);

    /* A pointer moves the sequence: VDS+0 points at a copy at 60. */
    fresh();
    memcpy(blk(60), blk(VDS), 4 * BS);
    pvd(60, "MOVED");
    pd(61, 0, PSTART, PLEN);
    lvd(62, "MOVED", 0);
    td(63);
    b = blk(VDS);
    memset(b, 0, BS);
    le32(b + 20, 4 * BS);
    le32(b + 24, 60);
    tag(b, 3, VDS, 496);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_OK);
    assert_string_equal(v.label, "MOVED");

    /* And a pointer to itself is a loop. */
    le32(b + 24, VDS);
    tag(b, 3, VDS, 496);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_CORRUPT);
}

static void test_udf_file_set(void **state)
{
    (void)state;
    fresh();
    pblk(0)[4] ^= 1;
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_CORRUPT);

    fresh();
    le16(pblk(0) + 408, 2);
    tag(pblk(0), 256, 0, 496);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_UNSUPPORTED);

    fresh();
    le32(blk(VDS + 2) + 252, PLEN + 5);  /* FSD outside the partition */
    tag(blk(VDS + 2), 6, VDS + 2, 430);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_CORRUPT);

    fresh();
    fail_off = (uint64_t)PSTART * BS;
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_ERR_IO);
}

/* ----------------------------------------------------------- entries */

static void open_hello(void)
{
    mount_root();
    next_named("hello.txt");
}

static void test_udf_entry_refusals(void **state)
{
    uint8_t ad[16];

    (void)state;
    /* Strategy 4096. */
    fresh();
    le16(pblk(3) + 20, 4096);
    tag(pblk(3), 261, 3, 176 + 8 - 16);
    open_hello();
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_UNSUPPORTED);

    /* An extended attribute length past the block. */
    fresh();
    le32(pblk(3) + 168, BS);
    tag(pblk(3), 261, 3, 176 + 8 - 16);
    open_hello();
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_CORRUPT);

    /* Embedded data larger than the area it is embedded in. */
    fresh();
    fe(3, 5, 50, 3, (const uint8_t *)"abcd", 4, 0);
    open_hello();
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_CORRUPT);

    /* extended_ad. */
    fresh();
    short_ad(ad, 5, 0, 10);
    fe(3, 5, 5, 2, ad, 8, 0);
    open_hello();
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_UNSUPPORTED);

    /* Extents that do not cover the file. */
    fresh();
    short_ad(ad, 5, 0, 10);
    fe(3, 5, 9000, 0, ad, 8, 0);
    open_hello();
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_CORRUPT);

    /* A long_ad into a second partition. */
    fresh();
    long_ad(ad, 5, 10, 1);
    fe(3, 5, 5, 1, ad, 16, 0);
    open_hello();
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_UNSUPPORTED);

    /* Not a File Entry at all. */
    fresh();
    tag(pblk(3), 262, 3, 100);
    mount_root();
    assert_int_equal(udf_dir_next(&d, &e), UDF_ERR_CORRUPT);

    /* A device node: a File Entry, but no byte stream. */
    fresh();
    short_ad(ad, 5, 0, 10);
    fe(3, 6, 5, 0, ad, 8, 0);
    open_hello();
    assert_false(e.is_regular);
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_SYMLINK);

    fresh();
    open_hello();
    fail_off = (uint64_t)(PSTART + 3) * BS;
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_IO);

    assert_int_equal(udf_file_open(NULL, &f, &e), UDF_ERR_BAD_ARG);
    assert_int_equal(udf_dir_open_root(NULL, &d), UDF_ERR_BAD_ARG);
    assert_int_equal(udf_dir_open(&v, NULL, &e), UDF_ERR_BAD_ARG);
    assert_int_equal(udf_dir_next(NULL, &e), UDF_ERR_BAD_ARG);
}

/* A file whose descriptors spill into an allocation extent chain, with an
 * unrecorded extent in the middle: reads must stitch the three recorded
 * pieces and zeros, and the extent map must decline, since zeros have no
 * physical run to report. */
static void test_udf_chains_and_holes(void **state)
{
    static uint8_t got[4 * BS];
    uint8_t ad[64], *aed;
    size_t n = 0;
    unsigned i;

    (void)state;
    fresh();
    n += short_ad(ad + n, BS, 0, 20);
    n += short_ad(ad + n, BS, 3, 30);           /* continue at lbn 30 */
    fe(3, 5, 3 * BS + 10, 0, ad, (uint32_t)n, 0);
    aed = pblk(30);
    memset(aed, 0, BS);
    n = 0;
    n += short_ad(aed + 24 + n, BS, 1, 0);      /* allocated, not recorded */
    n += short_ad(aed + 24 + n, BS + 10, 0, 21);
    le32(aed + 20, (uint32_t)n);
    tag(aed, 258, 30, 24 + n - 16);
    for (i = 0; i < 2 * BS; i++)
        pblk(20)[i] = (uint8_t)(i * 3u + 1u);

    open_hello();
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_OK);
    assert_int_equal(f.nextents, 3);
    assert_int_equal(udf_file_read(&f, 0, 3 * BS + 10, got), UDF_OK);
    assert_memory_equal(got, pblk(20), BS);
    for (i = 0; i < BS; i++)
        assert_int_equal(got[BS + i], 0);
    assert_memory_equal(got + 2 * BS, pblk(21), BS + 10);
    {
        udf_extent x[4];
        size_t c;

        assert_int_equal(udf_file_extents(&v, &e, x, 4, &c),
                         UDF_ERR_UNSUPPORTED);
    }

    /* The chain block with a wrong tag, an oversized area, and a loop. */
    tag(aed, 257, 30, 24 + n - 16);
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_CORRUPT);
    le32(aed + 20, BS);
    tag(aed, 258, 30, 100);
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_CORRUPT);
    short_ad(aed + 24, BS, 3, 30);
    le32(aed + 20, 8);
    tag(aed, 258, 30, 16);
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_CORRUPT);
    fail_off = (uint64_t)(PSTART + 30) * BS;
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_IO);
}

static void test_udf_extent_limits(void **state)
{
    uint8_t ad[BS];
    size_t n = 0;
    unsigned i;
    udf_extent x[4];
    size_t c;

    (void)state;
    /* More descriptors than a file may have. */
    fresh();
    for (i = 0; i <= UDF_MAX_EXTENTS; i++)
        n += short_ad(ad + n, BS, 0, 20);
    fe(3, 5, BS, 0, ad, (uint32_t)n, 0);
    open_hello();
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_ERR_UNSUPPORTED);

    /* Adjacent extents coalesce; a gap does not. */
    fresh();
    n = 0;
    n += short_ad(ad + n, BS, 0, 20);
    n += short_ad(ad + n, BS, 0, 21);
    n += short_ad(ad + n, 100, 0, 40);
    fe(3, 5, 2 * BS + 100, 0, ad, (uint32_t)n, 0);
    open_hello();
    assert_int_equal(udf_file_extents(&v, &e, x, 4, &c), UDF_OK);
    assert_int_equal(c, 2);
    assert_int_equal(x[0].vol_off, (uint64_t)(PSTART + 20) * BS);
    assert_int_equal(x[0].len, 2 * BS);
    assert_int_equal(x[1].len, 100);
    assert_int_equal(udf_file_extents(&v, &e, x, 1, &c), UDF_ERR_NOSPACE);
    assert_int_equal(c, 2);
    assert_int_equal(udf_file_extents(&v, &e, NULL, 1, &c), UDF_ERR_BAD_ARG);
    assert_int_equal(udf_file_extents(&v, &e, x, 1, NULL), UDF_ERR_BAD_ARG);

    /* An extent running out of the partition is refused at read time. */
    fresh();
    n = short_ad(ad, 3 * BS, 0, PLEN - 1);
    fe(3, 5, 3 * BS, 0, ad, (uint32_t)n, 0);
    open_hello();
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_OK);
    {
        static uint8_t buf[3 * BS];

        assert_int_equal(udf_file_read(&f, 0, 3 * BS, buf), UDF_ERR_CORRUPT);
        assert_int_equal(udf_file_read(&f, 0, 0, NULL), UDF_OK);
        assert_int_equal(udf_file_read(&f, 0, 1, NULL), UDF_ERR_BAD_ARG);
    }

    /* Reads through the cache and the direct path both fail cleanly. */
    fresh();
    n = short_ad(ad, 2 * BS, 0, 20);
    fe(3, 5, 2 * BS, 0, ad, (uint32_t)n, 0);
    open_hello();
    assert_int_equal(udf_file_open(&v, &f, &e), UDF_OK);
    fail_off = (uint64_t)(PSTART + 20) * BS;
    {
        uint8_t buf[BS];

        assert_int_equal(udf_file_read(&f, 0, BS, buf), UDF_ERR_IO);
        assert_int_equal(udf_file_read(&f, 5, 10, buf), UDF_ERR_IO);
    }
}

/* ------------------------------------------------------ identifiers */

/* Replaces the root's FID stream with one entry built by the caller and
 * re-points the root at it. */
static void root_with(const uint8_t *fids, size_t len)
{
    memcpy(g_dir, fids, len);
    g_dir_len = len;
    root_fids();
}

static void test_udf_fid_refusals(void **state)
{
    uint8_t x[128];
    size_t n;
    int i;

    (void)state;
    for (i = 0; i < 9; i++) {
        fresh();
        n = fid(x, 0, 3, "hello.txt", 0);
        switch (i) {
        case 0: x[4] ^= 1; break;                       /* checksum */
        case 1: le16(x, 258); tag(x, 258, 0, n - 16); break; /* not a FID */
        case 2: tag(x, 257, 0, n - 20); break;          /* CRC length */
        case 3: x[40] ^= 1; break;                      /* CRC */
        case 4: x[19] = 0; tag(x, 257, 0, 38 - 16); break;  /* no name */
        case 5: x[38] = 7; tag(x, 257, 0, n - 16); break;   /* comp id */
        case 6: x[38] = 16; tag(x, 257, 0, n - 16); break;  /* odd UCS-2 */
        case 7: x[19] = 200; tag(x, 257, 0, n - 16); break; /* runs off */
        default: n = 20; break;                         /* short stream */
        }
        root_with(x, n);
        mount_root();
        if (udf_dir_next(&d, &e) != UDF_ERR_CORRUPT)
            fail_msg("FID refusal %d was not refused", i);
    }

    /* A second partition, a child that is not what its FID says. */
    fresh();
    n = fid(x, 0, 3, "hello.txt", 0);
    le16(x + 28, 1);
    tag(x, 257, 0, n - 16);
    root_with(x, n);
    mount_root();
    assert_int_equal(udf_dir_next(&d, &e), UDF_ERR_UNSUPPORTED);

    fresh();
    n = fid(x, 0x02, 3, "hello.txt", 0);         /* says dir, FE says file */
    root_with(x, n);
    mount_root();
    assert_int_equal(udf_dir_next(&d, &e), UDF_ERR_CORRUPT);

    /* Deleted entries are skipped. */
    fresh();
    n = fid(x, 0x04, 3, "gone", 0);
    n += fid(x + n, 0, 3, "kept", 0);
    root_with(x, n);
    mount_root();
    next_named("kept");

    /* An implementation use area is skipped over and covered by the CRC. */
    fresh();
    memset(x, 0, sizeof x);
    le16(x + 16, 1);
    x[19] = 3;
    long_ad(x + 20, BS, 3, 0);
    le16(x + 36, 10);
    memset(x + 38, 0xA5, 10);
    x[48] = 8;
    x[49] = 'i';
    x[50] = 'u';
    n = (38 + 10 + 3 + 3) & ~3u;
    tag(x, 257, 0, 38 + 10 + 3 - 16);
    root_with(x, n);
    mount_root();
    next_named("iu");
    fresh();
    x[40] ^= 1;
    root_with(x, n);
    mount_root();
    assert_int_equal(udf_dir_next(&d, &e), UDF_ERR_CORRUPT);

    /* The directory stream unreadable, and the child's entry unreadable. */
    fresh();
    mount_root();
    fail_off = (uint64_t)(PSTART + 2) * BS;
    assert_int_equal(udf_dir_next(&d, &e), UDF_ERR_IO);
    fresh();
    mount_root();
    fail_off = (uint64_t)(PSTART + 3) * BS;
    v.scratch_off = ~(uint64_t)0;
    assert_int_equal(udf_dir_next(&d, &e), UDF_ERR_IO);
}

/* A directory ICB that is not a directory. */
static void test_udf_dir_types(void **state)
{
    uint8_t ad[8];

    (void)state;
    fresh();
    short_ad(ad, 5, 0, 10);
    fe(1, 5, 5, 0, ad, 8, 0);
    assert_int_equal(udf_mount(&v, img_read, NULL), UDF_OK);
    assert_int_equal(udf_dir_open_root(&v, &d), UDF_ERR_CORRUPT);
}

int run_udf_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_udf_basic),
        cmocka_unit_test(test_udf_labels),
        cmocka_unit_test(test_udf_recognition),
        cmocka_unit_test(test_udf_anchor_tags),
        cmocka_unit_test(test_udf_sequence),
        cmocka_unit_test(test_udf_file_set),
        cmocka_unit_test(test_udf_entry_refusals),
        cmocka_unit_test(test_udf_chains_and_holes),
        cmocka_unit_test(test_udf_extent_limits),
        cmocka_unit_test(test_udf_fid_refusals),
        cmocka_unit_test(test_udf_dir_types),
    };

    return cmocka_run_group_tests_name("udf", tests, NULL, NULL);
}
