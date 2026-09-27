/* Finding the partition an extent map names. See extfind.h. */

#include <media/extfind.h>

#include <lib/string.h>

int extmap_names(const extmap_hdr *h, const ident_rec *r)
{
    if (h == (const extmap_hdr *)0 || r == (const ident_rec *)0)
        return 0;
    return r->role == IDENT_ROLE_LIBRARY &&
           memcmp(r->install_id, h->install_id, IDENT_ID_LEN) == 0;
}

/* The disk's reader shifted to the partition's first byte, which is what
 * every filesystem driver expects its volume to start at. */
typedef struct {
    fs_read_fn read;
    void      *ctx;
    uint64_t   base;
} part_view;

static int view_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const part_view *v = ctx;

    if (off > ~v->base)
        return -1;
    return v->read(v->ctx, v->base + off, len, buf);
}

int extmap_find_on_disk(const extmap_hdr *h, fs_read_fn read, void *ctx,
                        uint32_t block_size, ident_fs_scratch *s)
{
    part_view view;
    ident_rec rec;
    int rc;

    if (h == (const extmap_hdr *)0 || read == (fs_read_fn)0 ||
        s == (ident_fs_scratch *)0 || h->block_size == 0u)
        return EXTMAP_ERR_BAD_ARG;
    if (block_size != 0u && block_size != h->block_size)
        return EXTMAP_ERR_NOMATCH;
    /* part_lba * block_size, refused rather than wrapped: a map from the
     * loader never overflows, so one that does names nothing on any disk. */
    if (h->part_lba > ~(uint64_t)0 / h->block_size)
        return EXTMAP_ERR_NOMATCH;
    view.read = read;
    view.ctx = ctx;
    view.base = h->part_lba * h->block_size;
    rc = ident_probe(view_read, &view, s, &rec);
    /* The view dies with this frame, so the volume goes with it. */
    s->vol.ops = (const struct fs_ops *)0;
    if (rc != IDENT_OK || !extmap_names(h, &rec))
        return EXTMAP_ERR_NOMATCH;
    return EXTMAP_OK;
}
