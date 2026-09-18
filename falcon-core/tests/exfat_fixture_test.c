/* Fixture-backed tests for falcon-core/src/fs/exfat.c.
 *
 * These run against build/test-exfat.img, the 256 MiB whole-device exFAT volume
 * falcon-core/tools/mk-test-exfat.sh produces with mkfs.exfat and the kernel's
 * exfat driver. Where the synthetic tests prove the driver rejects what it
 * should, these prove it agrees with a real implementation: entry-set layout,
 * name splitting across name entries, and the directory tree that script
 * writes.
 *
 * If the image is absent the whole group is skipped with an explanatory
 * message rather than failing, so a fresh checkout can run the suite before
 * anyone has been asked for a sudo password.
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
#include <sys/wait.h>
#include <unistd.h>

#include <fs/exfat.h>

#ifndef FIXTURE_IMG
#define FIXTURE_IMG "build/test-exfat.img"
#endif

#define FIXTURE_BYTES (256ull * 1024ull * 1024ull)
#define ISO_BYTES     (1024ull * 1024ull) /* mkiso() writes 1 MiB of zeroes */
#define TXT_BYTES     11ull               /* "not an ISO\n" */

#define WALK_MAX   64
#define PATH_MAX_T 256

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

/* ---------------------------------------------------------------- walk */

typedef struct {
    char     path[PATH_MAX_T];
    uint8_t  is_dir;
    uint64_t size;
    uint8_t  name_len;
} walk_ent;

typedef struct {
    walk_ent ents[WALK_MAX];
    unsigned n;
} walk_result;

/* Depth-first walk of the whole volume. Recursion depth is bounded by the
 * fixture's own tree (deep/a/b/c is the deepest), and each exfat_dir frame is
 * ~4.5 KB, which is why the loader caps depth rather than recursing freely. */
static void walk(exfat_vol *v, exfat_dir *d, const char *prefix,
                 walk_result *out, unsigned depth)
{
    exfat_dirent e;
    int rc;

    assert_true(depth < 8);
    for (;;) {
        char name[PATH_MAX_T];
        walk_ent *w;

        rc = exfat_dir_next(d, &e);
        assert_true(rc >= 0);
        if (rc == 0)
            break;

        ucs2_to_ascii(e.name, name, sizeof(name));
        assert_true(out->n < WALK_MAX);
        w = &out->ents[out->n++];
        snprintf(w->path, sizeof(w->path), "%s%s%s", prefix,
                 prefix[0] != '\0' ? "/" : "", name);
        w->is_dir = e.is_dir;
        w->size = e.size;
        w->name_len = e.name_len;

        if (e.is_dir) {
            exfat_dir sub;

            assert_int_equal(exfat_dir_open(v, &sub, &e), EXFAT_OK);
            walk(v, &sub, w->path, out, depth + 1);
        }
    }
}

static const walk_ent *find(const walk_result *r, const char *path)
{
    unsigned i;

    for (i = 0; i < r->n; i++) {
        if (strcmp(r->ents[i].path, path) == 0)
            return &r->ents[i];
    }
    return NULL;
}

static void dump(const walk_result *r)
{
    unsigned i;

    printf("  walk found %u entries:\n", r->n);
    for (i = 0; i < r->n; i++)
        printf("    %-40s %s size=%llu\n", r->ents[i].path,
               r->ents[i].is_dir ? "dir " : "file",
               (unsigned long long)r->ents[i].size);
}

/* ------------------------------------------------------------ fixtures */

static int group_setup(void **state)
{
    (void)state;
    img_fd = open(FIXTURE_IMG, O_RDONLY);
    return 0;
}

static void ref_close_all(void);

static int group_teardown(void **state)
{
    (void)state;
    if (img_fd >= 0)
        close(img_fd);
    img_fd = -1;
    ref_close_all();
    return 0;
}

static void mount_fixture(exfat_vol *v)
{
    assert_true(img_fd >= 0);
    assert_int_equal(exfat_mount(v, img_read, &img_fd), EXFAT_OK);
}

/* --------------------------------------------------------------- tests */

