/* Fixture-backed tests for src/exfat/exfat.c.
 *
 * These run against build/test-exfat.img, the 256 MiB whole-device exFAT
 * volume tools/mk-test-exfat.sh produces with mkfs.exfat and the kernel's
 * exfat driver. Where the synthetic tests prove the driver rejects what it
 * should, these prove it agrees with a real implementation: entry-set
 * layout, name splitting across name entries, and the directory tree that
 * script writes.
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
#include <string.h>
#include <unistd.h>

#include <exfat/exfat.h>

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

static int group_teardown(void **state)
{
    (void)state;
    if (img_fd >= 0)
        close(img_fd);
    img_fd = -1;
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

/* Names tools/mk-test-exfat.sh writes only when the corresponding artifact
 * exists, so their absence is not a failure and their presence must not be
 * reported as an unexpected entry. */
static int is_optional(const char *path)
{
    /* Copied in only when build/test-payload.iso has been built. */
    return strcmp(path, "payload-test.iso") == 0;
}

static void test_fixture_tree(void **state)
{
    /* The unconditional set tools/mk-test-exfat.sh writes. Size 0 means "not
     * predictable" (directories, whose size is the cluster allocation). */
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
    };
    int fd = open(FIXTURE_IMG, O_RDONLY);

    if (fd < 0) {
        printf("\n[  SKIPPED ] exfat fixture: %s not found.\n"
               "             Build it with: tools/mk-test-exfat.sh "
               "(needs mkfs.exfat and sudo),\n"
               "             or point FIXTURE_IMG at an existing image.\n",
               FIXTURE_IMG);
        return 0;
    }
    close(fd);

    return cmocka_run_group_tests_name("exfat fixture", tests, group_setup,
                                       group_teardown);
}
