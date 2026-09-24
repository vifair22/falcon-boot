/* The FAT adapter of the filesystem registry (DESIGN Section 9): the
 * FAT driver dressed in fs_ops. One file per driver so a build links only
 * the drivers its registry table names (fs_reg_all.c, fs_reg_ext4.c); fs.c
 * says why the adapters are thin.
 */

#include <fs/fs.h>
#include <fs/fs_adapter.h>

#include <fs/fat.h>
#include <lib/string.h>

/* The blobs must hold the driver's context (fs.c has the reason). */
_Static_assert(sizeof(fat_vol) <= FS_VOL_SIZE, "fat_vol exceeds FS_VOL_SIZE");
_Static_assert(sizeof(fat_dir) <= FS_DIR_SIZE, "fat_dir exceeds FS_DIR_SIZE");
_Static_assert(sizeof(fat_file) <= FS_FILE_SIZE, "fat_file exceeds FS_FILE_SIZE");

/* ---- status mapping ---- */

static const fs_status_row fat_rows[] = {
    { FAT_ERR_IO,          FS_ERR_IO },
    { FAT_ERR_NOT_FAT,     FS_ERR_BAD_SUPER },
    { FAT_ERR_CORRUPT,     FS_ERR_CORRUPT },
    { FAT_ERR_BAD_ARG,     FS_ERR_BAD_ARG },
    { FAT_ERR_RANGE,       FS_ERR_RANGE },
    { FAT_ERR_NOSPACE,     FS_ERR_NOSPACE },
    { FAT_ERR_UNSUPPORTED, FS_ERR_UNSUPPORTED }
};

int fs_status_from_fat(int rc)
{
    return FS_STATUS_MAP(fat_rows, rc);
}

/* ---- blob access ----
 *
 * The round trip through void * is what keeps -Wcast-align quiet about a cast
 * that is in fact aligned: the blob is a union with a uint64_t member, so it
 * carries the alignment every driver context needs, and the compiler cannot
 * see that through the unsigned char array alone. */

static fat_vol *tvol(fs_vol *v) { return (fat_vol *)(void *)v->priv.b; }
static const fat_vol *tvol_c(const fs_vol *v)
{
    return (const fat_vol *)(const void *)v->priv.b;
}
static fat_dir *tdir(fs_dir *d) { return (fat_dir *)(void *)d->priv.b; }
static fat_file *tfile(fs_file *f) { return (fat_file *)(void *)f->priv.b; }
static const fat_file *tfile_c(const fs_file *f)
{
    return (const fat_file *)(const void *)f->priv.b;
}

/* ---- the FAT adapter ---- */

/* What a FAT dirent needs back: the first cluster, and the size, which the
 * unified dirent already carries as 64 bits. Nothing else: FAT has no inode,
 * no record position and no second name space. */
typedef struct {
    uint32_t cluster;
} fat_priv;

_Static_assert(sizeof(fat_priv) <= FS_DIRENT_PRIV_SIZE,
               "fat dirent private part exceeds FS_DIRENT_PRIV_SIZE");

_Static_assert(sizeof(fs_extent) == sizeof(fat_extent),
               "fs_extent and fat_extent must stay layout-compatible");

static void fat_to_fs_dirent(const fat_dirent *in, fs_dirent *out)
{
    fat_priv p;
    size_t i;

    memset(out, 0, sizeof(*out));
    for (i = 0; i < in->name_len && i < FS_NAME_MAX; i++)
        out->name[i] = in->name[i];
    out->name[i] = 0;
    out->name_len = (uint16_t)i;
    out->size = in->size;
    out->is_dir = in->is_dir;
    out->is_regular = in->is_regular;

    p.cluster = in->cluster;
    memcpy(out->priv.b, &p, sizeof p);
}

static void fs_to_fat_dirent(const fs_dirent *in, fat_dirent *out)
{
    fat_priv p;

    memcpy(&p, in->priv.b, sizeof p);
    memset(out, 0, sizeof(*out));
    out->is_dir = in->is_dir;
    out->is_regular = in->is_regular;
    /* FAT's size field is 32 bits and the driver never reports more, so the
     * narrowing here cannot lose anything a FAT volume could have said. */
    out->size = (uint32_t)in->size;
    out->cluster = p.cluster;
}

static int fat_probe_op(fs_read_fn read, void *ctx)
{
    return fs_status_from_fat(fat_probe(read, ctx));
}

static int fat_mount_op(fs_vol *v, fs_read_fn read, void *ctx)
{
    return fs_status_from_fat(fat_mount(tvol(v), read, ctx));
}

static int fat_dir_open_root_op(fs_vol *v, fs_dir *d)
{
    return fs_status_from_fat(fat_dir_open_root(tvol(v), tdir(d)));
}

static int fat_dir_open_op(fs_vol *v, fs_dir *d, const fs_dirent *e)
{
    fat_dirent de;

    fs_to_fat_dirent(e, &de);
    return fs_status_from_fat(fat_dir_open(tvol(v), tdir(d), &de));
}

static int fat_dir_next_op(fs_dir *d, fs_dirent *out)
{
    fat_dirent de;
    int rc = fat_dir_next(tdir(d), &de);

    if (rc == 1) {
        fat_to_fs_dirent(&de, out);
        return 1;
    }
    return fs_status_from_fat(rc);
}

static int fat_file_open_op(fs_vol *v, fs_file *f, const fs_dirent *e)
{
    fat_dirent de;

    fs_to_fat_dirent(e, &de);
    return fs_status_from_fat(fat_file_open(tvol(v), tfile(f), &de));
}

static int fat_file_read_op(fs_file *f, uint64_t off, uint64_t len, void *buf)
{
    return fs_status_from_fat(fat_file_read(tfile(f), off, len, buf));
}

static uint64_t fat_file_size_op(const fs_file *f)
{
    return tfile_c(f)->size;
}

static int fat_extents_op(fs_vol *v, const fs_dirent *e, fs_extent *out,
                          size_t cap, size_t *count)
{
    fat_dirent de;

    fs_to_fat_dirent(e, &de);
    return fs_status_from_fat(
        fat_file_extents(tvol(v), &de, (fat_extent *)(void *)out, cap, count));
}

static const char *fat_label_op(const fs_vol *v)
{
    return tvol_c(v)->label;
}

/* FS_HINT_NONE: FAT is not the primary answer for either falcon partition.
 * p1 is vfat but the loader never mounts it through the registry, and a FAT
 * volume reached through this driver is an El Torito boot image, which has no
 * role. It is found by probing, in table order, like iso9660 and udf. */
const fs_ops fs_ops_fat = {
    "fat",
    FS_HINT_NONE,
    fat_probe_op,
    fat_mount_op,
    fat_dir_open_root_op,
    fat_dir_open_op,
    fat_dir_next_op,
    fat_file_open_op,
    fat_file_read_op,
    fat_file_size_op,
    fat_extents_op,
    fat_label_op
};
