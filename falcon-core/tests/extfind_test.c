/* Tests for falcon-core/src/media/extfind.c (which disk holds the partition
 * an extent map names).
 *
 * The disks are synthetic: an exFAT volume from the synth builder placed at
 * a byte offset inside a zero-filled "disk", with a /.falcon file written
 * into its root. Every way the finder can be told "not this one" is walked,
 * because each of them is a stick that is not the one the loader booted.
 * One more is a FAT32 library off a fixture image (mk-test-fat.sh), whose
 * record carries g_id.
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

#include <ident/identfs.h>
#include <media/extfind.h>

#define BPS_SHIFT 9u
#define SPC_SHIFT 2u
#define CLUSTERS  32u
#define REC_CLUSTER 3u
#define PART_LBA  2048u

static const uint8_t g_id[16] = {
    0x5a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x0f
};

/* A disk: nothing before the partition, the synth volume from its first
 * byte, and a read failure anywhere past the volume's end. */
typedef struct {
    synth_vol *sv;
    uint64_t   base;
} disk;

static int disk_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const disk *d = ctx;
    uint8_t *out = buf;

    while (len > 0) {
        if (off < d->base) {
            *out = 0;
        } else if (off - d->base < d->sv->len) {
            *out = d->sv->buf[off - d->base];
        } else {
            return -1;
        }
        out++;
        off++;
        len--;
    }
    return 0;
}

/* The volume with a root entry named `name` holding `text`, or no entry at
 * all when name is NULL. dir makes the entry a directory instead. */
static void volume(synth_vol *sv, const char *name, const char *text, int dir)
{
    uint8_t *root;
    unsigned n = 0;
    size_t len = text != NULL ? strlen(text) : 0u;

    synth_init(sv, BPS_SHIFT, SPC_SHIFT, CLUSTERS);
    root = synth_cluster(sv, sv->root_cluster);
    if (name != NULL) {
        n = synth_put_set(root, name, dir ? SYNTH_ATTR_DIR : 0u,
                          (uint8_t)(SYNTH_FLAG_ALLOC | SYNTH_FLAG_NOFAT),
                          REC_CLUSTER,
                          dir ? synth_cluster_bytes(sv) : (uint64_t)len);
        if (dir)
            synth_put_end(synth_cluster(sv, REC_CLUSTER));
        else
            memcpy(synth_cluster(sv, REC_CLUSTER), text, len);
    }
    synth_put_end(root + n * SYNTH_ENT);
}

static void record_text(ident_role role, const uint8_t *id, char *out)
{
    ident_rec r;

    memset(&r, 0, sizeof r);
    r.version = IDENT_VERSION;
    r.role = role;
    memcpy(r.install_id, id, 16);
    assert_true(ident_encode_text(&r, out, IDENT_TEXT_MAX) > 0u);
}

static void map_for(extmap_hdr *h, const uint8_t *id)
{
    memset(h, 0, sizeof *h);
    memcpy(h->install_id, id, 16);
    h->part_lba = PART_LBA;
    h->block_size = 512;
    h->n_extents = 1;
    h->iso_size = 4096;
}

static ident_fs_scratch g_s;

/* ---- extfind ---- */

static void test_extfind_names(void **state)
{
    extmap_hdr h;
    ident_rec r;

    (void)state;
    map_for(&h, g_id);
    memset(&r, 0, sizeof r);
    r.version = IDENT_VERSION;
    r.role = IDENT_ROLE_LIBRARY;
    memcpy(r.install_id, g_id, 16);
    assert_true(extmap_names(&h, &r));
    r.role = IDENT_ROLE_SHELF;
    assert_false(extmap_names(&h, &r));
    r.role = IDENT_ROLE_BOOT;
    assert_false(extmap_names(&h, &r));
    r.role = IDENT_ROLE_LIBRARY;
    r.install_id[15] ^= 1u;
    assert_false(extmap_names(&h, &r));
    assert_false(extmap_names(NULL, &r));
    assert_false(extmap_names(&h, NULL));
}

static void test_extfind_finds_the_library(void **state)
{
    synth_vol sv;
    char text[IDENT_TEXT_MAX];
    extmap_hdr h;
    disk d;

    (void)state;
    record_text(IDENT_ROLE_LIBRARY, g_id, text);
    volume(&sv, ".falcon", text, 0);
    d.sv = &sv;
    d.base = (uint64_t)PART_LBA * 512u;
    map_for(&h, g_id);

    assert_int_equal(extmap_find_on_disk(&h, disk_read, &d, 512, &g_s),
                     EXTMAP_OK);
    /* A reader that cannot learn the block size takes the map's. */
    assert_int_equal(extmap_find_on_disk(&h, disk_read, &d, 0, &g_s),
                     EXTMAP_OK);
    /* Nothing is left mounted over the reader's frame. */
    assert_null(g_s.vol.ops);
    /* A 4Kn disk: the same byte offset is LBA 256 there, and a map made on
     * it says so. */
    h.block_size = 4096;
    h.part_lba = PART_LBA / 8u;
    assert_int_equal(extmap_find_on_disk(&h, disk_read, &d, 4096, &g_s),
                     EXTMAP_OK);
    synth_free(&sv);
}

