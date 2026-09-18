/* The ISO9660 and UDF drivers (falcon-core/src/fs/iso9660.c, udf.c) against
 * images the real tools wrote, reached through the filesystem registry the
 * way every consumer reaches them.
 *
 * The fixtures are falcon-core/tools/mk-test-iso9660.sh (xorriso: Rock Ridge,
 * Joliet, plain, and a relocated deep tree with a zisofs file) and
 * falcon-core/tools/mk-test-udf.sh (mkudffs and the kernel's UDF writer: short
 * and long allocation descriptors, and an empty UDF 2.50 image the driver
 * must refuse). Each populated image has a manifest beside it listing every
 * object's type, size and SHA-256, and the walk here must reproduce it
 * exactly: nothing missing, nothing extra, every byte read back equal.
 *
 * Going through fs_* rather than the drivers is deliberate. It is how the
 * loader and the sign tool read media, so it tests the adapters in
 * falcon-core/src/fs/fs.c, the probe order that must put UDF before ISO9660
 * and the label hook, as well as the drivers. The drivers' own edge cases,
 * the structures no tool writes, are in iso9660_test.c and udf_test.c.
 *
 * The sweeps at the end follow ext4_test.c's: every read failing in turn
 * must produce a status rather than a crash, and every metadata byte a clean
 * walk reads is flipped in turn with the same requirement. The asan build is
 * what makes "no crash" mean "no out-of-bounds read" too.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fs/fs.h>
#include <crypto/sha256.h>

#ifndef MEDIA_FIXTURE_DIR
#define MEDIA_FIXTURE_DIR "build"
#endif

#define MAN_MAX   1024
#define PATH_CAP  1024
#define DEPTH_MAX 16

/* ------------------------------------------------------------- plumbing */

static int img_fd = -1;

static int img_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    int fd = *(const int *)ctx;
    uint8_t *p = buf;

    while (len > 0) {
        ssize_t n = pread(fd, p, (size_t)len, (off_t)off);

        if (n <= 0)
            return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

static int open_fixture(const char *name)
{
    char path[512];

    snprintf(path, sizeof path, "%s/%s", MEDIA_FIXTURE_DIR, name);
    return open(path, O_RDONLY);
}

/* ------------------------------------------------------------- manifest */

typedef struct {
    char     type;
    char     path[PATH_CAP];
    uint64_t size;
    char     sha[65];
    uint8_t  seen;
} man_ent;

static man_ent g_man[MAN_MAX];
static size_t  g_man_n;

static int load_manifest(const char *img)
{
    char path[512];
    char line[2048];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s.manifest", MEDIA_FIXTURE_DIR, img);
    f = fopen(path, "r");
    if (f == NULL)
        return -1;
    g_man_n = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        char *ty = strtok(line, "\t");
        char *p = strtok(NULL, "\t");
        char *sz = strtok(NULL, "\t");
        char *sha = strtok(NULL, "\t\n");

        if (ty == NULL || p == NULL || sz == NULL || sha == NULL ||
            g_man_n >= MAN_MAX)
            continue;
        g_man[g_man_n].type = ty[0];
        snprintf(g_man[g_man_n].path, PATH_CAP, "%s", p);
        g_man[g_man_n].size = (sz[0] == '-') ? 0 : strtoull(sz, NULL, 10);
        snprintf(g_man[g_man_n].sha, sizeof g_man[0].sha, "%s", sha);
        g_man[g_man_n].seen = 0;
        g_man_n++;
    }
    fclose(f);
    return g_man_n > 0 ? 0 : -1;
}

static man_ent *man_find(const char *path)
{
    size_t i;

    for (i = 0; i < g_man_n; i++) {
        if (strcmp(g_man[i].path, path) == 0)
            return &g_man[i];
    }
    return NULL;
}

/* ----------------------------------------------------------------- walk */

static fs_vol g_vol;

static void hex(const uint8_t *h, char *out)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++) {
        out[2 * i] = digits[h[i] >> 4];
        out[2 * i + 1] = digits[h[i] & 15u];
    }
    out[64] = '\0';
}

/* SHA-256 of a whole file through fs_file_read, in odd-sized pieces so the
 * reads land on and off block boundaries. */
