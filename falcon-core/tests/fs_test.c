/* Tests for falcon-core/src/fs/fs.c, the filesystem driver registry of DESIGN
 * Section 9.
 *
 * What is under test here is the SEAM, not the drivers: the driver groups
 * next door already pin exFAT and ext4 behaviour against synthetic volumes
 * and real fixture images, and this group exists so that dressing them in a
 * common vtable cannot quietly change what a consumer sees. So the questions
 * are the seam's questions:
 *
 *   - does the registry find a driver by name, by role hint, and by magic;
 *   - does the role hint only reorder the probes rather than restrict them
 *     (an exFAT volume on a shelf-typed partition must still identify);
 *   - is every driver status mapped onto an FS_* code, enumerated rather than
 *     spot-checked, so a driver that grows a status is caught here;
 *   - does a dirent survive the round trip through the unified form well
 *     enough to reopen the object it names, and do the names convert both
 *     ways without loss;
 *   - is the optional extent capability reported honestly, present on exFAT
 *     and absent (not broken) on ext4.
 *
 * The exFAT side runs on the synthetic volume builder
 * (falcon-core/tests/support/synth.c), so it needs no fixture image. The ext4
 * side uses the fixture when it is there and skips those cases when it is not,
 * exactly as the ext4 group does; the probe-order cases use a synthetic
 * superblock instead, because probe reads one magic number and nothing else.
 */

#include "synth.h"
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

#include <fs/exfat.h>
#include <fs/ext4.h>
#include <fs/iso9660.h>
#include <fs/udf.h>
#include <fs/fat.h>
#include <fs/fs.h>

/* The blob sizes are a compile-time contract with every registered driver;
 * fs.c asserts it too, and repeating it here means a driver that outgrows the
 * blob breaks the tests as well as the loader rather than only one of them. */
_Static_assert(sizeof(exfat_vol) <= FS_VOL_SIZE, "exfat_vol fits");
_Static_assert(sizeof(exfat_dir) <= FS_DIR_SIZE, "exfat_dir fits");
_Static_assert(sizeof(exfat_file) <= FS_FILE_SIZE, "exfat_file fits");
_Static_assert(sizeof(ext4_vol) <= FS_VOL_SIZE, "ext4_vol fits");
_Static_assert(sizeof(ext4_dir) <= FS_DIR_SIZE, "ext4_dir fits");
_Static_assert(sizeof(ext4_file) <= FS_FILE_SIZE, "ext4_file fits");

#define STD_BPS 9u
#define STD_SPC 2u
#define STD_CLUSTERS 32u

/* ---- a synthetic ext4 superblock, magic and nothing else ----
 *
 * Legitimate for probe, which reads exactly this much: mount is what
 * validates, and the mount path is the ext4 group's fixture image. */

#define SYNTH_EXT4_LEN 4096u

typedef struct {
    uint8_t buf[SYNTH_EXT4_LEN];
    int fail;      /* every read fails, for the I/O path */
} synth_sb;

static void synth_sb_init(synth_sb *s, int with_magic)
{
    memset(s, 0, sizeof(*s));
    if (with_magic) {
        s->buf[1024u + 0x38u] = 0x53;
        s->buf[1024u + 0x39u] = 0xEF;
    }
}

static int synth_sb_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    synth_sb *s = ctx;

    if (s->fail)
        return -1;
    if (off % 512u != 0 || len % 512u != 0)
        return -1;
    if (off + len > SYNTH_EXT4_LEN)
        return -1;
    memcpy(buf, s->buf + off, (size_t)len);
    return 0;
}

/* ---- the registry ---- */

static void test_fs_registry_lookup(void **state)
{
    const fs_ops *exfat;
    const fs_ops *ext4;
    size_t i;

    (void)state;

    assert_int_equal(fs_driver_count(), 5);
    for (i = 0; i < fs_driver_count(); i++)
        assert_non_null(fs_driver_at(i));
    assert_null(fs_driver_at(fs_driver_count()));

    exfat = fs_by_name("exfat");
    ext4 = fs_by_name("ext4");
    assert_non_null(exfat);
    assert_non_null(ext4);
    assert_ptr_equal(exfat, &fs_ops_exfat);
    assert_ptr_equal(ext4, &fs_ops_ext4);
    assert_string_equal(exfat->name, "exfat");
    assert_string_equal(ext4->name, "ext4");

    /* The role each driver is the first answer for (DESIGN Section 3's p2
     * and p3). */
    assert_int_equal(exfat->role, FS_HINT_ISO);
    assert_int_equal(ext4->role, FS_HINT_SHELF);

    /* FAT is in the table and is nobody's role: p1 is vfat but the
     * loader never reaches it through the registry, and an El Torito
     * boot image has no partition role at all. */
    assert_ptr_equal(fs_by_name("fat"), &fs_ops_fat);
    assert_int_equal(fs_ops_fat.role, FS_HINT_NONE);
    /* "fat" and "exfat" are different filesystems and different
     * drivers; a lookup for one must never answer with the other. */
    assert_ptr_not_equal(fs_by_name("fat"), &fs_ops_exfat);

    assert_null(fs_by_name("btrfs"));
    assert_null(fs_by_name(NULL));
    /* A prefix is not a name: "ext" must not match "ext4". */
    assert_null(fs_by_name("ext"));
    assert_null(fs_by_name("exfat2"));

    /* The optional capability, per driver. exFAT has the extent map the
     * injection framework needs; ext4 deliberately does not. */
    assert_non_null(exfat->extents);
    assert_null(ext4->extents);

    /* The media drivers: no partition role, both with an extent map and a
     * label, and UDF probed ahead of ISO9660 so a bridge disc is read
     * through its complete side. */
    assert_ptr_equal(fs_by_name("udf"), &fs_ops_udf);
    assert_ptr_equal(fs_by_name("iso9660"), &fs_ops_iso9660);
    assert_int_equal(fs_ops_udf.role, FS_HINT_NONE);
    assert_int_equal(fs_ops_iso9660.role, FS_HINT_NONE);
    assert_non_null(fs_ops_udf.extents);
    assert_non_null(fs_ops_iso9660.extents);
    assert_non_null(fs_ops_udf.label);
    assert_non_null(fs_ops_iso9660.label);
    assert_null(exfat->label);
    assert_null(ext4->label);
    assert_ptr_equal(fs_driver_at(2), &fs_ops_udf);
    assert_ptr_equal(fs_driver_at(3), &fs_ops_iso9660);
    assert_string_equal(fs_vol_label(NULL), "");
}

