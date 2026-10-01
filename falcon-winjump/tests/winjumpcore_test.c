/* Tests for falcon-winjump/src/winjumpcore.c: the disk match and the
 * arithmetic winjump (and falcon-vdisk) do to feed the driver. The Windows
 * I/O around it (enumerate the disks, read their sectors, load the driver)
 * is a thin layer over Win32 that runs only in a WinPE; what must not be
 * wrong is here.
 */

#include "synth.h"
#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>

#include <winjumpcore.h>
#include <media/extmap.h>

/* ---- the disk match ----
 *
 * A disk as Windows presents a raw one: reads must be whole sectors at
 * sector offsets, and anything else fails, which is what makes the bounce
 * path in winjumpcore.c load-bearing. The disk is zeros, then an exFAT
 * volume from the core tests' synth builder with a /.falcon record in its
 * root. */

#define PART_LBA 2048u

static const uint8_t g_id[16] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01
};

typedef struct {
    synth_vol *sv;
    uint64_t   base;
    uint32_t   sector;
    unsigned   unaligned;     /* requests refused for not being whole */
} raw_disk;

static int raw_read(void *ctx, uint64_t off, uint32_t len, void *buf)
{
    raw_disk *d = ctx;
    uint8_t *out = buf;
    uint32_t i;

    if (off % d->sector != 0 || len % d->sector != 0 || len == 0) {
        d->unaligned++;
        return -1;
    }
    for (i = 0; i < len; i++) {
        uint64_t at = off + i;

        if (at < d->base)
            out[i] = 0;
        else if (at - d->base < d->sv->len)
            out[i] = d->sv->buf[at - d->base];
        else
            return -1;
    }
    return 0;
}

/* A volume whose /.falcon is `role`'s record for `id`, or none. */
static void make_volume(synth_vol *sv, ident_role role, const uint8_t *id)
{
    uint8_t *root;
    unsigned n = 0;

    synth_init(sv, 9u, 2u, 32u);
    root = synth_cluster(sv, sv->root_cluster);
    if (id != NULL) {
        ident_rec r;
        char text[IDENT_TEXT_MAX];
        size_t len;

        memset(&r, 0, sizeof r);
        r.version = IDENT_VERSION;
        r.role = role;
        memcpy(r.install_id, id, 16);
        len = ident_encode_text(&r, text, sizeof text);
        assert_true(len > 0);
        n = synth_put_set(root, ".falcon", 0,
                          (uint8_t)(SYNTH_FLAG_ALLOC | SYNTH_FLAG_NOFAT), 3u,
                          len);
        memcpy(synth_cluster(sv, 3u), text, len);
    }
    synth_put_end(root + n * SYNTH_ENT);
}

static void map_hdr(extmap_hdr *h, const uint8_t *id, uint64_t lba,
                    uint32_t bs)
{
    memset(h, 0, sizeof *h);
    memcpy(h->install_id, id, 16);
    h->part_lba = lba;
    h->block_size = bs;
    h->n_extents = 1;
    h->iso_size = 2048;
}

static wj_scratch g_scratch;

static void test_disk_holds(void **state)
{
    synth_vol sv;
    raw_disk d;
    extmap_hdr h;

    (void)state;
    make_volume(&sv, IDENT_ROLE_LIBRARY, g_id);
    d.sv = &sv;
    d.base = (uint64_t)PART_LBA * 512u;
    d.sector = 512;
    d.unaligned = 0;
    map_hdr(&h, g_id, PART_LBA, 512);
    assert_int_equal(wj_disk_holds(&h, raw_read, &d, &g_scratch), WJ_OK);
    /* The record is 52 bytes; every read that reached the disk was whole. */
    assert_int_equal(d.unaligned, 0);

    /* A 4Kn disk: the same volume, whose 512-byte exFAT sectors now fall
     * inside the disk's 4096-byte ones and go through the bounce. */
    d.sector = 4096;
    map_hdr(&h, g_id, PART_LBA / 8u, 4096);
    assert_int_equal(wj_disk_holds(&h, raw_read, &d, &g_scratch), WJ_OK);
    assert_int_equal(d.unaligned, 0);
    synth_free(&sv);
}