static int file_sha(const fs_dirent *e, char *out, uint64_t *size)
{
    static uint8_t buf[70001];
    fs_file *f = malloc(sizeof *f);
    sha256_ctx c;
    uint8_t h[32];
    uint64_t off = 0;
    int rc;

    assert_non_null(f);
    rc = fs_file_open(&g_vol, f, e);
    if (rc != FS_OK) {
        free(f);
        return rc;
    }
    *size = fs_file_size(f);
    sha256_init(&c);
    while (off < *size) {
        uint64_t n = *size - off;

        if (n > sizeof buf)
            n = sizeof buf;
        rc = fs_file_read(f, off, n, buf);
        if (rc != FS_OK) {
            free(f);
            return rc;
        }
        sha256_update(&c, buf, (size_t)n);
        off += n;
    }
    sha256_final(&c, h);
    hex(h, out);
    free(f);
    return FS_OK;
}

typedef struct {
    size_t dirs, files, links, refused;
    int    refused_rc;
    char   refused_path[PATH_CAP];
} walk_stats;

/* Walks the whole tree, checking every object against the manifest. */
static void walk(fs_dir *d, char *path, unsigned depth, walk_stats *st)
{
    fs_dirent e;
    int rc;

    assert_true(depth < DEPTH_MAX);
    while ((rc = fs_dir_next(d, &e)) == 1) {
        char name[FS_NAME_MAX * 3 + 1];
        size_t pl = strlen(path);
        man_ent *m;

        fs_name_to_bytes(e.name, (uint8_t *)name, sizeof name);
        snprintf(path + pl, PATH_CAP - pl, "/%s", name);
        m = man_find(path);
        if (m == NULL)
            fail_msg("walk found %s, which the manifest does not list", path);
        assert_false(m->seen);
        m->seen = 1;

        if (e.is_dir) {
            fs_dir *sub = malloc(sizeof *sub);

            assert_non_null(sub);
            assert_int_equal(m->type, 'd');
            assert_int_equal(fs_dir_open(&g_vol, sub, &e), FS_OK);
            st->dirs++;
            walk(sub, path, depth + 1, st);
            free(sub);
        } else if (!e.is_regular) {
            fs_file *f = malloc(sizeof *f);

            assert_non_null(f);
            assert_int_equal(m->type, 'l');
            assert_int_equal(fs_file_open(&g_vol, f, &e), FS_ERR_SYMLINK);
            free(f);
            st->links++;
        } else {
            char sha[65];
            uint64_t size = 0;

            assert_int_equal(m->type, 'f');
            rc = file_sha(&e, sha, &size);
            if (rc != FS_OK) {
                /* Refused rather than read: recorded for the caller to
                 * judge, since only some fixtures carry such a file. */
                st->refused++;
                st->refused_rc = rc;
                snprintf(st->refused_path, PATH_CAP, "%s", path);
            } else {
                if (size != m->size || strcmp(sha, m->sha) != 0)
                    fail_msg("%s: size %llu sha %s, manifest %llu %s", path,
                             (unsigned long long)size, sha,
                             (unsigned long long)m->size, m->sha);
                assert_int_equal(e.size, m->size);
                st->files++;
            }
        }
        path[pl] = '\0';
    }
    assert_int_equal(rc, 0);
}

/* Mounts IMG through the registry and walks it against its manifest.
 * Returns the stats; the manifest must be fully seen apart from what the
 * caller expects to be refused. */
static void walk_image(const char *img, const char *driver, const char *label,
                       walk_stats *st)
{
    const fs_ops *o;
    fs_dir *root = malloc(sizeof *root);
    char path[PATH_CAP] = "";
    size_t i;

    assert_non_null(root);
    memset(st, 0, sizeof(*st));
    img_fd = open_fixture(img);
    assert_true(img_fd >= 0);
    assert_int_equal(load_manifest(img), 0);

    o = fs_identify(img_read, &img_fd, FS_HINT_NONE);
    assert_non_null(o);
    assert_string_equal(o->name, driver);
    assert_int_equal(fs_mount(&g_vol, o, img_read, &img_fd), FS_OK);
    assert_string_equal(fs_vol_label(&g_vol), label);
    assert_int_equal(fs_dir_open_root(&g_vol, root), FS_OK);
    walk(root, path, 0, st);
    free(root);

    for (i = 0; i < g_man_n; i++) {
        if (!g_man[i].seen && strcmp(g_man[i].path, st->refused_path) != 0)
            fail_msg("%s: manifest lists %s, the walk never found it", img,
                     g_man[i].path);
    }
    close(img_fd);
    img_fd = -1;
}