static void test_fixture_mount_geometry(void **state)
{
    exfat_vol v;
    uint64_t sector;
    uint64_t cluster;
    uint64_t heap_end;

    (void)state;
    mount_fixture(&v);

    /* Exact values depend on the mkfs.exfat version, so assert the
     * invariants the driver itself relies on rather than pinning numbers a
     * fixture regeneration would change. */
    assert_true(v.bps_shift >= 9 && v.bps_shift <= 12);
    assert_true(v.spc_shift <= 25u - v.bps_shift);
    sector = (uint64_t)1 << v.bps_shift;
    cluster = (uint64_t)1 << (v.bps_shift + v.spc_shift);
    assert_true(sector <= EXFAT_MAX_SECTOR);

    assert_true(v.fat_offset > 0);
    assert_true(v.cluster_heap_offset > v.fat_offset);
    assert_true(v.cluster_count > 0);
    /* Root must be a real cluster. */
    assert_true(v.root_cluster >= 2);
    assert_true(v.root_cluster < v.cluster_count + 2);

    /* The heap has to fit inside the 256 MiB image. */
    heap_end = (uint64_t)v.cluster_heap_offset * sector +
               (uint64_t)v.cluster_count * cluster;
    assert_true(heap_end <= FIXTURE_BYTES);
    /* ...and the FAT has to have room for every cluster's entry. */
    assert_true((uint64_t)v.fat_offset * sector +
                    (uint64_t)(v.cluster_count + 2) * 4u <=
                (uint64_t)v.cluster_heap_offset * sector);
}

/* Names falcon-core/tools/mk-test-exfat.sh writes only when the corresponding
 * artifact exists, so their absence is not a failure and their presence must
 * not be reported as an unexpected entry. */
static int is_optional(const char *path)
{
    /* Copied in only when build/test-payload.iso has been built. The nested
     * one is the same image again, three folders down, for the loader's
     * folder view. */
    return strcmp(path, "payload-test.iso") == 0 ||
           strcmp(path, "deep/a/b/payload-nested.iso") == 0 ||
           strcmp(path, "payload-gpt.img") == 0 ||
           strcmp(path, "payload-floppy.img") == 0;
}

static void test_fixture_tree(void **state)
{
    /* The unconditional set falcon-core/tools/mk-test-exfat.sh writes. Size 0
     * means "not predictable" (directories, whose size is the cluster
     * allocation). */
    static const struct {
        const char *path;
        uint8_t     is_dir;
        uint64_t    size;
    } expect[] = {
        { "archlinux-fake.iso",               0, ISO_BYTES },
        { "systemrescue-fake.iso",            0, ISO_BYTES },
        { "notaniso.txt",                     0, TXT_BYTES },
        { "Linux",                            1, 0 },
        { "Linux/ubuntu-fake.iso",            0, ISO_BYTES },
        { "Linux/fedora-fake.iso",            0, ISO_BYTES },
        { "Linux/rescue",                     1, 0 },
        { "Linux/rescue/gparted-fake.iso",    0, ISO_BYTES },
        { "notes",                            1, 0 },
        { "notes/readme.txt",                 0, TXT_BYTES },
        { "deep",                             1, 0 },
        { "deep/a",                           1, 0 },
        { "deep/a/b",                         1, 0 },
        { "deep/a/b/justdeepenough-fake.iso", 0, ISO_BYTES },
        { "deep/a/b/c",                       1, 0 },
        { "deep/a/b/c/toodeep-fake.iso",      0, ISO_BYTES },
        { ".hiddendir",                       1, 0 },
        { ".hiddendir/hidden-fake.iso",       0, ISO_BYTES },
    };
    exfat_vol v;
    exfat_dir root;
    walk_result r;
    uint64_t cluster;
    size_t i;
    unsigned j;

    (void)state;
    mount_fixture(&v);
    cluster = (uint64_t)1 << (v.bps_shift + v.spc_shift);
    memset(&r, 0, sizeof(r));
    assert_int_equal(exfat_dir_open_root(&v, &root), EXFAT_OK);
    walk(&v, &root, "", &r, 0);

    /* Nothing may appear that the fixture script does not write. Checked as a
     * set membership rather than a count so that an optional artifact does
     * not turn into a spurious failure. */
    for (j = 0; j < r.n; j++) {
        int known = is_optional(r.ents[j].path);

        for (i = 0; !known && i < sizeof(expect) / sizeof(expect[0]); i++)
            known = strcmp(r.ents[j].path, expect[i].path) == 0;
        if (!known) {
            printf("  unexpected entry: %s\n", r.ents[j].path);
            dump(&r);
        }
        assert_int_equal(known, 1);
    }

    for (i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        const walk_ent *w = find(&r, expect[i].path);

        if (w == NULL) {
            printf("  missing from walk: %s\n", expect[i].path);
            dump(&r);
        }
        assert_non_null(w);
        assert_int_equal(w->is_dir, expect[i].is_dir);
        assert_int_equal(w->name_len, strlen(strrchr(expect[i].path, '/')
                                                 ? strrchr(expect[i].path,
                                                           '/') + 1
                                                 : expect[i].path));
        if (expect[i].is_dir) {
            /* A directory's reported size is its DataLength, which is always
             * a whole number of clusters and never zero. */
            assert_true(w->size > 0);
            assert_true(w->size % cluster == 0);
        } else {
            assert_true(w->size == expect[i].size);
        }
    }
}