/* ---- probe order ---- */

static void test_fs_identify_exfat_any_hint(void **state)
{
    synth_vol sv;

    (void)state;
    synth_init(&sv, STD_BPS, STD_SPC, STD_CLUSTERS);

    /* The role hint reorders the probes; it never restricts them. An exFAT
     * volume sitting on a partition typed as the UKI shelf is a mistake to
     * report at the layer that cares, not a volume to declare unreadable. */
    assert_ptr_equal(fs_identify(synth_read, &sv, FS_HINT_ISO), &fs_ops_exfat);
    assert_ptr_equal(fs_identify(synth_read, &sv, FS_HINT_SHELF),
                     &fs_ops_exfat);
    assert_ptr_equal(fs_identify(synth_read, &sv, FS_HINT_NONE),
                     &fs_ops_exfat);

    synth_free(&sv);
}

static void test_fs_identify_ext4_any_hint(void **state)
{
    synth_sb sb;

    (void)state;
    synth_sb_init(&sb, 1);

    assert_ptr_equal(fs_identify(synth_sb_read, &sb, FS_HINT_SHELF),
                     &fs_ops_ext4);
    assert_ptr_equal(fs_identify(synth_sb_read, &sb, FS_HINT_ISO),
                     &fs_ops_ext4);
    assert_ptr_equal(fs_identify(synth_sb_read, &sb, FS_HINT_NONE),
                     &fs_ops_ext4);
}

static void test_fs_identify_nothing(void **state)
{
    synth_sb sb;

    (void)state;

    /* Zeroed bytes are nobody's filesystem. This is the LUKS2 case as far as
     * the registry is concerned: an encrypted volume identifies as nothing,
     * which is why falcon-efi/src/discover/discover.c does not even ask about
     * one. */
    synth_sb_init(&sb, 0);
    assert_null(fs_identify(synth_sb_read, &sb, FS_HINT_NONE));
    assert_null(fs_identify(synth_sb_read, &sb, FS_HINT_SHELF));

    /* A device that cannot be read identifies as nothing either, rather than
     * as whichever driver was probed first. */
    synth_sb_init(&sb, 1);
    sb.fail = 1;
    assert_null(fs_identify(synth_sb_read, &sb, FS_HINT_SHELF));

    assert_null(fs_identify(NULL, &sb, FS_HINT_NONE));
}

static void test_fs_probe_rejects_partial_magic(void **state)
{
    synth_vol sv;
    uint8_t *vbr;

    (void)state;
    synth_init(&sv, STD_BPS, STD_SPC, STD_CLUSTERS);
    vbr = sv.buf;

    /* probe must agree with mount about what an exFAT volume is: both the
     * filesystem name and the boot signature, or it is not one. */
    vbr[510] = 0;
    assert_null(fs_identify(synth_read, &sv, FS_HINT_ISO));
    vbr[510] = 0x55;
    assert_ptr_equal(fs_identify(synth_read, &sv, FS_HINT_ISO), &fs_ops_exfat);
    vbr[3] = 'X';
    assert_null(fs_identify(synth_read, &sv, FS_HINT_ISO));

    synth_free(&sv);
}

/* ---- status mapping ---- */

static void test_fs_status_map_exfat_total(void **state)
{
    static const struct {
        int driver;
        int unified;
    } want[] = {
        { EXFAT_OK,             FS_OK },
        { EXFAT_ERR_IO,         FS_ERR_IO },
        { EXFAT_ERR_BAD_SUPER,  FS_ERR_BAD_SUPER },
        { EXFAT_ERR_CORRUPT,    FS_ERR_CORRUPT },
        { EXFAT_ERR_BAD_ARG,    FS_ERR_BAD_ARG },
        { EXFAT_ERR_RANGE,      FS_ERR_RANGE },
        { EXFAT_ERR_NOSPACE,    FS_ERR_NOSPACE }
    };
    size_t i;

    (void)state;

    /* Enumerated, not spot-checked: the point is that the table is TOTAL over
     * the driver's enum. EXFAT_ERR_NOSPACE is the last value; if the driver
     * grows one past it, test_fs_status_map_sweeps sees the unmapped code. */
    for (i = 0; i < sizeof want / sizeof want[0]; i++)
        assert_int_equal(fs_status_from_exfat(want[i].driver),
                         want[i].unified);

    /* dir_next's "1 = entry produced" passes through untouched, as does any
     * other non-negative value; the vocabulary only differs in the errors. */
    assert_int_equal(fs_status_from_exfat(1), 1);

    /* A value outside the enum maps to the conservative answer rather than
     * leaking through as a number no consumer can interpret. */
    assert_int_equal(fs_status_from_exfat(-99), FS_ERR_CORRUPT);
}