/* --------------------------------------------------------------- tests */

static void test_media_rr_walk(void **state)
{
    walk_stats st;

    (void)state;
    walk_image("test-iso-rr.iso", "iso9660", "FALCON_ISO_RR", &st);
    assert_int_equal(st.refused, 0);
    assert_int_equal(st.links, 1);      /* link-to-s1 */
    assert_true(st.files > 400);        /* many/ alone is 400 */
}

static void test_media_joliet_walk(void **state)
{
    walk_stats st;

    (void)state;
    walk_image("test-iso-joliet.iso", "iso9660", "FALCON_ISO_J", &st);
    assert_int_equal(st.refused, 0);
    assert_int_equal(st.links, 0);
}

static void test_media_plain_walk(void **state)
{
    walk_stats st;

    (void)state;
    walk_image("test-iso-plain.iso", "iso9660", "FALCON_ISO_P", &st);
    assert_int_equal(st.refused, 0);
}

/* The deep tree is only reachable through Rock Ridge's relocation records:
 * the walk has to follow CL into rr_moved and must not list the relocated
 * directory a second time. The one refusal is the zisofs file. */
static void test_media_deep_walk(void **state)
{
    walk_stats st;

    (void)state;
    walk_image("test-iso-deep.iso", "iso9660", "FALCON_ISO_DEEP", &st);
    assert_int_equal(st.refused, 1);
    assert_int_equal(st.refused_rc, FS_ERR_UNSUPPORTED);
    assert_string_equal(st.refused_path, "/zeros.bin");
    assert_int_equal(st.dirs, 9);
}

static void test_media_udf_walk(void **state)
{
    walk_stats st;

    (void)state;
    walk_image("test-udf.img", "udf", "FALCON_UDF", &st);
    assert_int_equal(st.refused, 0);
    assert_int_equal(st.links, 1);
}

static void test_media_udf_long_walk(void **state)
{
    walk_stats st;

    (void)state;
    walk_image("test-udf-long.img", "udf", "FALCON_UDF_L", &st);
    assert_int_equal(st.refused, 0);
    assert_int_equal(st.links, 1);
}

/* UDF 2.50's metadata partition is refused, and refused as unsupported:
 * the image is valid, the driver just does not read that shape. */
static void test_media_udf_250_refused(void **state)
{
    const fs_ops *o;

    (void)state;
    img_fd = open_fixture("test-udf-250.img");
    assert_true(img_fd >= 0);
    o = fs_identify(img_read, &img_fd, FS_HINT_NONE);
    assert_non_null(o);
    assert_string_equal(o->name, "udf");
    assert_int_equal(fs_mount(&g_vol, o, img_read, &img_fd),
                     FS_ERR_UNSUPPORTED);
    assert_string_equal(fs_vol_label(&g_vol), "");
    close(img_fd);
    img_fd = -1;
}

/* Looks a root-level file up by name. */
static int find_root(const char *name, fs_dirent *out)
{
    fs_dir *d = malloc(sizeof *d);
    int rc;

    assert_non_null(d);
    assert_int_equal(fs_dir_open_root(&g_vol, d), FS_OK);
    while ((rc = fs_dir_next(d, out)) == 1) {
        if (fs_name_eq_ascii(out->name, name))
            break;
    }
    free(d);
    return rc == 1 ? 0 : -1;
}

/* 400 reads at pseudo-random offsets and lengths across big.bin, compared
 * against a plain copy: the head/tail cache paths and the aligned direct
 * path, in every combination the arithmetic can produce. */
