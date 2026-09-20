/* bootcfg's callbacks over the filesystem registry. See
 * falcon-core/src/media/bootfs.h. */

#include <media/bootfs.h>

#include <lib/string.h>

int bootfs_open(bootfs_ctx *ctx, const char *path, fs_file *f,
                uint64_t *size)
{
    int rc;

    if (ctx == (bootfs_ctx *)0 || path == (const char *)0 ||
        f == (fs_file *)0 || size == (uint64_t *)0)
        return FS_ERR_BAD_ARG;
    rc = fs_lookup(ctx->vol, path, &ctx->walk, &ctx->ent);
    if (rc != FS_OK)
        return rc;
    if (ctx->ent.is_dir || !ctx->ent.is_regular)
        return FS_ERR_NOT_FOUND;
    rc = fs_file_open(ctx->vol, f, &ctx->ent);
    if (rc != FS_OK)
        return rc;
    *size = fs_file_size(f);
    return FS_OK;
}

static int src_read(void *vctx, const char *path, char *buf, size_t cap,
                    size_t *len)
{
    bootfs_ctx *ctx = vctx;
    uint64_t size;

    if (bootfs_open(ctx, path, &ctx->file, &size) != FS_OK)
        return -1;
    if (size > (uint64_t)cap)
        return -1;
    if (size != 0 && fs_file_read(&ctx->file, 0, size, buf) != FS_OK)
        return -1;
    *len = (size_t)size;
    return 0;
}

/* A name bootcfg can compare: ASCII only. A file whose name is not has no
 * path a configuration could have spelled, so it is left out. */
static int name_ascii(const uint16_t *name, char *out, size_t cap)
{
    size_t i;

    for (i = 0; name[i] != 0; i++) {
        if (name[i] >= 0x80u || i + 1 >= cap)
            return 0;
        out[i] = (char)name[i];
    }
    out[i] = '\0';
    return 1;
}

static int src_list(void *vctx, const char *dir,
                    int (*cb)(void *arg, const char *name), void *arg)
{
    bootfs_ctx *ctx = vctx;
    char name[FS_NAME_MAX + 1];
    fs_dirent e;
    int rc;

    if (fs_lookup(ctx->vol, dir, &ctx->walk, &ctx->ent) != FS_OK ||
        !ctx->ent.is_dir)
        return -1;
    if (fs_dir_open(ctx->vol, &ctx->list, &ctx->ent) != FS_OK)
        return -1;
    while ((rc = fs_dir_next(&ctx->list, &e)) == 1) {
        if (e.is_dir || !e.is_regular || !name_ascii(e.name, name, sizeof name))
            continue;
        if (cb(arg, name) != 0)
            break;
    }
    return rc < 0 ? -1 : 0;
}

static int src_exists(void *vctx, const char *path)
{
    bootfs_ctx *ctx = vctx;

    return fs_lookup(ctx->vol, path, &ctx->walk, &ctx->ent) == FS_OK &&
           !ctx->ent.is_dir && ctx->ent.is_regular;
}

/* family_detect's exists: a directory counts, unlike bootcfg's, because a
 * layout fingerprint is usually a directory ("/casper", "/live"). */
static int probe_exists(void *vctx, const char *path)
{
    bootfs_ctx *ctx = vctx;

    return fs_lookup(ctx->vol, path, &ctx->walk, &ctx->ent) == FS_OK;
}

static int probe_read(void *vctx, const char *path, uint64_t off, void *buf,
                      size_t len, size_t *got)
{
    bootfs_ctx *ctx = vctx;
    uint64_t size;

    *got = 0;
    if (bootfs_open(ctx, path, &ctx->file, &size) != FS_OK)
        return -1;
    if (off >= size)
        return 0;
    if (size - off < (uint64_t)len)
        len = (size_t)(size - off);
    if (len != 0 && fs_file_read(&ctx->file, off, len, buf) != FS_OK)
        return -1;
    *got = len;
    return 0;
}

void bootfs_probe(bootfs_ctx *ctx, fs_vol *vol, family_probe *probe)
{
    ctx->vol = vol;
    probe->exists = probe_exists;
    probe->read = probe_read;
    probe->ctx = ctx;
}

void bootfs_src(bootfs_ctx *ctx, fs_vol *vol, bootcfg_src *src)
{
    ctx->vol = vol;
    src->read = src_read;
    src->list = src_list;
    src->exists = src_exists;
    src->ctx = ctx;
}