/* Names longer than 15 UTF-16 units are split across several name entries on
 * disk; the driver has to stitch them back together exactly. */
static void test_fixture_long_names_round_trip(void **state)
{
    static const char *const longnames[] = {
        "archlinux-fake.iso",               /* 18 -> 2 name entries */
        "systemrescue-fake.iso",            /* 21 -> 2 name entries */
        "deep/a/b/justdeepenough-fake.iso", /* 23 -> 2 name entries */
    };
    exfat_vol v;
    exfat_dir root;
    walk_result r;
    size_t i;

    (void)state;
    mount_fixture(&v);
    memset(&r, 0, sizeof(r));
    assert_int_equal(exfat_dir_open_root(&v, &root), EXFAT_OK);
    walk(&v, &root, "", &r, 0);

    for (i = 0; i < sizeof(longnames) / sizeof(longnames[0]); i++) {
        const char *base = strrchr(longnames[i], '/');
        const walk_ent *w = find(&r, longnames[i]);

        base = base ? base + 1 : longnames[i];
        assert_non_null(w);
        assert_true(strlen(base) > 15);
        /* name_len is what the driver reassembled; the path comparison in
         * find() already proved the characters match byte for byte. */
        assert_int_equal(w->name_len, strlen(base));
    }
}

/* Every directory in the fixture is non-empty, so there is no on-disk empty
 * directory to iterate here; that case is covered synthetically by
 * test_dir_unallocated_is_empty and test_dir_empty_end_marker_first. What
 * this does check is the other end of the walk: the deepest directory
 * terminates cleanly and repeatedly at its end marker. */
static void test_fixture_leaf_directory_terminates(void **state)
{
    exfat_vol v;
    exfat_dir root;
    exfat_dir sub;
    exfat_dirent e;
    int rc;
    int found = 0;

    (void)state;
    mount_fixture(&v);
    assert_int_equal(exfat_dir_open_root(&v, &root), EXFAT_OK);

    for (;;) {
        char name[PATH_MAX_T];

        rc = exfat_dir_next(&root, &e);
        assert_true(rc >= 0);
        if (rc == 0)
            break;
        ucs2_to_ascii(e.name, name, sizeof(name));
        if (strcmp(name, ".hiddendir") != 0)
            continue;

        found = 1;
        assert_int_equal(e.is_dir, 1);
        assert_int_equal(exfat_dir_open(&v, &sub, &e), EXFAT_OK);
        assert_int_equal(exfat_dir_next(&sub, &e), 1);
        ucs2_to_ascii(e.name, name, sizeof(name));
        assert_string_equal(name, "hidden-fake.iso");
        assert_true(e.size == ISO_BYTES);
        assert_int_equal(exfat_dir_next(&sub, &e), 0);
        break;
    }
    assert_int_equal(found, 1);
}

