/* Fixture-backed tests for src/ext4/ext4.c.
 *
 * These run against build/test-ext4.img, the 128 MiB ext4 filesystem
 * tools/mk-test-ext4.sh creates with a stock mkfs.ext4 and populates through
 * the kernel's own driver, plus the two products that script leaves beside
 * it: build/test-ext4.manifest (every object's type, size and sha256) and
 * build/test-ext4.bigfile (a plain copy of the 24 MiB file). Extracting both
 * at image-build time is what keeps this binary free of sudo and loop mounts;
 * see the script's header for the argument.
 *
 * What that buys, and what it does not. Agreement with mkfs.ext4 and the
 * kernel is the only evidence worth having that a from-scratch reader reads
 * ext4 rather than something ext4-shaped, so the walk, the extent tree, the
 * htree directory and every file's bytes are compared against what the
 * producers wrote. But a real image cannot be made to carry a corrupt
 * superblock, so the refusal paths are driven by patching a copy of the
 * superblock in memory and serving it through the read callback. Those
 * patches recompute the crc32c the driver verifies first, because otherwise
 * every one of them would come back EXT4_ERR_CSUM and prove nothing about the
 * check it was aiming at.
 *
 * If the image is absent the whole group is skipped with an explanatory
 * message rather than failing, exactly as the exFAT fixture group does, so a
 * fresh checkout can run the suite before anyone has been asked for a sudo
 * password.
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

#include <ext4/ext4.h>
#include <crypto/sha256.h>

#ifndef EXT4_IMG
#define EXT4_IMG "build/test-ext4.img"
#endif
#ifndef EXT4_MANIFEST
#define EXT4_MANIFEST "build/test-ext4.manifest"
#endif
#ifndef EXT4_BIGFILE
#define EXT4_BIGFILE "build/test-ext4.bigfile"
#endif

#define BIG_PATH  "/big.bin"
#define MANY_PATH "/manydir"
#define MANY_N    300u

#define MAN_MAX    512
#define PATH_CAP   512
#define WALK_DEPTH 8

static int img_fd = -1;
static int big_fd = -1;

/* ------------------------------------------------------------- plumbing */

static int img_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    int fd = *(const int *)ctx;
    uint8_t *p = buf;

    while (len > 0) {
        ssize_t n = pread(fd, p, (size_t)len, (off_t)off);

        if (n <= 0) {
            return -1;
        }
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

/* Fails every read, for the EXT4_ERR_IO paths. */
static int dead_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    (void)off;
    (void)len;
    (void)buf;
    return -1;
}

/* ------------------------------------------------------------- manifest */

typedef struct {
    char     type;                /* f regular, d directory, l symlink */
    char     path[PATH_CAP];
    uint64_t size;
    char     sha[65];             /* hex, or "-" */
    uint8_t  seen;
} man_ent;

static man_ent manifest[MAN_MAX];
static size_t  manifest_n;

static int load_manifest(void)
{
    char line[1024];
    FILE *f = fopen(EXT4_MANIFEST, "r");

    if (f == NULL) {
        return -1;
    }
    manifest_n = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        char ty[8], path[PATH_CAP], sha[65];
        unsigned long long size = 0;

        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (sscanf(line, "%7[^\t]\t%255[^\t]\t%llu\t%64s",
                   ty, path, &size, sha) != 4) {
            continue;
        }
        if (manifest_n >= MAN_MAX) {
            fclose(f);
            return -1;
        }
        manifest[manifest_n].type = ty[0];
        snprintf(manifest[manifest_n].path, PATH_CAP, "%s", path);
        manifest[manifest_n].size = size;
        snprintf(manifest[manifest_n].sha, sizeof manifest[0].sha, "%s", sha);
        manifest[manifest_n].seen = 0;
        manifest_n++;
    }
    fclose(f);
    return (manifest_n > 0) ? 0 : -1;
}

static man_ent *manifest_find(const char *path)
{
    size_t i;

    for (i = 0; i < manifest_n; i++) {
        if (strcmp(manifest[i].path, path) == 0) {
            return &manifest[i];
        }
    }
    return NULL;
}

static void hex_of(const uint8_t *d, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[2u * i] = digits[(d[i] >> 4) & 0xfu];
        out[2u * i + 1u] = digits[d[i] & 0xfu];
    }
    out[2u * n] = '\0';
}

/* ----------------------------------------------------------------- walk */

typedef struct {
    char     path[PATH_CAP];
    char     type;
    uint64_t size;
    uint32_t ino;
} walk_ent;

typedef struct {
    walk_ent ents[MAN_MAX];
    size_t   n;
} walk_result;

/* Depth-first walk of the whole volume. Each ext4_dir frame is about 4.3 KB
 * (a whole block plus the inode), which is exactly why the loader caps its
 * own walk rather than recursing freely; the fixture's deepest path is
 * /nested/a/b/c, so four frames is the most this ever holds. */