static void test_fs_status_map_ext4_total(void **state)
{
    static const struct {
        int driver;
        int unified;
    } want[] = {
        { EXT4_OK,                 FS_OK },
        { EXT4_ERR_IO,             FS_ERR_IO },
        { EXT4_ERR_BAD_SUPER,      FS_ERR_BAD_SUPER },
        { EXT4_ERR_CORRUPT,        FS_ERR_CORRUPT },
        { EXT4_ERR_BAD_ARG,        FS_ERR_BAD_ARG },
        { EXT4_ERR_RANGE,          FS_ERR_RANGE },
        { EXT4_ERR_UNSUPPORTED,    FS_ERR_UNSUPPORTED },
        { EXT4_ERR_NEEDS_RECOVERY, FS_ERR_NEEDS_RECOVERY },
        { EXT4_ERR_DIRTY,          FS_ERR_DIRTY },
        { EXT4_ERR_CSUM,           FS_ERR_CSUM },
        { EXT4_ERR_INLINE,         FS_ERR_INLINE },
        { EXT4_ERR_SYMLINK,        FS_ERR_SYMLINK },
        { EXT4_ERR_HOLE,           FS_ERR_HOLE }
    };
    size_t i;

    (void)state;

    for (i = 0; i < sizeof want / sizeof want[0]; i++)
        assert_int_equal(fs_status_from_ext4(want[i].driver),
                         want[i].unified);
    assert_int_equal(fs_status_from_ext4(1), 1);
    assert_int_equal(fs_status_from_ext4(-99), FS_ERR_CORRUPT);
}

static void test_fs_status_map_media_total(void **state)
{
    static const int iso[] = {
        ISO9660_OK, ISO9660_ERR_IO, ISO9660_ERR_BAD_SUPER, ISO9660_ERR_CORRUPT,
        ISO9660_ERR_BAD_ARG, ISO9660_ERR_RANGE, ISO9660_ERR_NOSPACE,
        ISO9660_ERR_UNSUPPORTED, ISO9660_ERR_SYMLINK
    };
    static const int udf[] = {
        UDF_OK, UDF_ERR_IO, UDF_ERR_BAD_SUPER, UDF_ERR_CORRUPT,
        UDF_ERR_BAD_ARG, UDF_ERR_RANGE, UDF_ERR_NOSPACE,
        UDF_ERR_UNSUPPORTED, UDF_ERR_SYMLINK
    };
    static const int want[] = {
        FS_OK, FS_ERR_IO, FS_ERR_BAD_SUPER, FS_ERR_CORRUPT, FS_ERR_BAD_ARG,
        FS_ERR_RANGE, FS_ERR_NOSPACE, FS_ERR_UNSUPPORTED, FS_ERR_SYMLINK
    };
    size_t i;

    (void)state;

    /* FAT has no symlink status: the format has no such thing, so its row
     * list is the same set minus that one. */
    static const int fat[] = {
        FAT_OK, FAT_ERR_IO, FAT_ERR_NOT_FAT, FAT_ERR_CORRUPT,
        FAT_ERR_BAD_ARG, FAT_ERR_RANGE, FAT_ERR_NOSPACE,
        FAT_ERR_UNSUPPORTED
    };

    for (i = 0; i < sizeof want / sizeof want[0]; i++) {
        assert_int_equal(fs_status_from_iso9660(iso[i]), want[i]);
        assert_int_equal(fs_status_from_udf(udf[i]), want[i]);
    }
    for (i = 0; i < sizeof fat / sizeof fat[0]; i++)
        assert_int_equal(fs_status_from_fat(fat[i]), want[i]);
    /* An unmapped code degrades to the conservative answer rather than
     * passing a driver's private number through as an fs status. */
    assert_int_equal(fs_status_from_fat(-99), FS_ERR_CORRUPT);
    assert_int_equal(fs_status_from_udf(UDF_ERR_NOT_FOUND), FS_ERR_NOT_FOUND);
    assert_int_equal(fs_status_from_iso9660(1), 1);
    assert_int_equal(fs_status_from_udf(1), 1);
    assert_int_equal(fs_status_from_iso9660(-99), FS_ERR_CORRUPT);
    assert_int_equal(fs_status_from_udf(-99), FS_ERR_CORRUPT);
}

/* Every negative value either driver's enum can hold must land on a distinct,
 * meaningful FS_* code rather than on the catch-all. Written as a sweep so
 * that a driver growing a status without a mapping row fails here. */
static void test_fs_status_map_sweeps(void **state)
{
    int rc;

    (void)state;

    for (rc = -1; rc >= EXFAT_ERR_NOSPACE; rc--) {
        int mapped = fs_status_from_exfat(rc);

        assert_true(mapped < 0);
        if (rc != EXFAT_ERR_CORRUPT)
            assert_int_not_equal(mapped, FS_ERR_CORRUPT);
    }
    for (rc = -1; rc >= EXT4_ERR_HOLE; rc--) {
        int mapped = fs_status_from_ext4(rc);

        assert_true(mapped < 0);
        if (rc != EXT4_ERR_CORRUPT)
            assert_int_not_equal(mapped, FS_ERR_CORRUPT);
    }
}

