/* The filesystem registry of DESIGN Section 9: the vtable, the two adapters
 * that dress the existing drivers in it, and the static table consumers pick
 * from.
 *
 * The adapters are deliberately thin and deliberately not clever. Each one
 * casts the caller's opaque blob to its driver's context, rebuilds the
 * driver's own dirent from the unified one when a call needs it, and maps the
 * status back. No driver source changed to make this work, which is the
 * property that lets the driver test groups keep pinning driver behaviour
 * unchanged: if an adapter is wrong, the fs group catches it, and the driver
 * groups stay the statement about the driver.
 *
 * Why the dirent is rebuilt rather than carried whole: the driver dirents are
 * mostly name (528 and 264 bytes, of which 512 and 256 are the name), and the
 * unified dirent already carries the name in the one canonical form. So the
 * private part is the handful of bytes that actually locate the object -
 * exFAT's first cluster and chain flag, ext4's inode and file type - and the
 * unified dirent stays roughly the size of the driver dirent it replaces
 * instead of the sum of both.
 */

#include <fs/fs.h>

#include <fs/exfat.h>
#include <fs/ext4.h>
#include <fs/iso9660.h>
#include <fs/udf.h>
#include <lib/string.h>

/* The blobs must hold every registered driver's context. A driver that grows
 * past these breaks the build here rather than overrunning a caller's
 * object at run time. */
_Static_assert(sizeof(exfat_vol) <= FS_VOL_SIZE, "exfat_vol exceeds FS_VOL_SIZE");
_Static_assert(sizeof(exfat_dir) <= FS_DIR_SIZE, "exfat_dir exceeds FS_DIR_SIZE");
_Static_assert(sizeof(exfat_file) <= FS_FILE_SIZE, "exfat_file exceeds FS_FILE_SIZE");
_Static_assert(sizeof(ext4_vol) <= FS_VOL_SIZE, "ext4_vol exceeds FS_VOL_SIZE");
_Static_assert(sizeof(ext4_dir) <= FS_DIR_SIZE, "ext4_dir exceeds FS_DIR_SIZE");
_Static_assert(sizeof(ext4_file) <= FS_FILE_SIZE, "ext4_file exceeds FS_FILE_SIZE");
_Static_assert(sizeof(iso9660_vol) <= FS_VOL_SIZE, "iso9660_vol exceeds FS_VOL_SIZE");
_Static_assert(sizeof(iso9660_dir) <= FS_DIR_SIZE, "iso9660_dir exceeds FS_DIR_SIZE");
_Static_assert(sizeof(iso9660_file) <= FS_FILE_SIZE, "iso9660_file exceeds FS_FILE_SIZE");
_Static_assert(sizeof(udf_vol) <= FS_VOL_SIZE, "udf_vol exceeds FS_VOL_SIZE");
_Static_assert(sizeof(udf_dir) <= FS_DIR_SIZE, "udf_dir exceeds FS_DIR_SIZE");
_Static_assert(sizeof(udf_file) <= FS_FILE_SIZE, "udf_file exceeds FS_FILE_SIZE");

/* The extent map's shape is shared rather than converted: exfat_extent and
 * fs_extent are the same two fields in the same order, and keeping them
 * interchangeable is what lets the wrapper hand the driver the caller's array
 * directly instead of copying a potentially large run table twice. */
_Static_assert(sizeof(fs_extent) == sizeof(exfat_extent),
               "fs_extent and exfat_extent must stay layout-compatible");
_Static_assert(sizeof(fs_extent) == sizeof(iso9660_extent),
               "fs_extent and iso9660_extent must stay layout-compatible");
_Static_assert(sizeof(fs_extent) == sizeof(udf_extent),
               "fs_extent and udf_extent must stay layout-compatible");

/* ---- name helpers ---- */

static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

size_t fs_name_to_bytes(const uint16_t *name, uint8_t *out, size_t cap)
{
    size_t i = 0;

    if (out == (uint8_t *)0 || cap == 0)
        return 0;
    if (name != (const uint16_t *)0) {
        while (name[i] != 0 && i + 1 < cap) {
            out[i] = (name[i] <= 0xFFu) ? (uint8_t)name[i] : (uint8_t)'?';
            i++;
        }
    }
    out[i] = 0;
    return i;
}

size_t fs_name_from_bytes(const uint8_t *name, size_t len, uint16_t *out,
                          size_t cap)
{
    size_t i = 0;

    if (out == (uint16_t *)0 || cap == 0)
        return 0;
    if (name != (const uint8_t *)0) {
        while (i < len && i + 1 < cap) {
            out[i] = name[i];
            i++;
        }
    }
    out[i] = 0;
    return i;
}