static void test_disk_holds_not(void **state)
{
    static const uint8_t other[16] = { 9, 9, 9, 9, 9, 9, 9, 9,
                                       9, 9, 9, 9, 9, 9, 9, 9 };
    synth_vol sv;
    raw_disk d;
    extmap_hdr h;

    (void)state;
    d.base = (uint64_t)PART_LBA * 512u;
    d.sector = 512;
    d.unaligned = 0;

    /* Another stick's library. */
    make_volume(&sv, IDENT_ROLE_LIBRARY, other);
    d.sv = &sv;
    map_hdr(&h, g_id, PART_LBA, 512);
    assert_int_equal(wj_disk_holds(&h, raw_read, &d, &g_scratch),
                     WJ_ERR_NOMATCH);
    synth_free(&sv);

    /* This install's shelf, not its library. */
    make_volume(&sv, IDENT_ROLE_SHELF, g_id);
    d.sv = &sv;
    assert_int_equal(wj_disk_holds(&h, raw_read, &d, &g_scratch),
                     WJ_ERR_NOMATCH);
    synth_free(&sv);

    /* An exFAT volume with no record. */
    make_volume(&sv, IDENT_ROLE_LIBRARY, NULL);
    d.sv = &sv;
    assert_int_equal(wj_disk_holds(&h, raw_read, &d, &g_scratch),
                     WJ_ERR_NOMATCH);
    synth_free(&sv);

    /* The library, but the map's LBA is somewhere else on the disk. */
    make_volume(&sv, IDENT_ROLE_LIBRARY, g_id);
    d.sv = &sv;
    map_hdr(&h, g_id, PART_LBA + 8u, 512);
    assert_int_equal(wj_disk_holds(&h, raw_read, &d, &g_scratch),
                     WJ_ERR_NOMATCH);
    /* A map made for 512-byte blocks read on a 4Kn disk: its LBA is
     * another byte there, and its reads are not whole sectors. */
    d.sector = 4096;
    map_hdr(&h, g_id, PART_LBA, 512);
    assert_int_equal(wj_disk_holds(&h, raw_read, &d, &g_scratch),
                     WJ_ERR_NOMATCH);

    assert_int_equal(wj_disk_holds(NULL, raw_read, &d, &g_scratch),
                     WJ_ERR_BAD_ARG);
    assert_int_equal(wj_disk_holds(&h, NULL, &d, &g_scratch), WJ_ERR_BAD_ARG);
    assert_int_equal(wj_disk_holds(&h, raw_read, &d, NULL), WJ_ERR_BAD_ARG);
    h.block_size = 0;
    assert_int_equal(wj_disk_holds(&h, raw_read, &d, &g_scratch),
                     WJ_ERR_BAD_ARG);
    h.block_size = WJ_BOUNCE * 2u;
    assert_int_equal(wj_disk_holds(&h, raw_read, &d, &g_scratch),
                     WJ_ERR_BAD_ARG);
    synth_free(&sv);
}

/* Encode a map of `n` runs and hand it back, with its header. */
static uint8_t g_map[EXTMAP_HDR_LEN + 8u * EXTMAP_EXT_LEN];

static void encode_map(extmap_hdr *h, const extmap_extent *ext, uint32_t n,
                       uint64_t part_lba, uint32_t bs)
{
    uint64_t total = 0;
    uint32_t i;
    size_t len = 0;

    memset(h, 0, sizeof *h);
    for (i = 0; i < 16; i++)
        h->install_id[i] = (uint8_t)(0xA0u + i);
    h->part_lba = part_lba;
    h->block_size = bs;
    h->n_extents = n;
    for (i = 0; i < n; i++)
        total += ext[i].length;
    h->iso_size = total;
    assert_int_equal(extmap_encode(h, ext, g_map, sizeof g_map, &len),
                     EXTMAP_OK);
}