/* ---- name conversion ---- */

static void test_fs_name_round_trip(void **state)
{
    static const uint8_t bytes[] = { 'v', 'm', 'l', 'i', 'n', 'u', 'z',
                                     0xC3, 0xA9, '.', 'e', 'f', 'i' };
    uint16_t wide[FS_NAME_MAX + 1];
    uint8_t back[FS_NAME_MAX + 1];
    size_t n;
    size_t m;

    (void)state;

    /* The property the unified dirent rests on: a byte-named filesystem's
     * name widens and narrows back to exactly the bytes the directory block
     * held, high bytes included. Widening is not a character-set conversion
     * and does not claim to be; it is a lossless carrier. */
    n = fs_name_from_bytes(bytes, sizeof bytes, wide, sizeof wide /
                                                      sizeof wide[0]);
    assert_int_equal(n, sizeof bytes);
    assert_int_equal(wide[7], 0x00C3);
    assert_int_equal(wide[8], 0x00A9);
    assert_int_equal(wide[n], 0);

    m = fs_name_to_bytes(wide, back, sizeof back);
    assert_int_equal(m, sizeof bytes);
    assert_memory_equal(back, bytes, sizeof bytes);
    assert_int_equal(back[m], 0);
}

static void test_fs_name_conversion_bounds(void **state)
{
    static const uint8_t bytes[] = { 'a', 'b', 'c', 'd' };
    static const uint16_t wide_hi[] = { 'a', 0x4E2D, 'b', 0 };
    uint16_t wide[3];
    uint8_t narrow[3];
    size_t n;

    (void)state;

    /* Both directions truncate inside the caller's buffer and always
     * terminate: a name longer than the destination is a display problem,
     * never an overrun. */
    n = fs_name_from_bytes(bytes, sizeof bytes, wide, 3);
    assert_int_equal(n, 2);
    assert_int_equal(wide[2], 0);

    n = fs_name_to_bytes(wide_hi, narrow, 3);
    assert_int_equal(n, 2);
    assert_int_equal(narrow[2], 0);

    /* A unit that cannot have come from a byte-named filesystem narrows to
     * '?', which is a display answer and is not claimed to round trip. */
    {
        uint8_t full[8];

        n = fs_name_to_bytes(wide_hi, full, sizeof full);
        assert_int_equal(n, 3);
        assert_int_equal(full[0], 'a');
        assert_int_equal(full[1], '?');
        assert_int_equal(full[2], 'b');
    }

    /* Degenerate arguments answer rather than crash. */
    assert_int_equal(fs_name_to_bytes(wide_hi, narrow, 0), 0);
    assert_int_equal(fs_name_from_bytes(bytes, sizeof bytes, NULL, 4), 0);
}

static void test_fs_name_compare(void **state)
{
    static const uint16_t a[] = { 'V', 'M', 'l', 'i', 'n', 'u', 'z', 0 };
    static const uint16_t b[] = { 'v', 'm', 'l', 'i', 'n', 'u', 'z', 0 };
    static const uint16_t c[] = { 'v', 'm', 'l', 'i', 'n', 'u', 'z', '2', 0 };
    static const uint16_t dotefi[] = { '.', 'e', 'f', 'i', 0 };

    (void)state;

    assert_true(fs_name_eq_ascii(a, "vmlinuz"));
    assert_true(fs_name_eq_ascii(b, "VMLINUZ"));
    assert_false(fs_name_eq_ascii(b, "vmlinu"));
    assert_false(fs_name_eq_ascii(c, "vmlinuz"));
    assert_false(fs_name_eq_ascii(NULL, "vmlinuz"));

    assert_true(fs_name_ends_ascii(c, "Z2"));
    assert_true(fs_name_ends_ascii(dotefi, ".efi"));
    assert_false(fs_name_ends_ascii(dotefi, "x.efi"));
    assert_false(fs_name_ends_ascii(b, ""));

    assert_int_equal(fs_name_cmp(a, b), 0);   /* case-insensitive */
    assert_true(fs_name_cmp(b, c) < 0);       /* prefix sorts first */
    assert_true(fs_name_cmp(c, b) > 0);
}

/* ---- the vtable over a real (synthetic) exFAT volume ---- */

/* Builds a root directory holding one subdirectory and one file, both
 * contiguous, and returns through *out the volume the tests mount. */