static void random_spans(const char *img, const char *bigcopy)
{
    static uint8_t a[300000], b[300000];
    const fs_ops *o;
    fs_dirent e;
    fs_file *f = malloc(sizeof *f);
    int big_fd;
    uint64_t size;
    uint32_t seed = 0x2545F491u;
    int i;

    assert_non_null(f);
    img_fd = open_fixture(img);
    big_fd = open_fixture(bigcopy);
    assert_true(img_fd >= 0 && big_fd >= 0);
    o = fs_identify(img_read, &img_fd, FS_HINT_NONE);
    assert_non_null(o);
    assert_int_equal(fs_mount(&g_vol, o, img_read, &img_fd), FS_OK);
    assert_int_equal(find_root("big.bin", &e), 0);
    assert_int_equal(fs_file_open(&g_vol, f, &e), FS_OK);
    size = fs_file_size(f);
    assert_true(size > sizeof a);

    for (i = 0; i < 400; i++) {
        uint64_t off, len;

        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        off = seed % size;
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        len = seed % sizeof a;
        if (len > size - off)
            len = size - off;
        assert_int_equal(fs_file_read(f, off, len, a), FS_OK);
        assert_int_equal(img_read(&big_fd, off, len, b), 0);
        assert_memory_equal(a, b, len);
    }
    /* The end exactly, and one byte past it. */
    assert_int_equal(fs_file_read(f, size, 0, a), FS_OK);
    assert_int_equal(fs_file_read(f, size - 1, 2, a), FS_ERR_RANGE);
    assert_int_equal(fs_file_read(f, size + 1, 0, a), FS_ERR_RANGE);

    free(f);
    close(big_fd);
    close(img_fd);
    img_fd = -1;
}

static void test_media_rr_random_spans(void **state)
{
    (void)state;
    random_spans("test-iso-rr.iso", "test-iso.bigfile");
}

static void test_media_udf_random_spans(void **state)
{
    (void)state;
    random_spans("test-udf.img", "test-udf.bigfile");
}

/* The extent map, checked the only way that means anything: the bytes at the
 * reported volume offsets, concatenated, are the file. */
static size_t extents_check(const char *img, const char *name)
{
    static uint8_t want[4u << 20], got[4u << 20];
    fs_extent ext[64];
    const fs_ops *o;
    fs_dirent e;
    fs_file *f = malloc(sizeof *f);
    size_t n = 0, need = 0, i;
    uint64_t at = 0, size;

    assert_non_null(f);
    img_fd = open_fixture(img);
    assert_true(img_fd >= 0);
    o = fs_identify(img_read, &img_fd, FS_HINT_NONE);
    assert_non_null(o);
    assert_int_equal(fs_mount(&g_vol, o, img_read, &img_fd), FS_OK);
    assert_true(fs_has_extents(&g_vol));
    assert_int_equal(find_root(name, &e), 0);
    assert_int_equal(fs_file_open(&g_vol, f, &e), FS_OK);
    size = fs_file_size(f);
    assert_true(size <= sizeof want);
    assert_int_equal(fs_file_read(f, 0, size, want), FS_OK);

    /* Sized first with no array, as the header promises a caller can. */
    assert_int_equal(fs_extents(&g_vol, &e, NULL, 0, &need), FS_ERR_NOSPACE);
    assert_true(need > 0 && need <= 64);
    assert_int_equal(fs_extents(&g_vol, &e, ext, 64, &n), FS_OK);
    assert_int_equal(n, need);
    for (i = 0; i < n; i++) {
        assert_true(at + ext[i].len <= size);
        assert_int_equal(img_read(&img_fd, ext[i].vol_off, ext[i].len,
                                  got + at), 0);
        at += ext[i].len;
    }
    assert_int_equal(at, size);
    assert_memory_equal(want, got, size);
    if (n > 1) {
        size_t short_n = 0;

        assert_int_equal(fs_extents(&g_vol, &e, ext, n - 1, &short_n),
                         FS_ERR_NOSPACE);
        assert_int_equal(short_n, n);
    }
    free(f);
    close(img_fd);
    img_fd = -1;
    return n;
}

static void test_media_extents(void **state)
{
    (void)state;
    /* An ISO file is one extent; the interleaved UDF writes are many. */
    assert_int_equal(extents_check("test-iso-rr.iso", "big.bin"), 1);
    assert_true(extents_check("test-udf.img", "big.bin") > 4);
    assert_true(extents_check("test-udf-long.img", "big.bin") > 4);
}