/* A file entry is not a directory, whatever the caller believes. */
static void test_fixture_open_file_as_dir_rejected(void **state)
{
    exfat_vol v;
    exfat_dir root;
    exfat_dir sub;
    exfat_dirent e;
    int found = 0;

    (void)state;
    mount_fixture(&v);
    assert_int_equal(exfat_dir_open_root(&v, &root), EXFAT_OK);
    while (exfat_dir_next(&root, &e) == 1) {
        if (e.is_dir)
            continue;
        found = 1;
        assert_int_equal(exfat_dir_open(&v, &sub, &e), EXFAT_ERR_BAD_ARG);
    }
    assert_int_equal(found, 1);
}

/* A closed descriptor makes every read fail, which must surface as
 * EXFAT_ERR_IO rather than a bogus mount. */
static void test_fixture_read_failure_is_io(void **state)
{
    exfat_vol v;
    int bad_fd = -1;

    (void)state;
    assert_int_equal(exfat_mount(&v, img_read, &bad_fd), EXFAT_ERR_IO);
}

/* ------------------------------------------------------ file contents */

/* The reference copy of a file, produced by the kernel's own exFAT driver.
 * Comparing against it is what makes these tests worth more than the
 * synthetic ones: they prove the cluster walk lands on the same bytes a
 * production implementation hands out, not merely on self-consistent ones. */
typedef struct {
    const char *path;   /* path inside the volume */
    int         fd;     /* extracted reference copy, -1 until pulled out */
    char        tmp[96];
} ref_file;

/* archlinux-fake.iso is the mandated 1 MiB target; its content is zeroes, so it
 * proves lengths, offsets and error-free traversal but cannot catch a read that
 * returns the right count of bytes from the wrong cluster. payload-test.iso is
 * the real 8 MB El Torito image falcon-efi/tools/mk-test-iso.sh builds, present
 * only when that has been run: its content is structured, so a misplaced span
 * changes the bytes. Both are checked when both exist. */
static ref_file refs[] = {
    { "archlinux-fake.iso", -1, { 0 } },
    { "payload-test.iso",   -1, { 0 } },
};
#define REF_COUNT (sizeof(refs) / sizeof(refs[0]))

/* Copies one file out of the image via a read-only loop mount. Everything
 * happens inside one shell with a trap, so no mount and no mountpoint outlives
 * this call even if the test binary dies immediately afterwards. sudo runs
 * with -n: without passwordless sudo this fails fast instead of stalling a
 * test run on a password prompt. */
static int ref_open(ref_file *r)
{
    char cmd[1024];
    int rc;

    if (r->fd >= 0)
        return r->fd;
    if (r->tmp[0] != '\0')
        return -1; /* already tried and failed */

    snprintf(r->tmp, sizeof(r->tmp), "/tmp/falcon-exfat-ref-%ld-%p.bin",
             (long)getpid(), (const void *)r);
    snprintf(cmd, sizeof(cmd),
             "set -e; "
             "mp=$(mktemp -d /tmp/falcon-exfat-mnt-XXXXXX); "
             "trap 'sudo -n umount \"$mp\" >/dev/null 2>&1; "
             "rmdir \"$mp\" >/dev/null 2>&1' EXIT; "
             "sudo -n mount -o ro,loop '%s' \"$mp\"; "
             "sudo -n cat \"$mp/%s\" > '%s'",
             FIXTURE_IMG, r->path, r->tmp);

    rc = system(cmd);
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0)
        return -1;
    r->fd = open(r->tmp, O_RDONLY);
    return r->fd;
}

static void ref_close_all(void)
{
    size_t i;

    for (i = 0; i < REF_COUNT; i++) {
        if (refs[i].fd >= 0)
            close(refs[i].fd);
        refs[i].fd = -1;
        if (refs[i].tmp[0] != '\0')
            (void)unlink(refs[i].tmp);
        refs[i].tmp[0] = '\0';
    }
}

/* Looks a name up in the root directory. Every file these tests compare lives
 * there, so no path splitting is needed. */