static void build_tree(synth_vol *sv)
{
    uint8_t *root;
    unsigned n;
    uint32_t sub_cluster = 3;
    uint32_t file_cluster = 4;
    uint32_t cb;

    synth_init(sv, STD_BPS, STD_SPC, STD_CLUSTERS);
    cb = synth_cluster_bytes(sv);

    root = synth_cluster(sv, sv->root_cluster);
    n = synth_put_set(root, "sub", SYNTH_ATTR_DIR,
                      (uint8_t)(SYNTH_FLAG_ALLOC | SYNTH_FLAG_NOFAT),
                      sub_cluster, cb);
    n += synth_put_set(root + n * SYNTH_ENT, "falcon.iso", 0,
                       (uint8_t)(SYNTH_FLAG_ALLOC | SYNTH_FLAG_NOFAT),
                       file_cluster, cb);
    synth_put_end(root + n * SYNTH_ENT);

    /* One file inside the subdirectory, so dir_open through the vtable has
     * something to find. */
    {
        uint8_t *sub = synth_cluster(sv, sub_cluster);
        unsigned m = synth_put_set(sub, "inner.iso", 0,
                                   (uint8_t)(SYNTH_FLAG_ALLOC |
                                             SYNTH_FLAG_NOFAT),
                                   file_cluster, cb);

        synth_put_end(sub + m * SYNTH_ENT);
    }

    {
        uint32_t clusters[1] = { 0 };

        clusters[0] = file_cluster;
        synth_write_file(sv, clusters, 1, cb, 0);
    }
}

static void test_fs_exfat_through_vtable(void **state)
{
    synth_vol sv;
    fs_vol vol;
    fs_dir dir;
    fs_dir sub;
    fs_file file;
    fs_dirent ent;
    fs_dirent dir_ent;
    fs_dirent file_ent;
    uint8_t buf[64];
    uint8_t want[64];
    int saw_dir = 0;
    int saw_file = 0;
    int rc;

    (void)state;
    build_tree(&sv);

    memset(&vol, 0, sizeof vol);
    assert_int_equal(fs_mount(&vol, &fs_ops_exfat, synth_read, &sv), FS_OK);
    assert_string_equal(fs_vol_name(&vol), "exfat");
    assert_true(fs_has_extents(&vol));

    assert_int_equal(fs_dir_open_root(&vol, &dir), FS_OK);
    while ((rc = fs_dir_next(&dir, &ent)) == 1) {
        if (ent.is_dir) {
            assert_true(fs_name_eq_ascii(ent.name, "sub"));
            assert_false(ent.is_regular);
            memcpy(&dir_ent, &ent, sizeof ent);
            saw_dir = 1;
        } else {
            assert_true(fs_name_eq_ascii(ent.name, "falcon.iso"));
            /* exFAT has no device nodes: not a directory means a byte
             * stream, and the unified view says so. */
            assert_true(ent.is_regular);
            assert_int_equal(ent.name_len, 10);
            assert_int_equal(ent.size, synth_cluster_bytes(&sv));
            memcpy(&file_ent, &ent, sizeof ent);
            saw_file = 1;
        }
    }
    assert_int_equal(rc, FS_OK);
    assert_true(saw_dir);
    assert_true(saw_file);

    /* The dirent carries enough of the driver's own view to reopen what it
     * names, which is the whole reason it has a private part. */
    assert_int_equal(fs_dir_open(&vol, &sub, &dir_ent), FS_OK);
    assert_int_equal(fs_dir_next(&sub, &ent), 1);
    assert_true(fs_name_eq_ascii(ent.name, "inner.iso"));

    assert_int_equal(fs_file_open(&vol, &file, &file_ent), FS_OK);
    assert_int_equal(fs_file_size(&file), synth_cluster_bytes(&sv));
    assert_int_equal(fs_file_read(&file, 8, sizeof buf, buf), FS_OK);
    synth_pattern_fill(want, 8, sizeof want);
    assert_memory_equal(buf, want, sizeof buf);

    /* A read past the end is the driver's answer, mapped. */
    assert_int_equal(fs_file_read(&file, synth_cluster_bytes(&sv),
                                 sizeof buf, buf), FS_ERR_RANGE);

    /* The callback form reads the same bytes and folds every error to -1. */
    memset(buf, 0, sizeof buf);
    assert_int_equal(fs_file_read_cb(&file, 8, sizeof buf, buf), 0);
    assert_memory_equal(buf, want, sizeof buf);
    assert_int_equal(fs_file_read_cb(&file, synth_cluster_bytes(&sv),
                                     sizeof buf, buf), -1);

    synth_free(&sv);
}

static void test_fs_exfat_extents_through_vtable(void **state)
{
    synth_vol sv;
    fs_vol vol;
    fs_dir dir;
    fs_dirent ent;
    fs_extent runs[4];
    size_t count = 0;
    int rc;

    (void)state;
    build_tree(&sv);

    memset(&vol, 0, sizeof vol);
    assert_int_equal(fs_mount(&vol, &fs_ops_exfat, synth_read, &sv), FS_OK);
    assert_int_equal(fs_dir_open_root(&vol, &dir), FS_OK);

    do {
        rc = fs_dir_next(&dir, &ent);
        assert_int_equal(rc, 1);
    } while (ent.is_dir);

    assert_int_equal(fs_extents(&vol, &ent, runs, 4, &count), FS_OK);
    assert_int_equal(count, 1);
    assert_int_equal(runs[0].len, synth_cluster_bytes(&sv));

    /* The sizing call: no array, and the required count comes back with
     * FS_ERR_NOSPACE, exactly as the driver's own contract says. */
    count = 0;
    assert_int_equal(fs_extents(&vol, &ent, NULL, 0, &count),
                     FS_ERR_NOSPACE);
    assert_int_equal(count, 1);

    synth_free(&sv);
}

/* ---- guard rails ---- */