static void test_backing_extents(void **state)
{
    static const extmap_extent ext[2] = {
        { 4096u, 1u << 20 },        /* 4 KiB into the partition, 1 MiB long */
        { 2u << 20, 2048u }
    };
    extmap_hdr h;
    wj_extent out[8];
    uint32_t n = 0;

    (void)state;
    encode_map(&h, ext, 2, 133120u, 512u);
    assert_int_equal(wj_backing_extents(&h, g_map, sizeof g_map, out, 8, &n),
                     WJ_OK);
    assert_int_equal(n, 2u);

    /* disk_offset = part_lba*block_size + run offset; iso_offset accumulates. */
    assert_int_equal((long long)out[0].iso_offset, 0);
    assert_int_equal((long long)out[0].disk_offset, 133120LL * 512 + 4096);
    assert_int_equal((long long)out[0].length, 1 << 20);

    assert_int_equal((long long)out[1].iso_offset, 1 << 20);
    assert_int_equal((long long)out[1].disk_offset, 133120LL * 512 + (2 << 20));
    assert_int_equal((long long)out[1].length, 2048);
}

static void test_backing_nospace(void **state)
{
    static const extmap_extent ext[2] = {
        { 4096u, 4096u }, { 8192u, 4096u }
    };
    extmap_hdr h;
    wj_extent out[1];
    uint32_t n = 0;

    (void)state;
    encode_map(&h, ext, 2, 64u, 512u);
    assert_int_equal(wj_backing_extents(&h, g_map, sizeof g_map, out, 1, &n),
                     WJ_ERR_NOSPACE);
}

static void test_backing_bad_args(void **state)
{
    extmap_hdr h;
    wj_extent out[4];
    uint32_t n = 0;
    static const extmap_extent ext[1] = { { 0u, 4096u } };

    (void)state;
    encode_map(&h, ext, 1, 64u, 512u);
    assert_int_equal(wj_backing_extents(NULL, g_map, sizeof g_map, out, 4, &n),
                     WJ_ERR_BAD_ARG);
    assert_int_equal(wj_backing_extents(&h, NULL, sizeof g_map, out, 4, &n),
                     WJ_ERR_BAD_ARG);
}

/* ---- the key=value lines ---- */

static void kv(const char *in, int ok, const char *k, const char *v)
{
    char line[64];
    char *key = NULL;
    char *value = NULL;

    snprintf(line, sizeof line, "%s", in);
    assert_int_equal(wj_kv_line(line, &key, &value), ok);
    if (ok) {
        assert_string_equal(key, k);
        assert_string_equal(value, v);
    }
}

static void test_kv_line(void **state)
{
    char line[8] = "a=b";
    char *key;
    char *value;

    (void)state;
    kv("win11_bypass=1", 1, "win11_bypass", "1");
    kv("win11_bypass = 1", 1, "win11_bypass", "1");
    kv("  win11_bypass\t=\t1  \r", 1, "win11_bypass", "1");
    kv("unattend = falcon\\my answers.xml\r\n", 1, "unattend",
       "falcon\\my answers.xml");
    kv("iso=a=b", 1, "iso", "a=b");
    kv("shell=", 1, "shell", "");
    kv("shell =   ", 1, "shell", "");
    kv("", 0, NULL, NULL);
    kv("   \r", 0, NULL, NULL);
    kv("# win11_bypass=1", 0, NULL, NULL);
    kv("  #x=1", 0, NULL, NULL);
    kv("win11_bypass", 0, NULL, NULL);
    kv("=1", 0, NULL, NULL);
    kv("  = 1", 0, NULL, NULL);
    assert_int_equal(wj_kv_line(NULL, &key, &value), 0);
    assert_int_equal(wj_kv_line(line, NULL, &value), 0);
    assert_int_equal(wj_kv_line(line, &key, NULL), 0);
}

int run_winjumpcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_disk_holds),
        cmocka_unit_test(test_disk_holds_not),
        cmocka_unit_test(test_backing_extents),
        cmocka_unit_test(test_backing_nospace),
        cmocka_unit_test(test_backing_bad_args),
        cmocka_unit_test(test_kv_line),
    };
    return cmocka_run_group_tests_name("winjumpcore", tests, NULL, NULL);
}