static int find_in_root(exfat_vol *v, const char *want, exfat_dirent *out)
{
    exfat_dir root;
    exfat_dirent e;

    assert_int_equal(exfat_dir_open_root(v, &root), EXFAT_OK);
    while (exfat_dir_next(&root, &e) == 1) {
        char name[PATH_MAX_T];

        ucs2_to_ascii(e.name, name, sizeof(name));
        if (strcmp(name, want) == 0) {
            *out = e;
            return 1;
        }
    }
    return 0;
}

/* Reads through the driver and through pread, and asserts the two agree. */
static void compare_span(exfat_file *f, int ref_fd, uint64_t off, uint64_t len,
                         uint8_t *got, uint8_t *want)
{
    ssize_t n;

    assert_int_equal(exfat_file_read(f, off, len, got), EXFAT_OK);
    if (len == 0)
        return;
    n = pread(ref_fd, want, (size_t)len, (off_t)off);
    assert_true(n == (ssize_t)len);
    assert_memory_equal(got, want, (size_t)len);
}

/* Every byte of a real file, in one call. */
static void test_fixture_file_read_whole_file(void **state)
{
    exfat_vol v;
    size_t i;
    unsigned checked = 0;

    (void)state;
    mount_fixture(&v);

    for (i = 0; i < REF_COUNT; i++) {
        exfat_dirent e;
        exfat_file f;
        uint8_t *got;
        uint8_t *want;
        int fd;

        if (!find_in_root(&v, refs[i].path, &e))
            continue; /* optional artifact, not in this image */
        fd = ref_open(&refs[i]);
        if (fd < 0) {
            printf("  cannot extract %s (needs passwordless sudo), "
                   "skipping it\n", refs[i].path);
            continue;
        }

        assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);
        assert_true(f.size == e.size);
        got = malloc((size_t)e.size);
        want = malloc((size_t)e.size);
        assert_non_null(got);
        assert_non_null(want);
        compare_span(&f, fd, 0, e.size, got, want);
        free(got);
        free(want);
        checked++;
    }

    if (checked == 0)
        skip();
}

/* 200 reads at pseudo-random offsets and lengths. The sequence is seeded with
 * a constant so a failure is reproducible, and the offsets move both forward
 * and backward, which exercises the chain cursor's resume and restart paths
 * against a real allocation rather than a synthesised one. */
static void test_fixture_file_read_random_spans(void **state)
{
    enum { SPANS = 200, SPAN_MAX = 64u * 1024u };
    exfat_vol v;
    size_t i;
    unsigned checked = 0;

    (void)state;
    mount_fixture(&v);

    for (i = 0; i < REF_COUNT; i++) {
        exfat_dirent e;
        exfat_file f;
        uint8_t *got;
        uint8_t *want;
        uint64_t seed = 0x5EED1F7EDD1E5ull;
        int fd;
        unsigned k;

        if (!find_in_root(&v, refs[i].path, &e))
            continue;
        fd = ref_open(&refs[i]);
        if (fd < 0)
            continue;

        assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);
        assert_true(e.size > SPAN_MAX);
        got = malloc(SPAN_MAX);
        want = malloc(SPAN_MAX);
        assert_non_null(got);
        assert_non_null(want);

        for (k = 0; k < SPANS; k++) {
            uint64_t off;
            uint64_t len;
            uint64_t room;

            /* Knuth's LCG: deterministic, and good enough to scatter the
             * offsets across the file. */
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            off = (seed >> 16) % e.size;
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            room = e.size - off;
            if (room > SPAN_MAX)
                room = SPAN_MAX;
            len = (seed >> 16) % (room + 1u);

            compare_span(&f, fd, off, len, got, want);
        }

        free(got);
        free(want);
        checked++;
    }

    if (checked == 0)
        skip();
}

/* ------------------------------------------------------------ extent map */

/* Rebuilds one file from its extent list by reading the named volume ranges
 * straight off the image, and compares that against what the driver's read
 * path returns for the same file.
 *
 * The two answers share nothing below the dirent: one walks the FAT and
 * copies through the driver's sector cache, the other is a list of byte
 * ranges handed to pread. That is the point. On a real mkfs.exfat volume the
 * synthetic tests' hand-built fragmentation is gone, so what is left to prove
 * here is that the map agrees with reality on an allocation nobody designed
 * for the test. */
