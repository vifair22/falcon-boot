/* falcon-boot: re-blocking under the read callback (blk/rebl.h). */

#include <blk/rebl.h>

#include <stddef.h>

#include <lib/string.h>

/* One sector through the bounce buffer: the sector starting at `sector`,
 * `take` bytes of it from `skip` on, to `out`. */
static int bounce_part(const blk_rebl *r, uint64_t sector, uint32_t skip,
                       uint32_t take, uint8_t *out)
{
    if (r->bounce == (uint8_t *)0 || r->bounce_len < r->sector_size)
        return -1;
    if (r->read(r->ctx, sector, r->sector_size, r->bounce) != 0)
        return -1;
    memcpy(out, r->bounce + skip, take);
    return 0;
}

int blk_rebl_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const blk_rebl *r = ctx;
    uint8_t *out = buf;
    uint32_t ss;
    uint32_t head;
    uint64_t whole;

    if (r == (const blk_rebl *)0 || r->read == (blk_read_fn)0 ||
        r->sector_size == 0)
        return -1;
    if (len == 0)
        return 0;
    if (out == (uint8_t *)0 || off > ~(uint64_t)0 - len)
        return -1;
    ss = r->sector_size;

    /* A head that starts inside a sector, or a request shorter than one:
     * that sector, whole, through the bounce. */
    head = (uint32_t)(off % ss);
    if (head != 0 || len < ss) {
        uint32_t take = ss - head;

        if (take > len)
            take = (uint32_t)len;
        if (bounce_part(r, off - head, head, take, out) != 0)
            return -1;
        out += take;
        off += take;
        len -= take;
    }

    /* The middle: sector-aligned by now, and read straight into the
     * caller's buffer in one call. For a request that was whole sectors to
     * begin with this is the only call, with the caller's own arguments. */
    whole = len - len % ss;
    if (whole != 0) {
        if (r->read(r->ctx, off, whole, out) != 0)
            return -1;
        out += whole;
        off += whole;
        len -= whole;
    }

    /* A tail shorter than a sector. */
    if (len != 0 && bounce_part(r, off, 0, (uint32_t)len, out) != 0)
        return -1;
    return 0;
}