static void walk(ext4_vol *v, ext4_dir *d, const char *prefix,
                 walk_result *out, unsigned depth)
{
    ext4_dirent e;
    int rc;

    assert_true(depth < WALK_DEPTH);

    for (;;) {
        char child[PATH_CAP];
        size_t at;

        rc = ext4_dir_next(d, &e);
        assert_true(rc >= 0);
        if (rc == 0) {
            break;
        }

        snprintf(child, sizeof child, "%s/%s", prefix, (const char *)e.name);

        assert_true(out->n < MAN_MAX);
        at = out->n++;
        snprintf(out->ents[at].path, PATH_CAP, "%s", child);
        out->ents[at].ino = e.ino;
        out->ents[at].type = e.is_dir ? 'd' : (e.is_regular ? 'f' : 'l');
        out->ents[at].size = 0;

        if (e.is_regular) {
            ext4_file f;

            assert_int_equal(ext4_file_open(v, &f, &e), EXT4_OK);
            out->ents[at].size = ext4_file_size(&f);
        } else if (e.is_dir) {
            ext4_dir sub;

            assert_int_equal(ext4_dir_open(v, &sub, &e), EXT4_OK);
            walk(v, &sub, child, out, depth + 1u);
        }
    }
}

/* --------------------------------------------------------- crc32c + sb */

/* crc32c (Castagnoli), reflected, computed bitwise. Deliberately not the
 * nibble-table form src/ext4/ext4.c uses: a superblock patched with the
 * driver's own checksum routine would pass the driver's own check even if
 * both were wrong the same way. This is 12 lines and independent. */
static uint32_t crc32c_ref(uint32_t crc, const uint8_t *p, size_t n)
{
    size_t i;
    unsigned b;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8u; b++) {
            crc = (crc >> 1) ^ (0x82F63B78u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc;
}

/* Superblock field offsets, from src/ext4/ext4.c's own table. They are part
 * of the on-disk format and cannot move. */
#define SB_AT             1024u
#define SB_LEN            1024u
#define SB_LOG_BLOCK_SIZE 0x18u
#define SB_MAGIC          0x38u
#define SB_STATE          0x3Au
#define SB_REV_LEVEL      0x4Cu
#define SB_INODE_SIZE     0x58u
#define SB_INCOMPAT       0x60u
#define SB_RO_COMPAT      0x64u
#define SB_CSUM_TYPE      0x175u
#define SB_CSUM           0x3FCu

#define ST_VALID_FS  0x0001u
#define ST_ERROR_FS  0x0002u
#define ST_ORPHAN_FS 0x0004u

#define IC_RECOVER        0x0004u
#define IC_META_BG        0x0010u
#define RC_ORPHAN_PRESENT 0x00010000u

/* The read callback for a patched volume: the first PATCH_BYTES come from a
 * private copy, everything past them from the real image. mount() reads only
 * the 1 KiB superblock, so this is enough for every refusal below while
 * leaving the rest of the filesystem genuine for the cases that do proceed. */
#define PATCH_BYTES 8192u

typedef struct {
    uint8_t head[PATCH_BYTES];
} patched;

static patched patch_ctx;

static int patched_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    patched *p = (patched *)ctx;

    if (off < PATCH_BYTES) {
        uint64_t take = PATCH_BYTES - off;

        if (take > len) {
            take = len;
        }
        memcpy(buf, p->head + off, (size_t)take);
        if (take == len) {
            return 0;
        }
        return img_read(&img_fd, off + take, len - take,
                        (uint8_t *)buf + take);
    }
    return img_read(&img_fd, off, len, buf);
}

static void patch_reset(void)
{
    assert_int_equal(img_read(&img_fd, 0, PATCH_BYTES, patch_ctx.head), 0);
}