static void check_file_extents(exfat_vol *v, const exfat_dirent *e,
                               const char *path)
{
    exfat_extent *ex;
    exfat_file f;
    uint8_t *from_extents;
    uint8_t *from_read;
    uint64_t total = 0;
    size_t count = 0;
    size_t got = 0;
    size_t i;
    int rc;

    /* The sizing call, against real data. */
    rc = exfat_file_extents(v, e, NULL, 0, &count);
    if (e->size == 0) {
        assert_int_equal(rc, EXFAT_OK);
        assert_int_equal(count, 0);
        return;
    }
    assert_int_equal(rc, EXFAT_ERR_NOSPACE);
    assert_true(count > 0);

    ex = malloc(count * sizeof(*ex));
    assert_non_null(ex);
    assert_int_equal(exfat_file_extents(v, e, ex, count, &got), EXFAT_OK);
    assert_int_equal(got, count);

    for (i = 0; i < count; i++) {
        assert_true(ex[i].len > 0);
        /* Every range has to be inside the volume, or the mapping would send
         * a consumer off the end of the partition. */
        assert_true(ex[i].vol_off < FIXTURE_BYTES);
        assert_true(ex[i].len <= FIXTURE_BYTES - ex[i].vol_off);
        total += ex[i].len;
    }
    /* The lengths cover the valid data length exactly: the tail of the final
     * cluster is not included. */
    assert_true(total == e->size);

    from_extents = malloc((size_t)e->size);
    from_read = malloc((size_t)e->size);
    assert_non_null(from_extents);
    assert_non_null(from_read);

    total = 0;
    for (i = 0; i < count; i++) {
        assert_int_equal(img_read(&img_fd, ex[i].vol_off, ex[i].len,
                                  from_extents + total), 0);
        total += ex[i].len;
    }

    assert_int_equal(exfat_file_open(v, &f, e), EXFAT_OK);
    assert_int_equal(exfat_file_read(&f, 0, e->size, from_read), EXFAT_OK);
    if (memcmp(from_extents, from_read, (size_t)e->size) != 0)
        printf("  extent reassembly differs from the read path for %s\n",
               path);
    assert_memory_equal(from_extents, from_read, (size_t)e->size);

    free(from_extents);
    free(from_read);
    free(ex);
}

/* Depth-first over the whole volume, checking every file it finds. Nothing is
 * hardcoded about which files exist, so an optional artifact or a change to
 * falcon-core/tools/mk-test-exfat.sh widens the coverage instead of breaking
 * the test. */
static unsigned extent_walk(exfat_vol *v, exfat_dir *d, const char *prefix,
                            unsigned depth)
{
    exfat_dirent e;
    unsigned checked = 0;
    int rc;

    assert_true(depth < 8);
    for (;;) {
        char name[PATH_MAX_T];
        char path[PATH_MAX_T];

        rc = exfat_dir_next(d, &e);
        assert_true(rc >= 0);
        if (rc == 0)
            break;

        ucs2_to_ascii(e.name, name, sizeof(name));
        snprintf(path, sizeof(path), "%s%s%s", prefix,
                 prefix[0] != '\0' ? "/" : "", name);

        if (e.is_dir) {
            exfat_dir sub;

            /* A directory has an allocation but no extent map: the call is
             * refused rather than answered, on a real dirent as well as a
             * synthetic one. */
            {
                size_t count = 99;

                assert_int_equal(exfat_file_extents(v, &e, NULL, 0, &count),
                                 EXFAT_ERR_BAD_ARG);
            }
            assert_int_equal(exfat_dir_open(v, &sub, &e), EXFAT_OK);
            checked += extent_walk(v, &sub, path, depth + 1);
            continue;
        }

        check_file_extents(v, &e, path);
        checked++;
    }
    return checked;
}

static void test_fixture_extents_every_file(void **state)
{
    exfat_vol v;
    exfat_dir root;
    unsigned checked;

    (void)state;
    mount_fixture(&v);
    assert_int_equal(exfat_dir_open_root(&v, &root), EXFAT_OK);
    checked = extent_walk(&v, &root, "", 0);
    /* The fixture script always writes more than one file; zero here would
     * mean the walk found nothing and the test proved nothing. */
    assert_true(checked >= 8u);
}