int fs_name_eq_ascii(const uint16_t *name, const char *ascii)
{
    size_t i = 0;

    if (name == (const uint16_t *)0 || ascii == (const char *)0)
        return 0;
    while (name[i] != 0) {
        if (ascii[i] == '\0')
            return 0;
        if (ascii_lower((int)name[i]) != ascii_lower((int)(unsigned char)ascii[i]))
            return 0;
        i++;
    }
    return ascii[i] == '\0';
}

int fs_name_ends_ascii(const uint16_t *name, const char *suffix)
{
    size_t nl = 0;
    size_t sl = 0;
    size_t i;

    if (name == (const uint16_t *)0 || suffix == (const char *)0)
        return 0;
    while (name[nl] != 0)
        nl++;
    while (suffix[sl] != '\0')
        sl++;
    if (sl == 0 || nl < sl)
        return 0;
    for (i = 0; i < sl; i++) {
        if (ascii_lower((int)name[nl - sl + i]) !=
            ascii_lower((int)(unsigned char)suffix[i]))
            return 0;
    }
    return 1;
}

int fs_name_cmp(const uint16_t *a, const uint16_t *b)
{
    size_t i = 0;

    while (a[i] != 0 && b[i] != 0) {
        int ca = ascii_lower((int)a[i]);
        int cb = ascii_lower((int)b[i]);

        if (ca != cb)
            return (ca < cb) ? -1 : 1;
        i++;
    }
    if (a[i] == b[i])
        return 0;
    return (a[i] == 0) ? -1 : 1;
}

/* ---- status mapping ----
 *
 * One row per driver status. The drivers' own enums are never renumbered:
 * their tests pin those values and they are the right vocabulary inside the
 * driver. The tables are what makes the two vocabularies one at the seam. */

typedef struct {
    int driver;
    int unified;
} status_row;

static int map_status(const status_row *rows, size_t n, int rc)
{
    size_t i;

    if (rc >= 0)
        return rc; /* OK, and dir_next's "1 = entry produced", pass through */
    for (i = 0; i < n; i++) {
        if (rows[i].driver == rc)
            return rows[i].unified;
    }
    /* A driver that grew a status without a row here: report the
     * conservative answer rather than leak an unmapped number. */
    return FS_ERR_CORRUPT;
}

static const status_row exfat_rows[] = {
    { EXFAT_ERR_IO,        FS_ERR_IO },
    { EXFAT_ERR_BAD_SUPER, FS_ERR_BAD_SUPER },
    { EXFAT_ERR_CORRUPT,   FS_ERR_CORRUPT },
    { EXFAT_ERR_BAD_ARG,   FS_ERR_BAD_ARG },
    { EXFAT_ERR_RANGE,     FS_ERR_RANGE },
    { EXFAT_ERR_NOSPACE,   FS_ERR_NOSPACE }
};

