/* The UDF adapter of the filesystem registry (DESIGN Section 9): the
 * UDF driver dressed in fs_ops. One file per driver so a build links only
 * the drivers its registry table names (fs_reg_all.c, fs_reg_ext4.c); fs.c
 * says why the adapters are thin.
 */

#include <fs/fs.h>
#include <fs/fs_adapter.h>

#include <fs/udf.h>
#include <lib/string.h>

/* The blobs must hold the driver's context (fs.c has the reason). */
_Static_assert(sizeof(udf_vol) <= FS_VOL_SIZE, "udf_vol exceeds FS_VOL_SIZE");
_Static_assert(sizeof(udf_dir) <= FS_DIR_SIZE, "udf_dir exceeds FS_DIR_SIZE");
_Static_assert(sizeof(udf_file) <= FS_FILE_SIZE, "udf_file exceeds FS_FILE_SIZE");

/* Shared rather than converted; fs.c has the reason. */
_Static_assert(sizeof(fs_extent) == sizeof(udf_extent),
               "fs_extent and udf_extent must stay layout-compatible");

/* ---- status mapping ---- */

static const fs_status_row udf_rows[] = {
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

int fs_status_from_udf(int rc)
{
    return FS_STATUS_MAP(udf_rows, rc);
}

/* ---- blob access ----
 *
 * The round trip through void * is what keeps -Wcast-align quiet about a cast
 * that is in fact aligned: the blob is a union with a uint64_t member, so it
 * carries the alignment every driver context needs, and the compiler cannot
 * see that through the unsigned char array alone. */

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