static void test_fs_unmounted_and_bad_args(void **state)
{
    fs_vol vol;
    fs_dir dir;
    fs_file file;
    fs_dirent ent;
    fs_extent runs[2];
    size_t count = 1;

    (void)state;
    memset(&vol, 0, sizeof vol);
    memset(&dir, 0, sizeof dir);
    memset(&file, 0, sizeof file);
    memset(&ent, 0, sizeof ent);

    /* A volume that never mounted dispatches nowhere, and says so rather
     * than calling through a NULL vtable. */
    assert_string_equal(fs_vol_name(&vol), "none");
    assert_false(fs_has_extents(&vol));
    assert_int_equal(fs_dir_open_root(&vol, &dir), FS_ERR_BAD_ARG);
    assert_int_equal(fs_dir_open(&vol, &dir, &ent), FS_ERR_BAD_ARG);
    assert_int_equal(fs_dir_next(&dir, &ent), FS_ERR_BAD_ARG);
    assert_int_equal(fs_file_open(&vol, &file, &ent), FS_ERR_BAD_ARG);
    assert_int_equal(fs_file_read(&file, 0, 512, runs), FS_ERR_BAD_ARG);
    assert_int_equal(fs_file_size(&file), 0);
    assert_int_equal(fs_extents(&vol, &ent, runs, 2, &count),
                     FS_ERR_BAD_ARG);
    assert_int_equal(fs_mount(NULL, &fs_ops_exfat, synth_sb_read, NULL),
                     FS_ERR_BAD_ARG);
    assert_int_equal(fs_mount(&vol, NULL, synth_sb_read, NULL),
                     FS_ERR_BAD_ARG);
}

static void test_fs_failed_mount_leaves_no_vtable(void **state)
{
    synth_sb sb;
    fs_vol vol;
    fs_dir dir;

    (void)state;
    synth_sb_init(&sb, 0);
    memset(&vol, 0, sizeof vol);

    /* Zeroed bytes are not exFAT; the volume must be left unusable rather
     * than half-mounted with a vtable a caller could dispatch through. */
    assert_int_equal(fs_mount(&vol, &fs_ops_exfat, synth_sb_read, &sb),
                     FS_ERR_BAD_SUPER);
    assert_null(vol.ops);
    assert_string_equal(fs_vol_name(&vol), "none");
    assert_int_equal(fs_dir_open_root(&vol, &dir), FS_ERR_BAD_ARG);
}

/* ---- the ext4 half, over the fixture image ---- */

static int ext4_fd = -1;

static int ext4_img_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    if (off % 512u != 0 || len % 512u != 0)
        return -1;
    if (pread(ext4_fd, buf, (size_t)len, (off_t)off) != (ssize_t)len)
        return -1;
    return 0;
}

static void test_fs_ext4_through_vtable(void **state)
{
    fs_vol vol;
    fs_dir dir;
    fs_dirent ent;
    fs_extent runs[2];
    size_t count = 7;
    const fs_ops *ops;
    int rc;
    int entries = 0;

    (void)state;

    /* Identification off the real image, not just the magic: the shelf hint
     * is what the personal-OS path passes after unlocking p3. */
    ops = fs_identify(ext4_img_read, NULL, FS_HINT_SHELF);
    assert_ptr_equal(ops, &fs_ops_ext4);

    memset(&vol, 0, sizeof vol);
    assert_int_equal(fs_mount(&vol, ops, ext4_img_read, NULL), FS_OK);
    assert_string_equal(fs_vol_name(&vol), "ext4");

    /* The optional capability is absent, and absent is reported honestly
     * rather than as a failure to resolve the file (DESIGN Section 9). */
    assert_false(fs_has_extents(&vol));

    assert_int_equal(fs_dir_open_root(&vol, &dir), FS_OK);
    while ((rc = fs_dir_next(&dir, &ent)) == 1) {
        /* Names arrive widened, and every name on this fixture is ASCII, so
         * the two views agree byte for byte. */
        uint8_t narrow[FS_NAME_MAX + 1];

        assert_int_equal(fs_name_to_bytes(ent.name, narrow, sizeof narrow),
                         ent.name_len);
        assert_true(ent.name_len > 0);
        /* ext4 directory entries carry no size; the inode does. */
        assert_int_equal(ent.size, 0);
        entries++;
        if (ent.is_regular) {
            fs_file file;

            assert_int_equal(fs_file_open(&vol, &file, &ent), FS_OK);
            assert_int_equal(fs_extents(&vol, &ent, runs, 2, &count),
                             FS_ERR_NO_CAPABILITY);
            assert_int_equal(count, 0);
        }
    }
    assert_int_equal(rc, FS_OK);
    assert_true(entries > 0);
}

/* The rest of the ext4 vtable: opening a subdirectory from an entry rather
 * than by root, and reading a file's bytes and size through the registry.
 * The test above opens files and never reads one, which left three of the
 * adapter's operations never called at all. */
