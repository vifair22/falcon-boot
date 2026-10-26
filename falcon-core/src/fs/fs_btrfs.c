/* The btrfs adapter of the filesystem registry (DESIGN Section 9): the
 * btrfs driver dressed in fs_ops. One file per driver so a build links only
 * the drivers its registry table names; fs.c says why the adapters are thin.
 */

#include <fs/fs.h>
#include <fs/fs_adapter.h>

#include <fs/btrfs.h>
#include <lib/string.h>

/* The blobs must hold the driver's context (fs.c has the reason). */
_Static_assert(sizeof(btrfs_vol) <= FS_VOL_SIZE, "btrfs_vol exceeds FS_VOL_SIZE");
_Static_assert(sizeof(btrfs_dir) <= FS_DIR_SIZE, "btrfs_dir exceeds FS_DIR_SIZE");
_Static_assert(sizeof(btrfs_file) <= FS_FILE_SIZE, "btrfs_file exceeds FS_FILE_SIZE");

/* ---- status mapping ---- */

static const fs_status_row btrfs_rows[] = {
    { BTRFS_ERR_IO,          FS_ERR_IO },
    { BTRFS_ERR_NOT_BTRFS,   FS_ERR_BAD_SUPER },
    { BTRFS_ERR_CORRUPT,     FS_ERR_CORRUPT },
    { BTRFS_ERR_BAD_ARG,     FS_ERR_BAD_ARG },
    { BTRFS_ERR_RANGE,       FS_ERR_RANGE },
    { BTRFS_ERR_UNSUPPORTED, FS_ERR_UNSUPPORTED },
    { BTRFS_ERR_CSUM,        FS_ERR_CSUM },
    { BTRFS_ERR_NOT_FOUND,   FS_ERR_NOT_FOUND }
};

int fs_status_from_btrfs(int rc)
{
    return FS_STATUS_MAP(btrfs_rows, rc);
}

/* ---- blob access (fs_fat.c has why the casts go through void *) ---- */

static btrfs_vol *bvol(fs_vol *v) { return (btrfs_vol *)(void *)v->priv.b; }
static btrfs_dir *bdir(fs_dir *d) { return (btrfs_dir *)(void *)d->priv.b; }
static btrfs_file *bfile(fs_file *f) { return (btrfs_file *)(void *)f->priv.b; }
static const btrfs_file *bfile_c(const fs_file *f)
{
    return (const btrfs_file *)(const void *)f->priv.b;
}

/* ---- the btrfs adapter ---- */

/* What a btrfs dirent needs back: its inode, in the default subvolume's
 * tree. */
typedef struct {
    uint64_t ino;
} btrfs_priv;

_Static_assert(sizeof(btrfs_priv) <= FS_DIRENT_PRIV_SIZE,
               "btrfs dirent private part exceeds FS_DIRENT_PRIV_SIZE");

static void btrfs_to_fs_dirent(const btrfs_dirent *in, fs_dirent *out)
{
    btrfs_priv p;

    memset(out, 0, sizeof(*out));
    out->name_len =
        (uint16_t)fs_name_from_bytes(in->name, in->name_len, out->name,
                                     FS_NAME_MAX + 1u);
    /* A btrfs directory entry carries no size; the inode does. */
    out->size = 0;
    out->is_dir = in->is_dir;
    out->is_regular = in->is_regular;

    p.ino = in->ino;
    memcpy(out->priv.b, &p, sizeof p);
}

static void fs_to_btrfs_dirent(const fs_dirent *in, btrfs_dirent *out)
{
    btrfs_priv p;

    memcpy(&p, in->priv.b, sizeof p);
    memset(out, 0, sizeof(*out));
    out->is_dir = in->is_dir;
    out->is_regular = in->is_regular;
    out->ino = p.ino;
}

static int btrfs_probe_op(fs_read_fn read, void *ctx)
{
    return fs_status_from_btrfs(btrfs_probe(read, ctx));
}

static int btrfs_mount_op(fs_vol *v, fs_read_fn read, void *ctx)
{
    return fs_status_from_btrfs(btrfs_mount(bvol(v), read, ctx));
}

/* Nothing to replay and no memory to ask for (btrfs.h): what there is to
 * say is a log tree of fsyncs since the last commit, which was not read,
 * and that is what FS_JNL_FC_SKIPPED says for ext4. */
static int btrfs_mount_journal_op(fs_vol *v, fs_read_fn read, void *ctx,
                                  const fs_jmem *mem, fs_jinfo *info)
{
    int rc = btrfs_mount(bvol(v), read, ctx);

    (void)mem;
    info->need = 0;
    info->flags = 0;
    if (rc == BTRFS_OK && (bvol(v)->flags & BTRFS_LOG_SKIPPED) != 0)
        info->flags |= FS_JNL_FC_SKIPPED;
    return fs_status_from_btrfs(rc);
}

static int btrfs_dir_open_root_op(fs_vol *v, fs_dir *d)
{
    return fs_status_from_btrfs(btrfs_dir_open_root(bvol(v), bdir(d)));
}

static int btrfs_dir_open_op(fs_vol *v, fs_dir *d, const fs_dirent *e)
{
    btrfs_dirent de;

    fs_to_btrfs_dirent(e, &de);
    return fs_status_from_btrfs(btrfs_dir_open(bvol(v), bdir(d), &de));
}

static int btrfs_dir_next_op(fs_dir *d, fs_dirent *out)
{
    btrfs_dirent de;
    int rc = btrfs_dir_next(bdir(d), &de);

    if (rc == 1)
        btrfs_to_fs_dirent(&de, out);
    return fs_status_from_btrfs(rc);
}

static int btrfs_file_open_op(fs_vol *v, fs_file *f, const fs_dirent *e)
{
    btrfs_dirent de;

    fs_to_btrfs_dirent(e, &de);
    return fs_status_from_btrfs(btrfs_file_open(bvol(v), bfile(f), &de));
}

static int btrfs_file_read_op(fs_file *f, uint64_t off, uint64_t len,
                              void *buf)
{
    return fs_status_from_btrfs(btrfs_file_read(bfile(f), off, len, buf));
}

static uint64_t btrfs_file_size_op(const fs_file *f)
{
    return bfile_c(f)->size;
}

const fs_ops fs_ops_btrfs = {
    "btrfs",
    FS_HINT_SHELF,
    btrfs_probe_op,
    btrfs_mount_op,
    btrfs_dir_open_root_op,
    btrfs_dir_open_op,
    btrfs_dir_next_op,
    btrfs_file_open_op,
    btrfs_file_read_op,
    btrfs_file_size_op,
    /* No extent map: btrfs is a shelf filesystem, and only p2 is asked for
     * one (DESIGN Section 9). */
    (int (*)(fs_vol *, const fs_dirent *, fs_extent *, size_t, size_t *))0,
    (const char *(*)(const fs_vol *))0,
    btrfs_mount_journal_op
};