/* The range contract holds on a real file too: the last byte is readable, one
 * past it is not, and neither is a length that would run off the end. */
static void test_fixture_file_read_range_guard(void **state)
{
    exfat_vol v;
    exfat_dirent e;
    exfat_file f;
    uint8_t buf[64];

    (void)state;
    mount_fixture(&v);
    assert_int_equal(find_in_root(&v, "archlinux-fake.iso", &e), 1);
    assert_true(e.size == ISO_BYTES);
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);

    assert_int_equal(exfat_file_read(&f, e.size - 1u, 1, buf), EXFAT_OK);
    assert_int_equal(exfat_file_read(&f, e.size, 1, buf), EXFAT_ERR_RANGE);
    assert_int_equal(exfat_file_read(&f, e.size, 0, buf), EXFAT_OK);
    assert_int_equal(exfat_file_read(&f, e.size - 8u, 9, buf),
                     EXFAT_ERR_RANGE);
    assert_int_equal(exfat_file_read(&f, 0, e.size + 1u, buf),
                     EXFAT_ERR_RANGE);
}

/* A directory entry is not a file, the mirror of
 * test_fixture_open_file_as_dir_rejected. */
static void test_fixture_open_dir_as_file_rejected(void **state)
{
    exfat_vol v;
    exfat_dir root;
    exfat_dirent e;
    exfat_file f;
    int found = 0;

    (void)state;
    mount_fixture(&v);
    assert_int_equal(exfat_dir_open_root(&v, &root), EXFAT_OK);
    while (exfat_dir_next(&root, &e) == 1) {
        if (!e.is_dir)
            continue;
        found = 1;
        assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_ERR_BAD_ARG);
    }
    assert_int_equal(found, 1);
}

/* A read that fails at the device must surface as EXFAT_ERR_IO, not as a
 * buffer of whatever the caller passed in. */
static void test_fixture_file_read_io_error(void **state)
{
    exfat_vol v;
    exfat_dirent e;
    exfat_file f;
    uint8_t buf[512];
    int bad_fd = -1;

    (void)state;
    mount_fixture(&v);
    assert_int_equal(find_in_root(&v, "archlinux-fake.iso", &e), 1);
    assert_int_equal(exfat_file_open(&v, &f, &e), EXFAT_OK);
    /* Swap the descriptor out from under the mounted volume. */
    v.read_ctx = &bad_fd;
    assert_int_equal(exfat_file_read(&f, 0, sizeof(buf), buf), EXFAT_ERR_IO);
}

/* ------------------------------------------------------------------ run */

int run_fixture_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_fixture_mount_geometry),
        cmocka_unit_test(test_fixture_tree),
        cmocka_unit_test(test_fixture_long_names_round_trip),
        cmocka_unit_test(test_fixture_leaf_directory_terminates),
        cmocka_unit_test(test_fixture_open_file_as_dir_rejected),
        cmocka_unit_test(test_fixture_read_failure_is_io),
        cmocka_unit_test(test_fixture_open_dir_as_file_rejected),
        cmocka_unit_test(test_fixture_file_read_range_guard),
        cmocka_unit_test(test_fixture_file_read_io_error),
        cmocka_unit_test(test_fixture_file_read_whole_file),
        cmocka_unit_test(test_fixture_file_read_random_spans),
        cmocka_unit_test(test_fixture_extents_every_file),
    };
    int fd = open(FIXTURE_IMG, O_RDONLY);

    if (fd < 0) {
        printf("\n[  SKIPPED ] exfat fixture: %s not found.\n"
               "             Build it with: make test-image "
               "(needs mkfs.exfat and sudo),\n"
               "             or point FIXTURE_IMG at an existing image.\n",
               FIXTURE_IMG);
        return test_skip_rc();
    }
    close(fd);

    return cmocka_run_group_tests_name("exfat fixture", tests, group_setup,
                                       group_teardown);
}
