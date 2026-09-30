/* The ext4 adapter of the filesystem registry (DESIGN Section 9): the
 * ext4 driver dressed in fs_ops. One file per driver so a build links only
 * the drivers its registry table names (fs_reg_all.c, fs_reg_ext4.c); fs.c
 * says why the adapters are thin.
 */

#include <fs/fs.h>
#include <fs/fs_adapter.h>

#include <fs/ext4.h>
#include <lib/string.h>

/* The blobs must hold the driver's context (fs.c has the reason). */
_Static_assert(sizeof(ext4_vol) <= FS_VOL_SIZE, "ext4_vol exceeds FS_VOL_SIZE");
_Static_assert(sizeof(ext4_dir) <= FS_DIR_SIZE, "ext4_dir exceeds FS_DIR_SIZE");
_Static_assert(sizeof(ext4_file) <= FS_FILE_SIZE, "ext4_file exceeds FS_FILE_SIZE");

/* ---- status mapping ---- */

static const fs_status_row ext4_rows[] = {
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
    { EXT4_ERR_HOLE,           FS_ERR_HOLE },
    { EXT4_ERR_JOURNAL,        FS_ERR_JOURNAL },
    { EXT4_ERR_NOMEM,          FS_ERR_NOMEM }
};

int fs_status_from_ext4(int rc)
{
    return FS_STATUS_MAP(ext4_rows, rc);
}

/* ---- blob access ----
 *
 * The round trip through void * is what keeps -Wcast-align quiet about a cast
 * that is in fact aligned: the blob is a union with a uint64_t member, so it
 * carries the alignment every driver context needs, and the compiler cannot
 * see that through the unsigned char array alone. */

static ext4_vol *evol(fs_vol *v) { return (ext4_vol *)(void *)v->priv.b; }
static ext4_dir *edir(fs_dir *d) { return (ext4_dir *)(void *)d->priv.b; }
static ext4_file *efile(fs_file *f) { return (ext4_file *)(void *)f->priv.b; }
static const ext4_file *efile_c(const fs_file *f)
{
    return (const ext4_file *)(const void *)f->priv.b;
}

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

/* fs_jmem and jbd2_mem are the same contract spelled twice, so that fs.h
 * names no driver's header; the fields are copied rather than the pointer
 * cast. */
static int ext4_mount_journal_op(fs_vol *v, fs_read_fn read, void *ctx,
                                 const fs_jmem *mem, fs_jinfo *info)
{
    jbd2_mem m = { 0, 0, 0 };
    ext4_jinfo ji;
    int rc;

    if (mem != (const fs_jmem *)0) {
        m.get = mem->get;
        m.put = mem->put;
        m.ctx = mem->ctx;
    }
    rc = ext4_mount_journal(evol(v), read, ctx,
                            mem != (const fs_jmem *)0 ? &m
                                                      : (const jbd2_mem *)0,
                            &ji);
    info->need = ji.need;
    info->flags = 0;
    if ((ji.flags & EXT4_JNL_REPLAYED) != 0)
        info->flags |= FS_JNL_REPLAYED;
    if ((ji.flags & EXT4_JNL_FC_SKIPPED) != 0)
        info->flags |= FS_JNL_FC_SKIPPED;
    return fs_status_from_ext4(rc);
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
    (const char *(*)(const fs_vol *))0,
    ext4_mount_journal_op
};