static uint16_t sb_get16(uint32_t at)
{
    const uint8_t *p = patch_ctx.head + SB_AT + at;

    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t sb_get32(uint32_t at)
{
    const uint8_t *p = patch_ctx.head + SB_AT + at;

    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void sb_put16(uint32_t at, uint16_t val)
{
    uint8_t *p = patch_ctx.head + SB_AT + at;

    p[0] = (uint8_t)val;
    p[1] = (uint8_t)(val >> 8);
}

static void sb_put32(uint32_t at, uint32_t val)
{
    uint8_t *p = patch_ctx.head + SB_AT + at;

    p[0] = (uint8_t)val;
    p[1] = (uint8_t)(val >> 8);
    p[2] = (uint8_t)(val >> 16);
    p[3] = (uint8_t)(val >> 24);
}

/* Every patch below must be followed by this, because ext4_mount verifies
 * the superblock checksum before it looks at a single other field. Without
 * it every one of these tests would pass for the wrong reason. */
static void sb_refresh_csum(void)
{
    uint32_t crc = crc32c_ref(0xFFFFFFFFu, patch_ctx.head + SB_AT, SB_CSUM);

    sb_put32(SB_CSUM, crc);
}

static int mount_patched(ext4_vol *v)
{
    return ext4_mount(v, patched_read, &patch_ctx);
}

/* ------------------------------------------------------------- geometry */

static void test_ext4_mount_geometry(void **state)
{
    ext4_vol v;

    (void)state;
    assert_int_equal(ext4_mount(&v, img_read, &img_fd), EXT4_OK);

    /* A 128 MiB image gets 1 KiB blocks from mke2fs, which is the only
     * layout where s_first_data_block is 1 rather than 0: the superblock is
     * block 1 and the descriptor table block 2. ext4.h names this as the
     * case the host tests exist to cover. */
    assert_int_equal(v.block_size, 1024);
    assert_int_equal(v.first_data_block, 1);
    assert_int_equal(v.gdt_off, 2048);

    assert_int_equal(v.inode_size, 256);
    assert_true(v.inodes_per_group > 0);
    assert_true(v.blocks_per_group > 0);
    assert_true(v.inodes_count > 0);
    assert_int_equal(v.blocks_count, 128ull * 1024ull); /* 128 MiB / 1 KiB */
    assert_true(v.group_count >= 16);

    /* mke2fs sets metadata_csum and metadata_csum_seed by default, so the
     * driver's checksum verification is live for every test in this file
     * rather than quietly skipped. */
    assert_int_equal(v.has_csum, 1);
    assert_true(v.csum_seed != 0);

    /* 64bit is on, so descriptors are 64 bytes. */
    assert_int_equal(v.desc_size, 64);

    assert_int_equal(ext4_mount(NULL, img_read, &img_fd), EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_mount(&v, NULL, &img_fd), EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_mount(&v, dead_read, NULL), EXT4_ERR_IO);
}

/* ----------------------------------------------------------------- walk */

static void test_ext4_walk_matches_manifest(void **state)
{
    static walk_result w;
    ext4_vol v;
    ext4_dir root;
    size_t i;

    (void)state;
    assert_int_equal(ext4_mount(&v, img_read, &img_fd), EXT4_OK);
    assert_int_equal(ext4_dir_open_root(&v, &root), EXT4_OK);

    w.n = 0;
    walk(&v, &root, "", &w, 0);

    /* Nothing extra: every path the driver produced is one the fixture
     * script recorded. */
    for (i = 0; i < w.n; i++) {
        man_ent *m = manifest_find(w.ents[i].path);

        if (m == NULL) {
            fail_msg("driver found an object not in the manifest: %s",
                     w.ents[i].path);
        }
        if (m->seen) {
            fail_msg("driver produced %s twice", w.ents[i].path);
        }
        m->seen = 1;
        if (m->type != w.ents[i].type) {
            fail_msg("%s: manifest says '%c', driver says '%c'",
                     m->path, m->type, w.ents[i].type);
        }
        /* Directory sizes are an allocation detail rather than a property of
         * the tree, and a symlink's "size" is its target length, which this
         * driver deliberately never reads. Only regular files are compared. */
        if (m->type == 'f' && m->size != w.ents[i].size) {
            fail_msg("%s: manifest size %llu, driver size %llu", m->path,
                     (unsigned long long)m->size,
                     (unsigned long long)w.ents[i].size);
        }
    }

    /* Nothing missing. */
    for (i = 0; i < manifest_n; i++) {
        if (!manifest[i].seen) {
            fail_msg("driver never produced %s", manifest[i].path);
        }
    }
    assert_int_equal(w.n, manifest_n);
}

/* Every file's contents, hashed with this repo's own SHA-256 and compared
 * against the sha256sum the fixture script recorded. Two units validate each
 * other here, which is fine: both are pinned to outside evidence elsewhere
 * (crypto_test.c against FIPS vectors, the driver against the kernel). */
static void test_ext4_file_hashes(void **state)
{
    static uint8_t chunk[64u * 1024u];
    ext4_vol v;
    size_t i;
    unsigned files = 0;

    (void)state;
    assert_int_equal(ext4_mount(&v, img_read, &img_fd), EXT4_OK);

    for (i = 0; i < manifest_n; i++) {
        ext4_dir d;
        ext4_dirent e;
        ext4_file f;
        sha256_ctx c;
        uint8_t digest[SHA256_DIGEST_LEN];
        char hex[2u * SHA256_DIGEST_LEN + 1u];
        char parent[PATH_CAP];
        const char *base;
        uint64_t off = 0;
        uint64_t size;
        int found = 0;

        if (manifest[i].type != 'f') {
            continue;
        }
        /* Reach the file the way the loader does: open its parent directory
         * and match the entry, rather than by inode number. */
        snprintf(parent, sizeof parent, "%s", manifest[i].path);
        base = strrchr(parent, '/');
        assert_non_null(base);
        {
            char *cut = parent + (base - parent);

            *cut = '\0';
            base++;
        }

        if (parent[0] == '\0') {
            assert_int_equal(ext4_dir_open_root(&v, &d), EXT4_OK);
        } else {
            /* One level is all the manifest's regular files need beyond the
             * root, except the nested chain, which is walked component by
             * component below. */
            char comp[PATH_CAP];
            const char *p = parent + 1;

            assert_int_equal(ext4_dir_open_root(&v, &d), EXT4_OK);
            while (*p != '\0') {
                const char *slash = strchr(p, '/');
                size_t n = (slash != NULL) ? (size_t)(slash - p) : strlen(p);
                int hit = 0;

                assert_true(n < sizeof comp);
                memcpy(comp, p, n);
                comp[n] = '\0';
                for (;;) {
                    int rc = ext4_dir_next(&d, &e);

                    assert_true(rc >= 0);
                    if (rc == 0) {
                        break;
                    }
                    if (e.is_dir &&
                        strcmp((const char *)e.name, comp) == 0) {
                        ext4_dir sub;

                        assert_int_equal(ext4_dir_open(&v, &sub, &e),
                                         EXT4_OK);
                        d = sub;
                        hit = 1;
                        break;
                    }
                }
                if (!hit) {
                    fail_msg("no directory %s on the way to %s", comp,
                             manifest[i].path);
                }
                p = (slash != NULL) ? slash + 1 : p + n;
            }
        }

        for (;;) {
            int rc = ext4_dir_next(&d, &e);

            assert_true(rc >= 0);
            if (rc == 0) {
                break;
            }
            if (strcmp((const char *)e.name, base) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            fail_msg("no entry %s in the directory of %s", base,
                     manifest[i].path);
        }

        assert_int_equal(ext4_file_open(&v, &f, &e), EXT4_OK);
        size = ext4_file_size(&f);
        assert_true(size == manifest[i].size);

        sha256_init(&c);
        while (off < size) {
            uint64_t take = size - off;

            if (take > sizeof chunk) {
                take = sizeof chunk;
            }
            assert_int_equal(ext4_file_read(&f, off, take, chunk), EXT4_OK);
            sha256_update(&c, chunk, (size_t)take);
            off += take;
        }
        sha256_final(&c, digest);
        hex_of(digest, sizeof digest, hex);
        if (strcmp(hex, manifest[i].sha) != 0) {
            fail_msg("%s: sha256 %s, manifest says %s", manifest[i].path,
                     hex, manifest[i].sha);
        }
        files++;
    }
    assert_true(files >= 310);
}

/* ------------------------------------------------------- the big file */

static ext4_dirent find_in_root(ext4_vol *v, const char *name)
{
    ext4_dir d;
    ext4_dirent e;

    assert_int_equal(ext4_dir_open_root(v, &d), EXT4_OK);
    for (;;) {
        int rc = ext4_dir_next(&d, &e);

        assert_true(rc >= 0);
        if (rc == 0) {
            break;
        }
        if (strcmp((const char *)e.name, name) == 0) {
            return e;
        }
    }
    fail_msg("no /%s in the fixture root", name);
    memset(&e, 0, sizeof e);
    return e;
}

/* xorshift64*, so the "random" spans are the same on every run and a failure
 * is reproducible. */
static uint64_t rnd_state = 0x9e3779b97f4a7c15ull;

static uint64_t rnd(void)
{
    rnd_state ^= rnd_state >> 12;
    rnd_state ^= rnd_state << 25;
    rnd_state ^= rnd_state >> 27;
    return rnd_state * 0x2545F4914F6CDD1Dull;
}

/* 150 seeded spans of the 24 MiB file, each compared against pread() of the
 * copy extracted at fixture-build time. The file is written in eight
 * discontiguous extents behind an interior node, so a span chosen at random
 * usually crosses an extent boundary, which is the arithmetic worth
 * checking: a driver that resolved only the first extent would pass a
 * whole-file sequential read and fail here. */
static void test_ext4_big_file_random_spans(void **state)
{
    static uint8_t got[256u * 1024u];
    static uint8_t want[256u * 1024u];
    ext4_vol v;
    ext4_file f;
    ext4_dirent e;
    uint64_t size;
    unsigned i;

    (void)state;
    assert_int_equal(ext4_mount(&v, img_read, &img_fd), EXT4_OK);
    e = find_in_root(&v, BIG_PATH + 1);
    assert_int_equal(ext4_file_open(&v, &f, &e), EXT4_OK);
    size = ext4_file_size(&f);
    assert_true(size == 24ull * 1024ull * 1024ull);

    rnd_state = 0x9e3779b97f4a7c15ull;
    for (i = 0; i < 150u; i++) {
        uint64_t len = 1u + (rnd() % sizeof got);
        uint64_t off;
        ssize_t n;

        if (len > size) {
            len = size;
        }
        off = rnd() % (size - len + 1u);

        assert_int_equal(ext4_file_read(&f, off, len, got), EXT4_OK);
        n = pread(big_fd, want, (size_t)len, (off_t)off);
        assert_true(n == (ssize_t)len);
        if (memcmp(got, want, (size_t)len) != 0) {
            fail_msg("mismatch reading %llu bytes at %llu",
                     (unsigned long long)len, (unsigned long long)off);
        }
    }

    /* The boundaries the random spans are unlikely to hit exactly. */
    assert_int_equal(ext4_file_read(&f, size - 1u, 1, got), EXT4_OK);
    assert_int_equal(ext4_file_read(&f, 0, 1, got), EXT4_OK);
    assert_int_equal(ext4_file_read(&f, size, 0, got), EXT4_OK);
}

/* --------------------------------------------------------- htree walk */

/* mke2fs's dir_index feature turns a directory that outgrows one block into
 * an htree, and 300 entries in 1 KiB blocks does that comfortably (the
 * fixture script prints the debugfs htree_dump that proves it). This driver
 * does not read the index: it walks the blocks linearly and relies on the
 * interior blocks presenting as entries with no inode. Every name must
 * therefore appear exactly once and nothing else may leak through. */
static void test_ext4_htree_directory(void **state)
{
    static uint8_t seen[MANY_N];
    ext4_vol v;
    ext4_dir d;
    ext4_dirent e;
    unsigned n = 0;
    unsigned i;

    (void)state;
    memset(seen, 0, sizeof seen);
    assert_int_equal(ext4_mount(&v, img_read, &img_fd), EXT4_OK);
    e = find_in_root(&v, MANY_PATH + 1);
    assert_int_equal(e.is_dir, 1);
    assert_int_equal(ext4_dir_open(&v, &d, &e), EXT4_OK);

    for (;;) {
        unsigned idx;
        int rc = ext4_dir_next(&d, &e);

        assert_true(rc >= 0);
        if (rc == 0) {
            break;
        }
        assert_int_equal(e.is_regular, 1);
        if (sscanf((const char *)e.name, "entry-%3u.txt", &idx) != 1) {
            fail_msg("unexpected name in the htree directory: %s",
                     (const char *)e.name);
        }
        assert_true(idx < MANY_N);
        if (seen[idx]) {
            fail_msg("entry-%03u.txt produced twice", idx);
        }
        seen[idx] = 1;
        n++;
    }
    assert_int_equal(n, MANY_N);
    for (i = 0; i < MANY_N; i++) {
        assert_true(seen[i]);
    }

    /* Past the end the iterator keeps saying end, rather than wrapping. */
    assert_int_equal(ext4_dir_next(&d, &e), 0);
    assert_int_equal(ext4_dir_next(&d, &e), 0);
}

/* --------------------------------------------- edges and distinct refusals */

static void test_ext4_empty_dir_and_file(void **state)
{
    ext4_vol v;
    ext4_dir d;
    ext4_dirent e;
    ext4_file f;
    uint8_t buf[4];

    (void)state;
    assert_int_equal(ext4_mount(&v, img_read, &img_fd), EXT4_OK);

    /* A directory holding only "." and "..", both of which the iterator
     * suppresses, so it must end immediately rather than produce them. */
    e = find_in_root(&v, "emptydir");
    assert_int_equal(e.is_dir, 1);
    assert_int_equal(ext4_dir_open(&v, &d, &e), EXT4_OK);
    assert_int_equal(ext4_dir_next(&d, &e), 0);

    /* lost+found is empty too and is the one directory mke2fs made rather
     * than the fixture script. */
    e = find_in_root(&v, "lost+found");
    assert_int_equal(e.is_dir, 1);
    assert_int_equal(ext4_dir_open(&v, &d, &e), EXT4_OK);
    assert_int_equal(ext4_dir_next(&d, &e), 0);

    /* A zero-length file has no extents at all, so every read of it must be
     * answered from i_size rather than from the tree. */
    {
        ext4_dir sizes;
        ext4_dirent se;
        int found = 0;

        e = find_in_root(&v, "sizes");
        assert_int_equal(ext4_dir_open(&v, &sizes, &e), EXT4_OK);
        for (;;) {
            int rc = ext4_dir_next(&sizes, &se);

            assert_true(rc >= 0);
            if (rc == 0) {
                break;
            }
            if (strcmp((const char *)se.name, "s0.bin") == 0) {
                found = 1;
                break;
            }
        }
        assert_int_equal(found, 1);
        assert_int_equal(ext4_file_open(&v, &f, &se), EXT4_OK);
        assert_int_equal(ext4_file_size(&f), 0);
        assert_int_equal(ext4_file_read(&f, 0, 0, buf), EXT4_OK);
        assert_int_equal(ext4_file_read(&f, 0, 1, buf), EXT4_ERR_RANGE);
        assert_int_equal(ext4_file_read(&f, 1, 0, buf), EXT4_ERR_RANGE);
    }
}

static void test_ext4_range_and_bad_args(void **state)
{
    ext4_vol v;
    ext4_file f;
    ext4_dir d;
    ext4_dirent e;
    static uint8_t buf[8192];
    uint64_t size;

    (void)state;
    assert_int_equal(ext4_mount(&v, img_read, &img_fd), EXT4_OK);
    e = find_in_root(&v, "hello.txt");
    assert_int_equal(ext4_file_open(&v, &f, &e), EXT4_OK);
    size = ext4_file_size(&f);
    assert_true(size > 0);

    /* Wholly inside the file or nothing: a short read is never reported as
     * success, which is what lets the UKI load path trust the byte count. */
    assert_int_equal(ext4_file_read(&f, 0, size, buf), EXT4_OK);
    assert_int_equal(ext4_file_read(&f, 0, size + 1u, buf), EXT4_ERR_RANGE);
    assert_int_equal(ext4_file_read(&f, size - 1u, 2, buf), EXT4_ERR_RANGE);
    assert_int_equal(ext4_file_read(&f, size + 1u, 0, buf), EXT4_ERR_RANGE);
    assert_int_equal(ext4_file_read(&f, ~0ull, 1, buf), EXT4_ERR_RANGE);
    assert_int_equal(ext4_file_read(&f, 0, ~0ull, buf), EXT4_ERR_RANGE);

    assert_int_equal(ext4_file_read(NULL, 0, 1, buf), EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_file_read(&f, 0, 1, NULL), EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_file_read(&f, 0, 0, NULL), EXT4_OK);
    assert_int_equal(ext4_file_size(NULL), 0);

    assert_int_equal(ext4_file_open(NULL, &f, &e), EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_file_open(&v, NULL, &e), EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_file_open(&v, &f, NULL), EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_dir_open(NULL, &d, &e), EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_dir_open(&v, NULL, &e), EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_dir_open(&v, &d, NULL), EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_dir_open_root(NULL, &d), EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_dir_open_root(&v, NULL), EXT4_ERR_BAD_ARG);
    /* d has to be a real open directory before the out-pointer check is
     * reached: ext4_dir_next looks at d->vol first. */
    assert_int_equal(ext4_dir_open_root(&v, &d), EXT4_OK);
    assert_int_equal(ext4_dir_next(NULL, &e), EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_dir_next(&d, NULL), EXT4_ERR_BAD_ARG);

    /* Kind confusion in both directions. */
    {
        ext4_dirent dir_e = find_in_root(&v, "sizes");

        assert_int_equal(ext4_file_open(&v, &f, &dir_e), EXT4_ERR_BAD_ARG);
        assert_int_equal(ext4_dir_open(&v, &d, &e), EXT4_ERR_BAD_ARG);
    }

    /* The root inode is a directory, so opening it as a file is refused at
     * the inode rather than at the directory entry. */
    assert_int_equal(ext4_file_open_ino(&v, &f, EXT4_ROOT_INO),
                     EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_dir_open_ino(&v, &d, EXT4_ROOT_INO), EXT4_OK);
    assert_int_equal(ext4_file_open_ino(NULL, &f, 2), EXT4_ERR_BAD_ARG);
    assert_int_equal(ext4_dir_open_ino(NULL, &d, 2), EXT4_ERR_BAD_ARG);
}

/* A symlink is not a byte stream, and the driver says so with its own status
 * rather than reading the inode and producing the target path as if it were
 * file contents. ext4.h lists this as a deliberate non-feature. */
static void test_ext4_symlink_refused(void **state)
{
    ext4_vol v;
    ext4_file f;
    ext4_dirent e;

    (void)state;
    assert_int_equal(ext4_mount(&v, img_read, &img_fd), EXT4_OK);
    e = find_in_root(&v, "link-to-s1");
    assert_int_equal(e.is_dir, 0);
    assert_int_equal(e.is_regular, 0);
    assert_int_equal(e.file_type, 7);
    assert_int_equal(ext4_file_open(&v, &f, &e), EXT4_ERR_SYMLINK);

    /* Reached by inode number instead there is no directory entry to consult,
     * and the refusal still names the reason: the inode read itself checks
     * S_IFLNK before it looks for an extent tree. Both ways in give the same
     * status, which is what makes the status worth acting on. */
    assert_int_equal(ext4_file_open_ino(&v, &f, e.ino), EXT4_ERR_SYMLINK);
}

/* ------------------------------------------------- superblock refusals */

static void test_ext4_superblock_refusals(void **state)
{
    ext4_vol v;

    (void)state;

    /* Sanity: an unpatched copy through the same callback still mounts, so
     * every failure below is the patch and not the plumbing. */
    patch_reset();
    assert_int_equal(mount_patched(&v), EXT4_OK);

    /* Magic is checked before anything else, so this needs no checksum fix
     * and would refuse even a superblock that was otherwise perfect. */
    patch_reset();
    sb_put16(SB_MAGIC, 0);
    assert_int_equal(mount_patched(&v), EXT4_ERR_BAD_SUPER);
    patch_reset();
    sb_put16(SB_MAGIC, 0xEF54u);
    assert_int_equal(mount_patched(&v), EXT4_ERR_BAD_SUPER);

    /* A corrupted body with the stale checksum still in place. This is the
     * one case that must NOT have the checksum refreshed: it is the check
     * itself under test. */
    patch_reset();
    patch_ctx.head[SB_AT + 0x100u] ^= 0xffu;
    assert_int_equal(mount_patched(&v), EXT4_ERR_CSUM);

    /* Rev 0 has no feature flags at all, so there is nothing to check the
     * format against. */
    patch_reset();
    sb_put32(SB_REV_LEVEL, 0);
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_UNSUPPORTED);

    /* A checksum algorithm other than crc32c. */
    patch_reset();
    patch_ctx.head[SB_AT + SB_CSUM_TYPE] = 2;
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_UNSUPPORTED);

    /* Feature bits outside the supported set, one incompat and one
     * ro_compat. meta_bg moves the descriptor table, which is precisely the
     * kind of thing a reader must not guess at. */
    patch_reset();
    sb_put32(SB_INCOMPAT, sb_get32(SB_INCOMPAT) | IC_META_BG);
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_UNSUPPORTED);

    patch_reset();
    sb_put32(SB_RO_COMPAT, sb_get32(SB_RO_COMPAT) | 0x00008000u);
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_UNSUPPORTED);

    /* Extents and filetype are required rather than merely supported. */
    patch_reset();
    sb_put32(SB_INCOMPAT, sb_get32(SB_INCOMPAT) & ~0x0040u);
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_UNSUPPORTED);
    patch_reset();
    sb_put32(SB_INCOMPAT, sb_get32(SB_INCOMPAT) & ~0x0002u);
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_UNSUPPORTED);

    /* An 8 KiB block is a legal ext4 layout this driver does not carry a
     * buffer for. */
    patch_reset();
    sb_put32(SB_LOG_BLOCK_SIZE, 3);
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_UNSUPPORTED);

    /* Self-inconsistent geometry. */
    patch_reset();
    sb_put16(SB_INODE_SIZE, 0);
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_BAD_SUPER);
    patch_reset();
    sb_put16(SB_INODE_SIZE, 200);   /* not a power of two */
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_BAD_SUPER);
    patch_reset();
    sb_put32(0x14u, 0);             /* first_data_block, wrong for 1 KiB */
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_BAD_SUPER);
    patch_reset();
    sb_put32(0x20u, 0);             /* blocks_per_group */
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_BAD_SUPER);
    patch_reset();
    sb_put32(0x00u, 0);             /* inodes_count */
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_BAD_SUPER);
}