static const status_row ext4_rows[] = {
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

int fs_status_from_exfat(int rc)
{
    return map_status(exfat_rows, sizeof exfat_rows / sizeof exfat_rows[0], rc);
}

int fs_status_from_ext4(int rc)
{
    return map_status(ext4_rows, sizeof ext4_rows / sizeof ext4_rows[0], rc);
}

static const status_row iso9660_rows[] = {
    { ISO9660_ERR_IO,          FS_ERR_IO },
    { ISO9660_ERR_BAD_SUPER,   FS_ERR_BAD_SUPER },
    { ISO9660_ERR_CORRUPT,     FS_ERR_CORRUPT },
    { ISO9660_ERR_BAD_ARG,     FS_ERR_BAD_ARG },
    { ISO9660_ERR_RANGE,       FS_ERR_RANGE },
    { ISO9660_ERR_NOSPACE,     FS_ERR_NOSPACE },
    { ISO9660_ERR_UNSUPPORTED, FS_ERR_UNSUPPORTED },
    { ISO9660_ERR_SYMLINK,     FS_ERR_SYMLINK }
};

static const status_row udf_rows[] = {
    { UDF_ERR_IO,          FS_ERR_IO },
    { UDF_ERR_BAD_SUPER,   FS_ERR_BAD_SUPER },
    { UDF_ERR_CORRUPT,     FS_ERR_CORRUPT },
    { UDF_ERR_BAD_ARG,     FS_ERR_BAD_ARG },
    { UDF_ERR_RANGE,       FS_ERR_RANGE },
    { UDF_ERR_NOSPACE,     FS_ERR_NOSPACE },
    { UDF_ERR_UNSUPPORTED, FS_ERR_UNSUPPORTED },
    { UDF_ERR_SYMLINK,     FS_ERR_SYMLINK },
    { UDF_ERR_NOT_FOUND,   FS_ERR_NOT_FOUND }
};

int fs_status_from_iso9660(int rc)
{
    return map_status(iso9660_rows,
                      sizeof iso9660_rows / sizeof iso9660_rows[0], rc);
}

int fs_status_from_udf(int rc)
{
    return map_status(udf_rows, sizeof udf_rows / sizeof udf_rows[0], rc);
}

/* ---- blob access ----
 *
 * The round trip through void * is what keeps -Wcast-align quiet about a cast
 * that is in fact aligned: the blob is a union with a uint64_t member, so it
 * carries the alignment every driver context needs, and the compiler cannot
 * see that through the unsigned char array alone. */

static exfat_vol *xvol(fs_vol *v) { return (exfat_vol *)(void *)v->priv.b; }
static exfat_dir *xdir(fs_dir *d) { return (exfat_dir *)(void *)d->priv.b; }
static exfat_file *xfile(fs_file *f) { return (exfat_file *)(void *)f->priv.b; }
static const exfat_file *xfile_c(const fs_file *f)
{
    return (const exfat_file *)(const void *)f->priv.b;
}

static iso9660_vol *ivol(fs_vol *v) { return (iso9660_vol *)(void *)v->priv.b; }
static const iso9660_vol *ivol_c(const fs_vol *v)
{
    return (const iso9660_vol *)(const void *)v->priv.b;
}
static iso9660_dir *idir(fs_dir *d) { return (iso9660_dir *)(void *)d->priv.b; }
static iso9660_file *ifile(fs_file *f) { return (iso9660_file *)(void *)f->priv.b; }
static const iso9660_file *ifile_c(const fs_file *f)
{
    return (const iso9660_file *)(const void *)f->priv.b;
}

static udf_vol *uvol(fs_vol *v) { return (udf_vol *)(void *)v->priv.b; }
static const udf_vol *uvol_c(const fs_vol *v)
{
    return (const udf_vol *)(const void *)v->priv.b;
}
static udf_dir *udir(fs_dir *d) { return (udf_dir *)(void *)d->priv.b; }
static udf_file *ufile(fs_file *f) { return (udf_file *)(void *)f->priv.b; }
static const udf_file *ufile_c(const fs_file *f)
{
    return (const udf_file *)(const void *)f->priv.b;
}

static ext4_vol *evol(fs_vol *v) { return (ext4_vol *)(void *)v->priv.b; }
static ext4_dir *edir(fs_dir *d) { return (ext4_dir *)(void *)d->priv.b; }
static ext4_file *efile(fs_file *f) { return (ext4_file *)(void *)f->priv.b; }
static const ext4_file *efile_c(const fs_file *f)
{
    return (const ext4_file *)(const void *)f->priv.b;
}

/* ---- the exFAT adapter ---- */

/* What an exFAT dirent needs to be rebuilt from a unified one. valid_len
 * rides along because a sparse file's tail (valid..size) must read as
 * zeros, and dropping it here would resurrect the truncation bug the
 * driver just fixed via a different door. */
typedef struct {
    uint64_t valid_len;
    uint32_t first_cluster;
    uint8_t  no_fat_chain;
} exfat_priv;

_Static_assert(sizeof(exfat_priv) <= FS_DIRENT_PRIV_SIZE,
               "exfat dirent private part exceeds FS_DIRENT_PRIV_SIZE");

static void exfat_to_fs_dirent(const exfat_dirent *in, fs_dirent *out)
{
    exfat_priv p;
    size_t len = in->name_len;
    size_t i;

    memset(out, 0, sizeof(*out));
    /* The FS_NAME_MAX bound cannot decide the loop today and is kept anyway.
     * exfat_dirent carries at most EXFAT_NAME_MAX units and both constants
     * are 255, so the two conditions fail on the same iteration and only the
     * first is ever the one that ends it. What it guards is out->name, which
     * is FS_NAME_MAX + 1 units: if either limit moves and this is gone, a
     * long exFAT name writes past the end of a unified dirent. */
    for (i = 0; i < len && i < FS_NAME_MAX; i++)
        out->name[i] = in->name[i];
    out->name[i] = 0;
    out->name_len = (uint16_t)i;
    out->size = in->size;
    out->is_dir = in->is_dir;
    /* exFAT has no device nodes and no symlinks: anything that is not a
     * directory is a byte stream. */
    out->is_regular = (uint8_t)(in->is_dir ? 0 : 1);

    p.valid_len = in->valid_len;
    p.first_cluster = in->first_cluster;
    p.no_fat_chain = in->no_fat_chain;
    memcpy(out->priv.b, &p, sizeof p);
}

static void fs_to_exfat_dirent(const fs_dirent *in, exfat_dirent *out)
{
    exfat_priv p;

    memcpy(&p, in->priv.b, sizeof p);
    memset(out, 0, sizeof(*out));
    /* The name is not carried back: no exFAT call reads it, and copying it
     * would mean widening 512 bytes on every file open for nothing. */
    out->is_dir = in->is_dir;
    out->size = in->size;
    out->valid_len = p.valid_len;
    out->first_cluster = p.first_cluster;
    out->no_fat_chain = p.no_fat_chain;
}

static int exfat_probe_op(fs_read_fn read, void *ctx)
{
    uint8_t vbr[512];

    /* Exactly the read exfat_mount opens with, and exactly the two fields it
     * refuses on, so probe and mount agree about what an exFAT volume is. */
    if (read(ctx, 0, sizeof vbr, vbr) != 0)
        return FS_ERR_IO;
    if (memcmp(vbr + 3, "EXFAT   ", 8) != 0)
        return FS_ERR_BAD_SUPER;
    if (vbr[510] != 0x55 || vbr[511] != 0xAA)
        return FS_ERR_BAD_SUPER;
    return FS_OK;
}

static int exfat_mount_op(fs_vol *v, fs_read_fn read, void *ctx)
{
    return fs_status_from_exfat(exfat_mount(xvol(v), read, ctx));
}

static int exfat_dir_open_root_op(fs_vol *v, fs_dir *d)
{
    return fs_status_from_exfat(exfat_dir_open_root(xvol(v), xdir(d)));
}

static int exfat_dir_open_op(fs_vol *v, fs_dir *d, const fs_dirent *e)
{
    exfat_dirent de;

    fs_to_exfat_dirent(e, &de);
    return fs_status_from_exfat(exfat_dir_open(xvol(v), xdir(d), &de));
}

static int exfat_dir_next_op(fs_dir *d, fs_dirent *out)
{
    exfat_dirent de;
    int rc = exfat_dir_next(xdir(d), &de);

    if (rc == 1)
        exfat_to_fs_dirent(&de, out);
    return fs_status_from_exfat(rc);
}

static int exfat_file_open_op(fs_vol *v, fs_file *f, const fs_dirent *e)
{
    exfat_dirent de;

    fs_to_exfat_dirent(e, &de);
    return fs_status_from_exfat(exfat_file_open(xvol(v), xfile(f), &de));
}

static int exfat_file_read_op(fs_file *f, uint64_t off, uint64_t len,
                              void *buf)
{
    return fs_status_from_exfat(exfat_file_read(xfile(f), off, len, buf));
}

static uint64_t exfat_file_size_op(const fs_file *f)
{
    return xfile_c(f)->size;
}

static int exfat_extents_op(fs_vol *v, const fs_dirent *e, fs_extent *out,
                            size_t cap, size_t *count)
{
    exfat_dirent de;

    fs_to_exfat_dirent(e, &de);
    return fs_status_from_exfat(
        exfat_file_extents(xvol(v), &de, (exfat_extent *)(void *)out, cap,
                           count));
}

const fs_ops fs_ops_exfat = {
    "exfat",
    FS_HINT_ISO,
    exfat_probe_op,
    exfat_mount_op,
    exfat_dir_open_root_op,
    exfat_dir_open_op,
    exfat_dir_next_op,
    exfat_file_open_op,
    exfat_file_read_op,
    exfat_file_size_op,
    exfat_extents_op,
    (const char *(*)(const fs_vol *))0
};

/* ---- the ext4 adapter ---- */

typedef struct {
    uint32_t ino;
    uint8_t  file_type;
} ext4_priv;

_Static_assert(sizeof(ext4_priv) <= FS_DIRENT_PRIV_SIZE,
               "ext4 dirent private part exceeds FS_DIRENT_PRIV_SIZE");

static void ext4_to_fs_dirent(const ext4_dirent *in, fs_dirent *out)
{
    ext4_priv p;

    memset(out, 0, sizeof(*out));
    out->name_len =
        (uint16_t)fs_name_from_bytes(in->name, in->name_len, out->name,
                                     FS_NAME_MAX + 1u);
    /* ext4 directory entries carry no size; the inode does, so a consumer
     * that wants one opens the file and asks fs_file_size. */
    out->size = 0;
    out->is_dir = in->is_dir;
    out->is_regular = in->is_regular;

    p.ino = in->ino;
    p.file_type = in->file_type;
    memcpy(out->priv.b, &p, sizeof p);
}

static void fs_to_ext4_dirent(const fs_dirent *in, ext4_dirent *out)
{
    ext4_priv p;

    memcpy(&p, in->priv.b, sizeof p);
    memset(out, 0, sizeof(*out));
    out->is_dir = in->is_dir;
    out->is_regular = in->is_regular;
    out->file_type = p.file_type;
    out->ino = p.ino;
}

static int ext4_probe_op(fs_read_fn read, void *ctx)
{
    uint8_t sb[1024];

    /* The superblock read ext4_mount opens with, and its magic. Everything
     * else it refuses on (revision, feature bits, clean state) is validation
     * and belongs to mount. */
    if (read(ctx, 1024, sizeof sb, sb) != 0)
        return FS_ERR_IO;
    if (get_le16(sb + 0x38) != 0xEF53u)
        return FS_ERR_BAD_SUPER;
    return FS_OK;
}

static int ext4_mount_op(fs_vol *v, fs_read_fn read, void *ctx)
{
    return fs_status_from_ext4(ext4_mount(evol(v), read, ctx));
}

static int ext4_dir_open_root_op(fs_vol *v, fs_dir *d)
{
    return fs_status_from_ext4(ext4_dir_open_root(evol(v), edir(d)));
}

static int ext4_dir_open_op(fs_vol *v, fs_dir *d, const fs_dirent *e)
{
    ext4_dirent de;

    fs_to_ext4_dirent(e, &de);
    return fs_status_from_ext4(ext4_dir_open(evol(v), edir(d), &de));
}

static int ext4_dir_next_op(fs_dir *d, fs_dirent *out)
{
    ext4_dirent de;
    int rc = ext4_dir_next(edir(d), &de);

    if (rc == 1)
        ext4_to_fs_dirent(&de, out);
    return fs_status_from_ext4(rc);
}

static int ext4_file_open_op(fs_vol *v, fs_file *f, const fs_dirent *e)
{
    ext4_dirent de;

    fs_to_ext4_dirent(e, &de);
    return fs_status_from_ext4(ext4_file_open(evol(v), efile(f), &de));
}

static int ext4_file_read_op(fs_file *f, uint64_t off, uint64_t len, void *buf)
{
    return fs_status_from_ext4(ext4_file_read(efile(f), off, len, buf));
}

static uint64_t ext4_file_size_op(const fs_file *f)
{
    /* ext4_file_size takes a non-const pointer nowhere: it reads i_size out
     * of the cached inode, so the const view is honest. */
    return ext4_file_size(efile_c(f));
}

const fs_ops fs_ops_ext4 = {
    "ext4",
    FS_HINT_SHELF,
    ext4_probe_op,
    ext4_mount_op,
    ext4_dir_open_root_op,
    ext4_dir_open_op,
    ext4_dir_next_op,
    ext4_file_open_op,
    ext4_file_read_op,
    ext4_file_size_op,
    /* No extent map. DESIGN Section 9 makes this an optional capability, and
     * the one consumer that needs it (the ISO injection framework) only ever
     * asks about p2. Writing one for p3 would be code with no caller. */
    (int (*)(fs_vol *, const fs_dirent *, fs_extent *, size_t, size_t *))0,
    (const char *(*)(const fs_vol *))0
};

/* ---- the ISO9660 adapter ---- */

/* What an ISO9660 dirent needs back: the first extent, and where its first
 * record sits so a multi-extent file's later parts can be found again. */
typedef struct {
    uint32_t lba;
    uint32_t rec_lba;
    uint16_t rec_off;
    uint8_t  parts;
    uint8_t  compressed;
} iso9660_priv;

_Static_assert(sizeof(iso9660_priv) <= FS_DIRENT_PRIV_SIZE,
               "iso9660 dirent private part exceeds FS_DIRENT_PRIV_SIZE");

static void iso9660_to_fs_dirent(const iso9660_dirent *in, fs_dirent *out)
{
    iso9660_priv p;
    size_t i;

    memset(out, 0, sizeof(*out));
    for (i = 0; i < in->name_len && i < FS_NAME_MAX; i++)
        out->name[i] = in->name[i];
    out->name[i] = 0;
    out->name_len = (uint16_t)i;
    out->size = in->size;
    out->is_dir = in->is_dir;
    out->is_regular = in->is_regular;

    p.lba = in->lba;
    p.rec_lba = in->rec_lba;
    p.rec_off = in->rec_off;
    p.parts = in->parts;
    p.compressed = in->compressed;
    memcpy(out->priv.b, &p, sizeof p);
}

static void fs_to_iso9660_dirent(const fs_dirent *in, iso9660_dirent *out)
{
    iso9660_priv p;

    memcpy(&p, in->priv.b, sizeof p);
    memset(out, 0, sizeof(*out));
    out->is_dir = in->is_dir;
    out->is_regular = in->is_regular;
    out->size = in->size;
    out->lba = p.lba;
    out->rec_lba = p.rec_lba;
    out->rec_off = p.rec_off;
    out->parts = p.parts;
    out->compressed = p.compressed;
}

static int iso9660_probe_op(fs_read_fn read, void *ctx)
{
    return fs_status_from_iso9660(iso9660_probe(read, ctx));
}

static int iso9660_mount_op(fs_vol *v, fs_read_fn read, void *ctx)
{
    return fs_status_from_iso9660(iso9660_mount(ivol(v), read, ctx));
}

static int iso9660_dir_open_root_op(fs_vol *v, fs_dir *d)
{
    return fs_status_from_iso9660(iso9660_dir_open_root(ivol(v), idir(d)));
}

static int iso9660_dir_open_op(fs_vol *v, fs_dir *d, const fs_dirent *e)
{
    iso9660_dirent de;

    fs_to_iso9660_dirent(e, &de);
    return fs_status_from_iso9660(iso9660_dir_open(ivol(v), idir(d), &de));
}

static int iso9660_dir_next_op(fs_dir *d, fs_dirent *out)
{
    iso9660_dirent de;
    int rc = iso9660_dir_next(idir(d), &de);

    if (rc == 1)
        iso9660_to_fs_dirent(&de, out);
    return fs_status_from_iso9660(rc);
}

static int iso9660_file_open_op(fs_vol *v, fs_file *f, const fs_dirent *e)
{
    iso9660_dirent de;

    fs_to_iso9660_dirent(e, &de);
    return fs_status_from_iso9660(iso9660_file_open(ivol(v), ifile(f), &de));
}

static int iso9660_file_read_op(fs_file *f, uint64_t off, uint64_t len,
                                void *buf)
{
    return fs_status_from_iso9660(iso9660_file_read(ifile(f), off, len, buf));
}

static uint64_t iso9660_file_size_op(const fs_file *f)
{
    return ifile_c(f)->size;
}

static int iso9660_extents_op(fs_vol *v, const fs_dirent *e, fs_extent *out,
                              size_t cap, size_t *count)
{
    iso9660_dirent de;

    fs_to_iso9660_dirent(e, &de);
    return fs_status_from_iso9660(
        iso9660_file_extents(ivol(v), &de, (iso9660_extent *)(void *)out, cap,
                             count));
}

static const char *iso9660_label_op(const fs_vol *v)
{
    return ivol_c(v)->label;
}

const fs_ops fs_ops_iso9660 = {
    "iso9660",
    FS_HINT_NONE,
    iso9660_probe_op,
    iso9660_mount_op,
    iso9660_dir_open_root_op,
    iso9660_dir_open_op,
    iso9660_dir_next_op,
    iso9660_file_open_op,
    iso9660_file_read_op,
    iso9660_file_size_op,
    iso9660_extents_op,
    iso9660_label_op
};

/* ---- the UDF adapter ---- */

typedef struct {
    uint32_t icb_lbn;
} udf_priv;

_Static_assert(sizeof(udf_priv) <= FS_DIRENT_PRIV_SIZE,
               "udf dirent private part exceeds FS_DIRENT_PRIV_SIZE");

static void udf_to_fs_dirent(const udf_dirent *in, fs_dirent *out)
{
    udf_priv p;
    size_t i;

    memset(out, 0, sizeof(*out));
    for (i = 0; i < in->name_len && i < FS_NAME_MAX; i++)
        out->name[i] = in->name[i];
    out->name[i] = 0;
    out->name_len = (uint16_t)i;
    out->size = in->size;
    out->is_dir = in->is_dir;
    out->is_regular = in->is_regular;
    p.icb_lbn = in->icb_lbn;
    memcpy(out->priv.b, &p, sizeof p);
}

static void fs_to_udf_dirent(const fs_dirent *in, udf_dirent *out)
{
    udf_priv p;

    memcpy(&p, in->priv.b, sizeof p);
    memset(out, 0, sizeof(*out));
    out->is_dir = in->is_dir;
    out->is_regular = in->is_regular;
    out->size = in->size;
    out->icb_lbn = p.icb_lbn;
}

static int udf_probe_op(fs_read_fn read, void *ctx)
{
    return fs_status_from_udf(udf_probe(read, ctx));
}

static int udf_mount_op(fs_vol *v, fs_read_fn read, void *ctx)
{
    return fs_status_from_udf(udf_mount(uvol(v), read, ctx));
}

static int udf_dir_open_root_op(fs_vol *v, fs_dir *d)
{
    return fs_status_from_udf(udf_dir_open_root(uvol(v), udir(d)));
}

static int udf_dir_open_op(fs_vol *v, fs_dir *d, const fs_dirent *e)
{
    udf_dirent de;

    fs_to_udf_dirent(e, &de);
    return fs_status_from_udf(udf_dir_open(uvol(v), udir(d), &de));
}

static int udf_dir_next_op(fs_dir *d, fs_dirent *out)
{
    udf_dirent de;
    int rc = udf_dir_next(udir(d), &de);

    if (rc == 1)
        udf_to_fs_dirent(&de, out);
    return fs_status_from_udf(rc);
}

static int udf_file_open_op(fs_vol *v, fs_file *f, const fs_dirent *e)
{
    udf_dirent de;

    fs_to_udf_dirent(e, &de);
    return fs_status_from_udf(udf_file_open(uvol(v), ufile(f), &de));
}

static int udf_file_read_op(fs_file *f, uint64_t off, uint64_t len, void *buf)
{
    return fs_status_from_udf(udf_file_read(ufile(f), off, len, buf));
}

static uint64_t udf_file_size_op(const fs_file *f)
{
    return ufile_c(f)->size;
}

static int udf_extents_op(fs_vol *v, const fs_dirent *e, fs_extent *out,
                          size_t cap, size_t *count)
{
    udf_dirent de;

    fs_to_udf_dirent(e, &de);
    return fs_status_from_udf(
        udf_file_extents(uvol(v), &de, (udf_extent *)(void *)out, cap, count));
}

static const char *udf_label_op(const fs_vol *v)
{
    return uvol_c(v)->label;
}

const fs_ops fs_ops_udf = {
    "udf",
    FS_HINT_NONE,
    udf_probe_op,
    udf_mount_op,
    udf_dir_open_root_op,
    udf_dir_open_op,
    udf_dir_next_op,
    udf_file_open_op,
    udf_file_read_op,
    udf_file_size_op,
    udf_extents_op,
    udf_label_op
};

/* ---- the registry ---- */

/* Probe order for a volume with no role hint. exFAT first because p2 is the
 * partition a stick has most of, and because its magic sits in the first
 * sector where ext4's is a kilobyte in.
 *
 * The two media drivers come last, and UDF before ISO9660, because a bridge
 * disc answers both and the UDF side is the complete one: Windows media's
 * ISO9660 tree is a README and nothing else. A plain ISO9660 image has no
 * NSR descriptor, so udf_probe passes it on. */
static const fs_ops *const registry[] = {
    &fs_ops_exfat,
    &fs_ops_ext4,
    &fs_ops_udf,
    &fs_ops_iso9660
};

#define REGISTRY_N (sizeof registry / sizeof registry[0])

size_t fs_driver_count(void)
{
    return REGISTRY_N;
}

const fs_ops *fs_driver_at(size_t i)
{
    return (i < REGISTRY_N) ? registry[i] : (const fs_ops *)0;
}

const fs_ops *fs_by_name(const char *name)
{
    size_t i;
    size_t j;

    if (name == (const char *)0)
        return (const fs_ops *)0;
    for (i = 0; i < REGISTRY_N; i++) {
        const char *n = registry[i]->name;

        for (j = 0; n[j] != '\0' && name[j] != '\0' && n[j] == name[j]; j++)
            ;
        if (n[j] == '\0' && name[j] == '\0')
            return registry[i];
    }
    return (const fs_ops *)0;
}

const fs_ops *fs_identify(fs_read_fn read, void *ctx, fs_hint hint)
{
    unsigned pass;
    size_t i;

    if (read == (fs_read_fn)0)
        return (const fs_ops *)0;

    /* Pass 0 is the role's own driver, pass 1 everything else in table
     * order. The fallback is not a nicety: an exFAT volume sitting on a
     * partition typed as the shelf is a mistake worth reporting honestly at
     * the next layer up, not a volume to declare unreadable here. */
    for (pass = 0; pass < 2u; pass++) {
        for (i = 0; i < REGISTRY_N; i++) {
            const fs_ops *o = registry[i];
            int preferred = (hint != FS_HINT_NONE && o->role == hint);

            if ((pass == 0u) != (preferred != 0))
                continue;
            if (o->probe(read, ctx) == FS_OK)
                return o;
        }
    }
    return (const fs_ops *)0;
}

/* ---- the wrappers ---- */

int fs_mount(fs_vol *v, const fs_ops *ops, fs_read_fn read, void *ctx)
{
    int rc;

    if (v == (fs_vol *)0 || ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    v->ops = (const fs_ops *)0;
    rc = ops->mount(v, read, ctx);
    if (rc == FS_OK)
        v->ops = ops;
    return rc;
}

int fs_dir_open_root(fs_vol *v, fs_dir *d)
{
    if (v == (fs_vol *)0 || d == (fs_dir *)0 || v->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    d->ops = v->ops;
    return v->ops->dir_open_root(v, d);
}

int fs_dir_open(fs_vol *v, fs_dir *d, const fs_dirent *e)
{
    if (v == (fs_vol *)0 || d == (fs_dir *)0 || e == (const fs_dirent *)0 ||
        v->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    d->ops = v->ops;
    return v->ops->dir_open(v, d, e);
}

int fs_dir_next(fs_dir *d, fs_dirent *out)
{
    if (d == (fs_dir *)0 || out == (fs_dirent *)0 ||
        d->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    return d->ops->dir_next(d, out);
}

int fs_file_open(fs_vol *v, fs_file *f, const fs_dirent *e)
{
    if (v == (fs_vol *)0 || f == (fs_file *)0 || e == (const fs_dirent *)0 ||
        v->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    f->ops = v->ops;
    return v->ops->file_open(v, f, e);
}

int fs_file_read(fs_file *f, uint64_t off, uint64_t len, void *buf)
{
    if (f == (fs_file *)0 || f->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    return f->ops->file_read(f, off, len, buf);
}

int fs_file_read_cb(void *file, uint64_t off, uint64_t len, void *buf)
{
    return fs_file_read((fs_file *)file, off, len, buf) == FS_OK ? 0 : -1;
}

uint64_t fs_file_size(const fs_file *f)
{
    if (f == (const fs_file *)0 || f->ops == (const fs_ops *)0)
        return 0;
    return f->ops->file_size(f);
}

int fs_has_extents(const fs_vol *v)
{
    if (v == (const fs_vol *)0 || v->ops == (const fs_ops *)0)
        return 0;
    return v->ops->extents != (int (*)(fs_vol *, const fs_dirent *,
                                       fs_extent *, size_t, size_t *))0;
}

int fs_extents(fs_vol *v, const fs_dirent *e, fs_extent *out, size_t cap,
               size_t *count)
{
    if (v == (fs_vol *)0 || e == (const fs_dirent *)0 ||
        count == (size_t *)0 || v->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    if (!fs_has_extents(v)) {
        *count = 0;
        return FS_ERR_NO_CAPABILITY;
    }
    return v->ops->extents(v, e, out, cap, count);
}

int fs_lookup(fs_vol *v, const char *path, fs_dir *scratch, fs_dirent *out)
{
    const char *p = path;
    int have = 0;
    int rc;

    if (v == (fs_vol *)0 || path == (const char *)0 ||
        scratch == (fs_dir *)0 || out == (fs_dirent *)0 ||
        v->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    rc = fs_dir_open_root(v, scratch);
    if (rc != FS_OK)
        return rc;
    for (;;) {
        char comp[FS_NAME_MAX + 1];
        size_t n = 0;
        fs_dirent e;
        int found = 0;

        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        while (p[n] != '\0' && p[n] != '/') {
            if (n >= FS_NAME_MAX)
                return FS_ERR_BAD_ARG;
            comp[n] = p[n];
            n++;
        }
        comp[n] = '\0';
        p += n;
        if (have) {
            if (!out->is_dir)
                return FS_ERR_NOT_FOUND;
            rc = fs_dir_open(v, scratch, out);
            if (rc != FS_OK)
                return rc;
        }
        while ((rc = fs_dir_next(scratch, &e)) == 1) {
            if (fs_name_eq_ascii(e.name, comp)) {
                found = 1;
                break;
            }
        }
        if (rc < 0)
            return rc;
        if (!found)
            return FS_ERR_NOT_FOUND;
        memcpy(out, &e, sizeof e);
        have = 1;
    }
    return have ? FS_OK : FS_ERR_BAD_ARG;
}

const char *fs_vol_name(const fs_vol *v)
{
    if (v == (const fs_vol *)0 || v->ops == (const fs_ops *)0)
        return "none";
    return v->ops->name;
}

const char *fs_vol_label(const fs_vol *v)
{
    const char *l;

    if (v == (const fs_vol *)0 || v->ops == (const fs_ops *)0 ||
        v->ops->label == (const char *(*)(const fs_vol *))0)
        return "";
    l = v->ops->label(v);
    return l != (const char *)0 ? l : "";
}
