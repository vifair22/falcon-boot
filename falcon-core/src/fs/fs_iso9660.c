/* The ISO9660 adapter of the filesystem registry (DESIGN Section 9): the
 * ISO9660 driver dressed in fs_ops. One file per driver so a build links only
 * the drivers its registry table names (fs_reg_all.c, fs_reg_ext4.c); fs.c
 * says why the adapters are thin.
 */

#include <fs/fs.h>
#include <fs/fs_adapter.h>

#include <fs/iso9660.h>
#include <lib/string.h>

/* The blobs must hold the driver's context (fs.c has the reason). */
_Static_assert(sizeof(iso9660_vol) <= FS_VOL_SIZE, "iso9660_vol exceeds FS_VOL_SIZE");
_Static_assert(sizeof(iso9660_dir) <= FS_DIR_SIZE, "iso9660_dir exceeds FS_DIR_SIZE");
_Static_assert(sizeof(iso9660_file) <= FS_FILE_SIZE, "iso9660_file exceeds FS_FILE_SIZE");

/* Shared rather than converted; fs.c has the reason. */
_Static_assert(sizeof(fs_extent) == sizeof(iso9660_extent),
               "fs_extent and iso9660_extent must stay layout-compatible");

/* ---- status mapping ---- */

static const fs_status_row iso9660_rows[] = {
    { ISO9660_ERR_IO,          FS_ERR_IO },
    { ISO9660_ERR_BAD_SUPER,   FS_ERR_BAD_SUPER },
    { ISO9660_ERR_CORRUPT,     FS_ERR_CORRUPT },
    { ISO9660_ERR_BAD_ARG,     FS_ERR_BAD_ARG },
    { ISO9660_ERR_RANGE,       FS_ERR_RANGE },
    { ISO9660_ERR_NOSPACE,     FS_ERR_NOSPACE },
    { ISO9660_ERR_UNSUPPORTED, FS_ERR_UNSUPPORTED },
    { ISO9660_ERR_SYMLINK,     FS_ERR_SYMLINK }
};

int fs_status_from_iso9660(int rc)
{
    return FS_STATUS_MAP(iso9660_rows, rc);
}

/* ---- blob access ----
 *
 * The round trip through void * is what keeps -Wcast-align quiet about a cast
 * that is in fact aligned: the blob is a union with a uint64_t member, so it
 * carries the alignment every driver context needs, and the compiler cannot
 * see that through the unsigned char array alone. */

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