/* ------------------------------------------------------------- sweeps */

static unsigned g_fail_at;      /* 1-based read index that fails, 0 = none */
static unsigned g_reads;
static int      g_recording;
static uint64_t g_flip_off = ~(uint64_t)0;
static uint8_t  g_flip_xor;

#define REC_MAX 8192u
static struct { uint64_t off, len; } g_rec[REC_MAX];
static size_t g_rec_n;

static int sweep_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    int rc;

    g_reads++;
    if (g_fail_at != 0 && g_reads >= g_fail_at)
        return -1;
    rc = img_read(ctx, off, len, buf);
    if (rc == 0 && g_recording && len <= 2048u && g_rec_n < REC_MAX) {
        size_t i;

        for (i = 0; i < g_rec_n; i++) {
            if (g_rec[i].off == off)
                break;
        }
        if (i == g_rec_n) {
            g_rec[g_rec_n].off = off;
            g_rec[g_rec_n].len = len;
            g_rec_n++;
        }
    }
    if (rc == 0 && g_flip_off >= off && g_flip_off < off + len)
        ((uint8_t *)buf)[g_flip_off - off] ^= g_flip_xor;
    return rc;
}

static int status_known(int rc)
{
    return rc == FS_OK || rc == FS_ERR_IO || rc == FS_ERR_BAD_SUPER ||
           rc == FS_ERR_CORRUPT || rc == FS_ERR_BAD_ARG ||
           rc == FS_ERR_RANGE || rc == FS_ERR_NOSPACE ||
           rc == FS_ERR_UNSUPPORTED || rc == FS_ERR_SYMLINK;
}

/* Mount through the registry and walk two levels, opening and reading a
 * little of every regular file. Stops at the first failure and returns it. */
static int sweep_walk(fs_dir *d, unsigned depth)
{
    static uint8_t buf[4096];
    fs_dirent e;
    unsigned seen = 0;
    int rc;

    while ((rc = fs_dir_next(d, &e)) == 1) {
        /* A bounded share of each directory: the sweep is about structures,
         * and many/ is 400 copies of one. */
        if (++seen > 24)
            break;
        if (e.is_dir) {
            fs_dir *sub;

            if (depth >= 2)
                continue;
            sub = malloc(sizeof *sub);
            assert_non_null(sub);
            rc = fs_dir_open(&g_vol, sub, &e);
            if (rc == FS_OK)
                rc = sweep_walk(sub, depth + 1);
            free(sub);
            if (rc != FS_OK)
                return rc;
        } else if (e.is_regular) {
            fs_file *f = malloc(sizeof *f);
            uint64_t n;

            assert_non_null(f);
            rc = fs_file_open(&g_vol, f, &e);
            if (rc == FS_OK) {
                n = fs_file_size(f);
                if (n > sizeof buf)
                    n = sizeof buf;
                rc = fs_file_read(f, 0, n, buf);
            }
            free(f);
            if (rc != FS_OK && rc != FS_ERR_UNSUPPORTED)
                return rc;
        }
    }
    return rc < 0 ? rc : FS_OK;
}

static int sweep_workload(void)
{
    const fs_ops *o = fs_identify(sweep_read, &img_fd, FS_HINT_NONE);
    fs_dir *root;
    int rc;

    if (o == NULL)
        return FS_ERR_BAD_SUPER;
    rc = fs_mount(&g_vol, o, sweep_read, &img_fd);
    if (rc != FS_OK)
        return rc;
    root = malloc(sizeof *root);
    assert_non_null(root);
    rc = fs_dir_open_root(&g_vol, root);
    if (rc == FS_OK)
        rc = sweep_walk(root, 0);
    free(root);
    return rc;
}