/* The FAT32 library mk-test-fat.sh builds, as `mkstick create --p2-fs
 * fat32` makes one, read from a FILE at the partition's offset: the map's
 * readers find a FAT library as they find an exFAT one. */
#define FAT_LIBRARY "build/test-fat32-library.img"

typedef struct {
    FILE    *fp;
    uint64_t base;
} file_disk;

static int file_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const file_disk *d = ctx;
    uint8_t *out = buf;

    while (len > 0 && off < d->base) {
        *out++ = 0;
        off++;
        len--;
    }
    if (len == 0)
        return 0;
    if (fseek(d->fp, (long)(off - d->base), SEEK_SET) != 0 ||
        fread(out, 1, (size_t)len, d->fp) != (size_t)len)
        return -1;
    return 0;
}

static void test_extfind_fat_library(void **state)
{
    file_disk d;
    extmap_hdr h;

    (void)state;
    d.fp = fopen(FAT_LIBRARY, "rb");
    assert_non_null(d.fp);
    d.base = (uint64_t)PART_LBA * 512u;
    map_for(&h, g_id);
    assert_int_equal(extmap_find_on_disk(&h, file_read, &d, 512, &g_s),
                     EXTMAP_OK);
    assert_null(g_s.vol.ops);
    h.install_id[0] ^= 1u;
    assert_int_not_equal(extmap_find_on_disk(&h, file_read, &d, 512, &g_s),
                         EXTMAP_OK);
    fclose(d.fp);
}

static void test_extfind_other_disks(void **state)
{
    synth_vol sv;
    char text[IDENT_TEXT_MAX];
    static const uint8_t other[16] = { 1, 2, 3, 4, 5, 6, 7, 8,
                                       9, 10, 11, 12, 13, 14, 15, 16 };
    extmap_hdr h;
    disk d;

    (void)state;
    d.base = (uint64_t)PART_LBA * 512u;
    map_for(&h, g_id);

    /* A second stick: the same layout, another install. */
    record_text(IDENT_ROLE_LIBRARY, other, text);
    volume(&sv, ".falcon", text, 0);
    d.sv = &sv;
    assert_int_equal(extmap_find_on_disk(&h, disk_read, &d, 512, &g_s),
                     EXTMAP_ERR_NOMATCH);
    synth_free(&sv);

    /* This install's shelf where the library should be. */
    record_text(IDENT_ROLE_SHELF, g_id, text);
    volume(&sv, ".falcon", text, 0);
    d.sv = &sv;
    assert_int_equal(extmap_find_on_disk(&h, disk_read, &d, 512, &g_s),
                     EXTMAP_ERR_NOMATCH);
    synth_free(&sv);

    /* An exFAT volume at that LBA with no record: somebody's data disk. */
    volume(&sv, NULL, NULL, 0);
    d.sv = &sv;
    assert_int_equal(extmap_find_on_disk(&h, disk_read, &d, 512, &g_s),
                     EXTMAP_ERR_NOMATCH);
    synth_free(&sv);

    /* The right volume, but the map points elsewhere on the disk. */
    record_text(IDENT_ROLE_LIBRARY, g_id, text);
    volume(&sv, ".falcon", text, 0);
    d.sv = &sv;
    h.part_lba = PART_LBA - 1u;
    assert_int_equal(extmap_find_on_disk(&h, disk_read, &d, 512, &g_s),
                     EXTMAP_ERR_NOMATCH);
    /* Past the disk's end: the read fails, and that is not a match. */
    h.part_lba = PART_LBA * 64u;
    assert_int_equal(extmap_find_on_disk(&h, disk_read, &d, 512, &g_s),
                     EXTMAP_ERR_NOMATCH);
    /* An offset that would wrap. */
    h.part_lba = ~(uint64_t)0;
    assert_int_equal(extmap_find_on_disk(&h, disk_read, &d, 512, &g_s),
                     EXTMAP_ERR_NOMATCH);
    /* A disk of another block size. */
    h.part_lba = PART_LBA;
    assert_int_equal(extmap_find_on_disk(&h, disk_read, &d, 4096, &g_s),
                     EXTMAP_ERR_NOMATCH);

    assert_int_equal(extmap_find_on_disk(NULL, disk_read, &d, 512, &g_s),
                     EXTMAP_ERR_BAD_ARG);
    assert_int_equal(extmap_find_on_disk(&h, NULL, &d, 512, &g_s),
                     EXTMAP_ERR_BAD_ARG);
    assert_int_equal(extmap_find_on_disk(&h, disk_read, &d, 512, NULL),
                     EXTMAP_ERR_BAD_ARG);
    h.block_size = 0;
    assert_int_equal(extmap_find_on_disk(&h, disk_read, &d, 0, &g_s),
                     EXTMAP_ERR_BAD_ARG);
    synth_free(&sv);
}

int run_extfind_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_extfind_names),
        cmocka_unit_test(test_extfind_finds_the_library),
        cmocka_unit_test(test_extfind_fat_library),
        cmocka_unit_test(test_extfind_other_disks),
    };

    return cmocka_run_group_tests_name("extfind", tests, NULL, NULL);
}
