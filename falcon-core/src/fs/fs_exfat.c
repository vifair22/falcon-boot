/* The exFAT adapter of the filesystem registry (DESIGN Section 9): the
 * exFAT driver dressed in fs_ops. One file per driver so a build links only
 * the drivers its registry table names (fs_reg_all.c, fs_reg_ext4.c); fs.c
 * says why the adapters are thin.
 */

#include <fs/fs.h>
#include <fs/fs_adapter.h>

#include <fs/exfat.h>
#include <lib/string.h>

/* The blobs must hold the driver's context (fs.c has the reason). */
_Static_assert(sizeof(exfat_vol) <= FS_VOL_SIZE, "exfat_vol exceeds FS_VOL_SIZE");
_Static_assert(sizeof(exfat_dir) <= FS_DIR_SIZE, "exfat_dir exceeds FS_DIR_SIZE");
_Static_assert(sizeof(exfat_file) <= FS_FILE_SIZE, "exfat_file exceeds FS_FILE_SIZE");

/* Shared rather than converted; fs.c has the reason. */
_Static_assert(sizeof(fs_extent) == sizeof(exfat_extent),
               "fs_extent and exfat_extent must stay layout-compatible");

/* ---- status mapping ---- */

static const fs_status_row exfat_rows[] = {
    { EXFAT_ERR_IO,        FS_ERR_IO },
    { EXFAT_ERR_BAD_SUPER, FS_ERR_BAD_SUPER },
    { EXFAT_ERR_CORRUPT,   FS_ERR_CORRUPT },
    { EXFAT_ERR_BAD_ARG,   FS_ERR_BAD_ARG },
    { EXFAT_ERR_RANGE,     FS_ERR_RANGE },
    { EXFAT_ERR_NOSPACE,   FS_ERR_NOSPACE }
};

int fs_status_from_exfat(int rc)
{
    return FS_STATUS_MAP(exfat_rows, rc);
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
    (const char *(*)(const fs_vol *))0,
    /* No journal to replay. */
    (int (*)(fs_vol *, fs_read_fn, void *, const struct fs_jmem_s *,
             struct fs_jinfo_s *))0
};