static void io_sweep(const char *img)
{
    unsigned total, n;

    img_fd = open_fixture(img);
    assert_true(img_fd >= 0);
    g_fail_at = 0;
    g_reads = 0;
    assert_int_equal(sweep_workload(), FS_OK);
    total = g_reads;
    assert_true(total > 10);
    for (n = 1; n <= total; n++) {
        int rc;

        g_fail_at = n;
        g_reads = 0;
        rc = sweep_workload();
        if (rc == FS_OK || !status_known(rc))
            fail_msg("%s: read %u of %u failing gave %d", img, n, total, rc);
    }
    g_fail_at = 0;
    close(img_fd);
    img_fd = -1;
}

static void test_media_io_failure_sweep(void **state)
{
    (void)state;
    io_sweep("test-iso-rr.iso");
    io_sweep("test-iso-joliet.iso");
    io_sweep("test-iso-deep.iso");
    io_sweep("test-udf.img");
    io_sweep("test-udf-long.img");
}

static void corruption_sweep(const char *img, size_t min_refused)
{
    static const uint8_t masks[] = { 0xffu, 0x01u };
    size_t i, m, swept = 0, refused = 0;

    img_fd = open_fixture(img);
    assert_true(img_fd >= 0);
    g_rec_n = 0;
    g_recording = 1;
    assert_int_equal(sweep_workload(), FS_OK);
    g_recording = 0;
    assert_true(g_rec_n > 0 && g_rec_n < REC_MAX);

    for (i = 0; i < g_rec_n; i++) {
        uint64_t at;

        for (at = g_rec[i].off; at < g_rec[i].off + g_rec[i].len; at++) {
            for (m = 0; m < sizeof masks; m++) {
                int rc;

                g_flip_off = at;
                g_flip_xor = masks[m];
                rc = sweep_workload();
                if (!status_known(rc))
                    fail_msg("%s: flip 0x%02x at %llu gave %d", img,
                             (unsigned)masks[m], (unsigned long long)at, rc);
                if (rc != FS_OK)
                    refused++;
                swept++;
            }
        }
    }
    g_flip_off = ~(uint64_t)0;
    assert_true(swept > 10000);
    if (refused <= min_refused)
        fail_msg("%s: only %zu of %zu flips refused", img, refused, swept);
    assert_int_equal(sweep_workload(), FS_OK);
    close(img_fd);
    img_fd = -1;
}

static void test_media_corruption_sweep_iso(void **state)
{
    (void)state;
    corruption_sweep("test-iso-rr.iso", 500u);
    corruption_sweep("test-iso-deep.iso", 100u);
}

static void test_media_corruption_sweep_udf(void **state)
{
    (void)state;
    /* Every UDF descriptor carries a CRC, so almost any flip in one is
     * caught; the refusals are most of the sweep. */
    corruption_sweep("test-udf.img", 2000u);
}

int run_media_fs_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_media_rr_walk),
        cmocka_unit_test(test_media_joliet_walk),
        cmocka_unit_test(test_media_plain_walk),
        cmocka_unit_test(test_media_deep_walk),
        cmocka_unit_test(test_media_udf_walk),
        cmocka_unit_test(test_media_udf_long_walk),
        cmocka_unit_test(test_media_udf_250_refused),
        cmocka_unit_test(test_media_rr_random_spans),
        cmocka_unit_test(test_media_udf_random_spans),
        cmocka_unit_test(test_media_extents),
        cmocka_unit_test(test_media_io_failure_sweep),
        cmocka_unit_test(test_media_corruption_sweep_iso),
        cmocka_unit_test(test_media_corruption_sweep_udf),
    };
    static const char *const need[] = {
        "test-iso-rr.iso", "test-iso-joliet.iso", "test-iso-plain.iso",
        "test-iso-deep.iso", "test-iso.bigfile", "test-udf.img",
        "test-udf-long.img", "test-udf-250.img", "test-udf.bigfile",
    };
    size_t i;

    for (i = 0; i < sizeof need / sizeof need[0]; i++) {
        int fd = open_fixture(need[i]);

        if (fd < 0) {
            printf("\n[  SKIPPED ] media fixtures: %s/%s not found.\n"
                   "             Build them with: make test-iso9660 "
                   "test-udf (the second needs sudo).\n",
                   MEDIA_FIXTURE_DIR, need[i]);
            return test_skip_rc();
        }
        close(fd);
    }
    return cmocka_run_group_tests_name("media fs", tests, NULL, NULL);
}
