/* The /.falcon reader. See identfs.h. */

#include <ident/identfs.h>

int ident_read_vol(fs_vol *v, ident_fs_scratch *s, ident_rec *out)
{
    char text[IDENT_TEXT_MAX];
    uint64_t size;
    int rc;

    if (v == (fs_vol *)0 || s == (ident_fs_scratch *)0 ||
        out == (ident_rec *)0 || v->ops == (const struct fs_ops *)0)
        return IDENT_ERR_ARG;
    rc = fs_lookup(v, IDENT_FILE, &s->dir, &s->ent);
    if (rc == FS_ERR_NOT_FOUND)
        return IDENT_ERR_NONE;
    if (rc != FS_OK)
        return IDENT_ERR_BAD;
    if (s->ent.is_dir || !s->ent.is_regular)
        return IDENT_ERR_NONE;
    /* The record is one short line; anything the size of a real file is
     * not one, and is refused before a byte of it is read. The terminator's
     * byte is kept back so the length check below is the decoder's. The
     * size is the open file's: an ext4 directory entry carries none, so a
     * plaintext shelf's record would otherwise read as empty. */
    if (fs_file_open(v, &s->file, &s->ent) != FS_OK)
        return IDENT_ERR_BAD;
    size = fs_file_size(&s->file);
    if (size == 0u || size >= (uint64_t)sizeof text)
        return IDENT_ERR_BAD;
    if (fs_file_read(&s->file, 0, size, text) != FS_OK)
        return IDENT_ERR_BAD;
    return ident_decode_text(text, (size_t)size, out);
}

int ident_probe_mem(fs_read_fn read, void *ctx, ident_fs_scratch *s,
                    const fs_jmem *mem, ident_rec *out)
{
    const fs_ops *ops;
    fs_jinfo info;
    int rc;

    if (read == (fs_read_fn)0 || s == (ident_fs_scratch *)0 ||
        out == (ident_rec *)0)
        return IDENT_ERR_ARG;
    s->vol.ops = (const struct fs_ops *)0;
    ops = fs_identify(read, ctx, FS_HINT_NONE);
    if (ops == (const fs_ops *)0)
        return IDENT_ERR_NONE;
    rc = (mem == (const fs_jmem *)0)
             ? fs_mount(&s->vol, ops, read, ctx)
             : fs_mount_journal(&s->vol, ops, read, ctx, mem, &info);
    if (rc != FS_OK)
        return IDENT_ERR_NONE;
    return ident_read_vol(&s->vol, s, out);
}

int ident_probe(fs_read_fn read, void *ctx, ident_fs_scratch *s,
                ident_rec *out)
{
    return ident_probe_mem(read, ctx, s, (const fs_jmem *)0, out);
}