static void test_fs_ext4_descend_and_read(void **state)
{
    fs_vol vol;
    fs_dir dir;
    fs_dirent ent;
    int rc;
    int descended = 0;
    int read_one = 0;

    (void)state;

    memset(&vol, 0, sizeof vol);
    assert_int_equal(fs_mount(&vol, &fs_ops_ext4, ext4_img_read, NULL), FS_OK);
    assert_int_equal(fs_dir_open_root(&vol, &dir), FS_OK);

    while ((rc = fs_dir_next(&dir, &ent)) == 1) {
        if (ent.is_dir && !descended) {
            fs_dir sub;
            fs_dirent sent;
            int inner = 0;

            /* fs_dir_open takes an entry, unlike fs_dir_open_root, and is
             * the only way a consumer walks below the top of a volume. */
            assert_int_equal(fs_dir_open(&vol, &sub, &ent), FS_OK);
            while ((rc = fs_dir_next(&sub, &sent)) == 1) {
                inner++;
            }
            assert_int_equal(rc, FS_OK);
            (void)inner;   /* an empty subdirectory is still a walk */
            descended = 1;
            continue;
        }
        if (ent.is_regular && !read_one) {
            fs_file file;
            uint64_t size;
            uint8_t buf[512];
            uint64_t take;

            assert_int_equal(fs_file_open(&vol, &file, &ent), FS_OK);

            /* The size comes from the inode, which is why the dirent above
             * reports zero and this does not. */
            size = fs_file_size(&file);
            if (size == 0) {
                continue;          /* an empty file proves nothing here */
            }
            take = (size < sizeof buf) ? size : sizeof buf;
            assert_int_equal(fs_file_read(&file, 0, take, buf), FS_OK);

            /* Past the end is refused rather than served short. */
            assert_int_not_equal(fs_file_read(&file, size, 1u, buf), FS_OK);
            read_one = 1;
        }
    }
    assert_int_equal(rc, FS_OK);
    assert_true(descended);
    assert_true(read_one);
}


/* The UCS-2 name helpers every consumer matches with. ASCII case folding on
 * both sides, because the two namespaces of one image disagree about case and
 * so do the fingerprints in falcon-core/src/media/family.c. */
static void test_fs_name_helpers(void **state)
{
    static const uint16_t iso[]   = { 'A','r','c','h','.','I','S','O', 0 };
    static const uint16_t other[] = { 'a','r','c','h','.','i','m','g', 0 };
    static const uint16_t shortn[] = { 'a', 0 };
    static const uint16_t empty[]  = { 0 };

    (void)state;

    /* Equality folds case and requires both sides to end together. */
    assert_true(fs_name_eq_ascii(iso, "arch.iso"));
    assert_true(fs_name_eq_ascii(iso, "ARCH.ISO"));
    assert_false(fs_name_eq_ascii(iso, "arch.isox"));  /* ascii runs on */
    assert_false(fs_name_eq_ascii(iso, "arch.is"));    /* name runs on */
    assert_false(fs_name_eq_ascii(iso, "arch.isp"));   /* same length, one
                                                          character differs */
    assert_false(fs_name_eq_ascii(iso, "brch.iso"));   /* and at the front */
    assert_false(fs_name_eq_ascii(iso, (const char *)0));
    assert_false(fs_name_eq_ascii((const uint16_t *)0, "arch.iso"));

    /* Suffix matching, which is how the menu picks .iso and .img out of a
     * directory listing. */
    assert_true(fs_name_ends_ascii(iso, ".iso"));
    assert_true(fs_name_ends_ascii(iso, ".ISO"));
    assert_false(fs_name_ends_ascii(iso, ".img"));     /* differs inside */
    assert_false(fs_name_ends_ascii(shortn, ".iso"));  /* shorter than the
                                                          suffix */
    assert_false(fs_name_ends_ascii(iso, ""));         /* nothing is not a
                                                          suffix of anything */
    assert_false(fs_name_ends_ascii(iso, (const char *)0));
    assert_false(fs_name_ends_ascii((const uint16_t *)0, ".iso"));

    /* Ordering, which is what the menu sorts on. Folded, so case never
     * decides. */
    assert_true(fs_name_cmp(iso, other) > 0);          /* 's' after 'm' */
    assert_true(fs_name_cmp(other, iso) < 0);
    assert_int_equal(fs_name_cmp(iso, iso), 0);
    assert_true(fs_name_cmp(shortn, iso) < 0);         /* prefix sorts first */
    assert_true(fs_name_cmp(iso, shortn) > 0);
    assert_int_equal(fs_name_cmp(empty, empty), 0);
}


/* Every arm of every guard on the public entry points.
 *
 * The bad-args test above passes a volume that never mounted, which is only
 * ever the LAST arm of each `||`: the earlier ones are evaluated and found
 * false and nothing exercises them as true. Lines stay at 100% and branches
 * do not, which is precisely where an inverted test or a missing check sits
 * unnoticed. Each arm here is reached on its own, with the arms before it
 * valid. */
