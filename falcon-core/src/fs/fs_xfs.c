/* The xfs adapter of the filesystem registry (DESIGN Section 9): the xfs
 * driver dressed in fs_ops. One file per driver so a build links only the
 * drivers its registry table names; fs.c says why the adapters are thin.
 */

#include <fs/fs.h>
#include <fs/fs_adapter.h>

#include <fs/xfs.h>
#include <lib/string.h>

/* The blobs must hold the driver's context (fs.c has the reason). */
_Static_assert(sizeof(xfs_vol) <= FS_VOL_SIZE, "xfs_vol exceeds FS_VOL_SIZE");
_Static_assert(sizeof(xfs_dir) <= FS_DIR_SIZE, "xfs_dir exceeds FS_DIR_SIZE");
_Static_assert(sizeof(xfs_file) <= FS_FILE_SIZE, "xfs_file exceeds FS_FILE_SIZE");

/* ---- status mapping ---- */

static const fs_status_row xfs_rows[] = {
    { XFS_ERR_IO,             FS_ERR_IO },
    { XFS_ERR_NOT_XFS,        FS_ERR_BAD_SUPER },
    { XFS_ERR_CORRUPT,        FS_ERR_CORRUPT },
    { XFS_ERR_BAD_ARG,        FS_ERR_BAD_ARG },
    { XFS_ERR_RANGE,          FS_ERR_RANGE },
    { XFS_ERR_UNSUPPORTED,    FS_ERR_UNSUPPORTED },
    { XFS_ERR_CSUM,           FS_ERR_CSUM },
    { XFS_ERR_NEEDS_RECOVERY, FS_ERR_NEEDS_RECOVERY }
};

int fs_status_from_xfs(int rc)
{
    return FS_STATUS_MAP(xfs_rows, rc);
}

/* ---- blob access (fs_fat.c has why the casts go through void *) ---- */

static xfs_vol *xvol(fs_vol *v) { return (xfs_vol *)(void *)v->priv.b; }
static xfs_dir *xdir(fs_dir *d) { return (xfs_dir *)(void *)d->priv.b; }
static xfs_file *xfile(fs_file *f) { return (xfs_file *)(void *)f->priv.b; }
static const xfs_file *xfile_c(const fs_file *f)
{
    return (const xfs_file *)(const void *)f->priv.b;
}

/* ---- the xfs adapter ---- */

typedef struct {
    uint64_t ino;
} xfs_priv;

_Static_assert(sizeof(xfs_priv) <= FS_DIRENT_PRIV_SIZE,
               "xfs dirent private part exceeds FS_DIRENT_PRIV_SIZE");

static void xfs_to_fs_dirent(const xfs_dirent *in, fs_dirent *out)
{
    xfs_priv p;

    memset(out, 0, sizeof(*out));
    out->name_len =
        (uint16_t)fs_name_from_bytes(in->name, in->name_len, out->name,
                                     FS_NAME_MAX + 1u);
    /* An xfs directory entry carries no size; the inode does. */
    out->size = 0;
    out->is_dir = in->is_dir;
    out->is_regular = in->is_regular;

    p.ino = in->ino;
    memcpy(out->priv.b, &p, sizeof p);
}

static void fs_to_xfs_dirent(const fs_dirent *in, xfs_dirent *out)
{
    xfs_priv p;

    memcpy(&p, in->priv.b, sizeof p);
    memset(out, 0, sizeof(*out));
    out->is_dir = in->is_dir;
    out->is_regular = in->is_regular;
    out->ino = p.ino;
}

static int xfs_probe_op(fs_read_fn read, void *ctx)
{
    return fs_status_from_xfs(xfs_probe(read, ctx));
}

static int xfs_mount_op(fs_vol *v, fs_read_fn read, void *ctx)
{
    return fs_status_from_xfs(xfs_mount(xvol(v), read, ctx));
}

static int xfs_dir_open_root_op(fs_vol *v, fs_dir *d)
{
    return fs_status_from_xfs(xfs_dir_open_root(xvol(v), xdir(d)));
}

static int xfs_dir_open_op(fs_vol *v, fs_dir *d, const fs_dirent *e)
{
    xfs_dirent de;

    fs_to_xfs_dirent(e, &de);
    return fs_status_from_xfs(xfs_dir_open(xvol(v), xdir(d), &de));
}

static int xfs_dir_next_op(fs_dir *d, fs_dirent *out)
{
    xfs_dirent de;
    int rc = xfs_dir_next(xdir(d), &de);

    if (rc == 1)
        xfs_to_fs_dirent(&de, out);
    return fs_status_from_xfs(rc);
}

static int xfs_file_open_op(fs_vol *v, fs_file *f, const fs_dirent *e)
{
    xfs_dirent de;

    fs_to_xfs_dirent(e, &de);
    return fs_status_from_xfs(xfs_file_open(xvol(v), xfile(f), &de));
}

static int xfs_file_read_op(fs_file *f, uint64_t off, uint64_t len, void *buf)
{
    return fs_status_from_xfs(xfs_file_read(xfile(f), off, len, buf));
}

static uint64_t xfs_file_size_op(const fs_file *f)
{
    return xfile_c(f)->inode.size;
}

const fs_ops fs_ops_xfs = {
    "xfs",
    FS_HINT_SHELF,
    xfs_probe_op,
    xfs_mount_op,
    xfs_dir_open_root_op,
    xfs_dir_open_op,
    xfs_dir_next_op,
    xfs_file_open_op,
    xfs_file_read_op,
    xfs_file_size_op,
    /* No extent map: xfs is a shelf filesystem, and only p2 is asked for
     * one (DESIGN Section 9). */
    (int (*)(fs_vol *, const fs_dirent *, fs_extent *, size_t, size_t *))0,
    (const char *(*)(const fs_vol *))0,
    /* A log that needs recovery is refused by mount. */
    (int (*)(fs_vol *, fs_read_fn, void *, const struct fs_jmem_s *,
             struct fs_jinfo_s *))0
};