/* The three ways a filesystem says "nobody finished writing me", which get
 * two statuses between them because they mean different things to a
 * read-only driver. ext4.h argues that all three have to be checked: an
 * image snapshotted mid-write carries recover and orphan_present while
 * s_state still reads clean, so any one of them alone would miss it. */
static void test_ext4_unclean_states(void **state)
{
    ext4_vol v;

    (void)state;

    /* The journal holds committed data the filesystem does not. Reading past
     * this would hand back stale pre-journal bytes, which is worse than
     * refusing to boot, so it gets its own status. */
    patch_reset();
    sb_put32(SB_INCOMPAT, sb_get32(SB_INCOMPAT) | IC_RECOVER);
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_NEEDS_RECOVERY);

    /* The orphan file still lists inodes a mount must finish deleting. Not a
     * format we cannot read, so it is dirty rather than unsupported. */
    patch_reset();
    sb_put32(SB_RO_COMPAT, sb_get32(SB_RO_COMPAT) | RC_ORPHAN_PRESENT);
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_DIRTY);

    /* s_state, the weakest of the three and the one checked last. */
    patch_reset();
    sb_put16(SB_STATE, (uint16_t)(sb_get16(SB_STATE) & ~ST_VALID_FS));
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_DIRTY);

    patch_reset();
    sb_put16(SB_STATE, (uint16_t)(sb_get16(SB_STATE) | ST_ERROR_FS));
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_DIRTY);

    patch_reset();
    sb_put16(SB_STATE, (uint16_t)(sb_get16(SB_STATE) | ST_ORPHAN_FS));
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_DIRTY);

    /* recover is reported before orphan_present and before s_state, because
     * it is the one that says the data on disk is not the truth. */
    patch_reset();
    sb_put32(SB_INCOMPAT, sb_get32(SB_INCOMPAT) | IC_RECOVER);
    sb_put32(SB_RO_COMPAT, sb_get32(SB_RO_COMPAT) | RC_ORPHAN_PRESENT);
    sb_put16(SB_STATE, (uint16_t)(sb_get16(SB_STATE) & ~ST_VALID_FS));
    sb_refresh_csum();
    assert_int_equal(mount_patched(&v), EXT4_ERR_NEEDS_RECOVERY);

    /* The fixture itself is clean, which is what makes all of the above
     * meaningful; mk-test-ext4.sh runs fsck to guarantee it. */
    patch_reset();
    assert_int_equal(mount_patched(&v), EXT4_OK);
}