static void test_fs_entry_guard_arms(void **state)
{
    fs_vol vol;
    fs_dir dir;
    fs_file file;
    fs_dirent ent;
    fs_extent runs[2];
    size_t count = 1;
    uint8_t narrow[8];
    uint16_t wide[8];
    synth_sb sb;

    (void)state;
    memset(&vol, 0, sizeof vol);
    memset(&dir, 0, sizeof dir);
    memset(&file, 0, sizeof file);
    memset(&ent, 0, sizeof ent);

    /* First arm: the container itself. Second arm: the output. The third,
     * an unmounted volume, is what the test above already covers. */
    assert_int_equal(fs_dir_open_root(NULL, &dir), FS_ERR_BAD_ARG);
    assert_int_equal(fs_dir_open_root(&vol, NULL), FS_ERR_BAD_ARG);

    assert_int_equal(fs_dir_open(NULL, &dir, &ent), FS_ERR_BAD_ARG);
    assert_int_equal(fs_dir_open(&vol, NULL, &ent), FS_ERR_BAD_ARG);
    assert_int_equal(fs_dir_open(&vol, &dir, NULL), FS_ERR_BAD_ARG);

    assert_int_equal(fs_dir_next(NULL, &ent), FS_ERR_BAD_ARG);
    assert_int_equal(fs_dir_next(&dir, NULL), FS_ERR_BAD_ARG);

    assert_int_equal(fs_file_open(NULL, &file, &ent), FS_ERR_BAD_ARG);
    assert_int_equal(fs_file_open(&vol, NULL, &ent), FS_ERR_BAD_ARG);
    assert_int_equal(fs_file_open(&vol, &file, NULL), FS_ERR_BAD_ARG);

    assert_int_equal(fs_file_read(NULL, 0, 512u, runs), FS_ERR_BAD_ARG);
    assert_int_equal(fs_file_size(NULL), 0);

    assert_int_equal(fs_extents(NULL, &ent, runs, 2, &count), FS_ERR_BAD_ARG);
    assert_int_equal(fs_extents(&vol, NULL, runs, 2, &count), FS_ERR_BAD_ARG);
    assert_int_equal(fs_extents(&vol, &ent, runs, 2, NULL), FS_ERR_BAD_ARG);

    assert_string_equal(fs_vol_name(NULL), "none");
    assert_false(fs_has_extents(NULL));

    /* The name converters. Each refuses on its output and tolerates a NULL
     * input by producing an empty result, which is a different thing and has
     * its own arm. */
    assert_int_equal(fs_name_to_bytes(wide, NULL, sizeof narrow), 0);
    assert_int_equal(fs_name_to_bytes(wide, narrow, 0), 0);
    narrow[0] = 0xffu;
    assert_int_equal(fs_name_to_bytes(NULL, narrow, sizeof narrow), 0);
    assert_int_equal(narrow[0], 0);          /* terminated, not left alone */

    assert_int_equal(fs_name_from_bytes(narrow, 4u, NULL, sizeof wide), 0);
    assert_int_equal(fs_name_from_bytes(narrow, 4u, wide, 0), 0);
    wide[0] = 0xffffu;
    assert_int_equal(fs_name_from_bytes(NULL, 4u, wide, sizeof wide /
                                        sizeof wide[0]), 0);
    assert_int_equal(wide[0], 0);

    /* The boot signature is two bytes and the probe checks both. Only the
     * first was ever wrong in a test, so the second arm never decided
     * anything. */
    synth_sb_init(&sb, 0);
    memcpy(sb.buf + 3, "EXFAT   ", 8);
    sb.buf[510] = 0x55;
    sb.buf[511] = 0x00;                      /* first byte right, second not */
    assert_null(fs_identify(synth_sb_read, &sb, FS_HINT_ISO));

    sb.buf[510] = 0x00;
    sb.buf[511] = 0xAA;                      /* and the other way round */
    assert_null(fs_identify(synth_sb_read, &sb, FS_HINT_ISO));

    sb.buf[510] = 0x55;
    sb.buf[511] = 0xAA;
    assert_ptr_equal(fs_identify(synth_sb_read, &sb, FS_HINT_ISO),
                     &fs_ops_exfat);
}

int run_fs_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_fs_registry_lookup),
        cmocka_unit_test(test_fs_identify_exfat_any_hint),
        cmocka_unit_test(test_fs_identify_ext4_any_hint),
        cmocka_unit_test(test_fs_identify_nothing),
        cmocka_unit_test(test_fs_probe_rejects_partial_magic),
        cmocka_unit_test(test_fs_status_map_exfat_total),
        cmocka_unit_test(test_fs_status_map_ext4_total),
        cmocka_unit_test(test_fs_status_map_media_total),
        cmocka_unit_test(test_fs_status_map_sweeps),
        cmocka_unit_test(test_fs_name_round_trip),
        cmocka_unit_test(test_fs_name_conversion_bounds),
        cmocka_unit_test(test_fs_name_compare),
        cmocka_unit_test(test_fs_exfat_through_vtable),
        cmocka_unit_test(test_fs_exfat_extents_through_vtable),
        cmocka_unit_test(test_fs_unmounted_and_bad_args),
        cmocka_unit_test(test_fs_failed_mount_leaves_no_vtable),
        cmocka_unit_test(test_fs_name_helpers),
        cmocka_unit_test(test_fs_entry_guard_arms)
    };
    const struct CMUnitTest ext4_tests[] = {
        cmocka_unit_test(test_fs_ext4_through_vtable),
        cmocka_unit_test(test_fs_ext4_descend_and_read)
    };
    int failed;

    failed = cmocka_run_group_tests_name("fs", tests, NULL, NULL);

    /* The ext4 half needs the fixture image, and skips itself the way the
     * ext4 group does when it is not there. */
    ext4_fd = open(EXT4_IMG, O_RDONLY);
    if (ext4_fd < 0) {
        printf("\n[  SKIPPED ] fs over ext4: %s not found.\n"
               "             Build it with: make test-ext4 "
               "(needs mkfs.ext4 and sudo).\n", EXT4_IMG);
        return failed + test_skip_rc();
    }
    failed += cmocka_run_group_tests_name("fs/ext4", ext4_tests, NULL, NULL);
    close(ext4_fd);
    ext4_fd = -1;
    return failed;
}