/* ------------------------------------------------------- I/O failures */

/* A stick that stops answering partway through a read is the failure this
 * driver will actually meet, and every layer of it has to say EXT4_ERR_IO
 * rather than return stale cache contents or a partial buffer. This sweeps
 * the cut-off across the first few dozen callbacks and drives a whole
 * mount/walk/read sequence at each one: whatever fails, the only two
 * acceptable answers anywhere are success and I/O.
 *
 * A sweep rather than a handful of hand-placed failures because the read
 * sites are not all obvious from the outside: the superblock, the group
 * descriptor, the inode table, an extent tree node, a directory block and a
 * file data block are six different callers and their order depends on which
 * caches are warm. */
static unsigned io_budget;
static unsigned io_used;

static int flaky_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    if (io_used >= io_budget) {
        return -1;
    }
    io_used++;
    return img_read(ctx, off, len, buf);
}

static void io_ok(int rc)
{
    if (rc != EXT4_OK && rc != EXT4_ERR_IO) {
        fail_msg("read failure after %u callbacks produced %d, which is "
                 "neither success nor EXT4_ERR_IO", io_budget, rc);
    }
}

static void test_ext4_io_failure_sweep(void **state)
{
    static uint8_t buf[8192];
    unsigned budget;
    unsigned io_seen = 0;

    (void)state;
    for (budget = 0; budget < 48u; budget++) {
        ext4_vol v;
        ext4_dir d;
        ext4_dirent e;
        int rc;

        io_budget = budget;
        io_used = 0;

        rc = ext4_mount(&v, flaky_read, &img_fd);
        io_ok(rc);
        if (rc != EXT4_OK) {
            io_seen++;
            continue;
        }
        rc = ext4_dir_open_root(&v, &d);
        io_ok(rc);
        if (rc != EXT4_OK) {
            io_seen++;
            continue;
        }
        for (;;) {
            ext4_file f;

            rc = ext4_dir_next(&d, &e);
            if (rc <= 0) {
                io_ok(rc == 0 ? EXT4_OK : rc);
                break;
            }
            if (!e.is_regular) {
                continue;
            }
            rc = ext4_file_open(&v, &f, &e);
            io_ok(rc);
            if (rc != EXT4_OK) {
                break;
            }
            /* Both the aligned bulk path and the ragged one. */
            rc = ext4_file_read(&f, 0, (ext4_file_size(&f) < sizeof buf)
                                ? ext4_file_size(&f) : sizeof buf, buf);
            io_ok(rc);
            if (rc != EXT4_OK) {
                break;
            }
            if (ext4_file_size(&f) > 1u) {
                rc = ext4_file_read(&f, 1, 1, buf);
                io_ok(rc);
                if (rc != EXT4_OK) {
                    break;
                }
            }
        }
        if (rc == EXT4_ERR_IO) {
            io_seen++;
        }
    }
    /* The sweep is only worth anything if it actually starved the driver. */
    assert_true(io_seen > 8);
}

/* ------------------------------------------------------------------ run */

int run_ext4_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_ext4_mount_geometry),
        cmocka_unit_test(test_ext4_walk_matches_manifest),
        cmocka_unit_test(test_ext4_file_hashes),
        cmocka_unit_test(test_ext4_big_file_random_spans),
        cmocka_unit_test(test_ext4_htree_directory),
        cmocka_unit_test(test_ext4_empty_dir_and_file),
        cmocka_unit_test(test_ext4_range_and_bad_args),
        cmocka_unit_test(test_ext4_symlink_refused),
        cmocka_unit_test(test_ext4_superblock_refusals),
        cmocka_unit_test(test_ext4_unclean_states),
        cmocka_unit_test(test_ext4_io_failure_sweep),
    };
    int failed;

    img_fd = open(EXT4_IMG, O_RDONLY);
    if (img_fd < 0) {
        printf("\n[  SKIPPED ] ext4 fixture: %s not found.\n"
               "             Build it with: tools/mk-test-ext4.sh "
               "(needs mkfs.ext4 and sudo).\n", EXT4_IMG);
        return 0;
    }
    big_fd = open(EXT4_BIGFILE, O_RDONLY);
    if (big_fd < 0 || load_manifest() != 0) {
        printf("\n[  SKIPPED ] ext4 fixture: %s or %s missing or "
               "unreadable.\n"
               "             Rebuild both with: tools/mk-test-ext4.sh\n",
               EXT4_MANIFEST, EXT4_BIGFILE);
        if (big_fd >= 0) {
            close(big_fd);
        }
        close(img_fd);
        img_fd = -1;
        return 0;
    }

    failed = cmocka_run_group_tests_name("ext4", tests, NULL, NULL);

    close(big_fd);
    close(img_fd);
    big_fd = -1;
    img_fd = -1;
    return failed;
}
