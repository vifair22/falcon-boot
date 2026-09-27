/* The pure half of winjump's driver-mount fallback. See winjumpcore.h. */

#include "winjumpcore.h"

#include <lib/string.h>

#include <media/extfind.h>

/* The byte reader extfind wants, over a reader that takes whole sectors:
 * aligned requests go straight through, anything else is read whole into
 * the bounce buffer and the asked-for bytes copied out. */
typedef struct {
    wj_sector_read read;
    void          *ctx;
    uint32_t       sector;
    uint8_t       *bounce;
} wj_bytes;

static int bytes_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const wj_bytes *b = ctx;
    uint8_t *out = buf;

    if ((off % b->sector) == 0 && (len % b->sector) == 0) {
        while (len > 0) {
            uint32_t n = len > WJ_BOUNCE ? WJ_BOUNCE : (uint32_t)len;

            if (b->read(b->ctx, off, n, out) != 0)
                return -1;
            off += n;
            out += n;
            len -= n;
        }
        return 0;
    }
    while (len > 0) {
        uint64_t at = off - off % b->sector;
        uint32_t skip = (uint32_t)(off - at);
        uint32_t n = b->sector - skip;

        if (n > len)
            n = (uint32_t)len;
        if (b->read(b->ctx, at, b->sector, b->bounce) != 0)
            return -1;
        memcpy(out, b->bounce + skip, n);
        off += n;
        out += n;
        len -= n;
    }
    return 0;
}

int wj_disk_holds(const extmap_hdr *hdr, wj_sector_read read, void *ctx,
                  wj_scratch *s)
{
    wj_bytes b;

    if (hdr == (const extmap_hdr *)0 || read == (wj_sector_read)0 ||
        s == (wj_scratch *)0 || hdr->block_size == 0 ||
        hdr->block_size > WJ_BOUNCE)
        return WJ_ERR_BAD_ARG;
    b.read = read;
    b.ctx = ctx;
    b.sector = hdr->block_size;
    b.bounce = s->bounce;
    return extmap_find_on_disk(hdr, bytes_read, &b, 0, &s->rec) == EXTMAP_OK
               ? WJ_OK
               : WJ_ERR_NOMATCH;
}

int wj_backing_extents(const extmap_hdr *hdr, const uint8_t *map, size_t map_len,
                       wj_extent *out, uint32_t cap, uint32_t *n)
{
    uint64_t part_base, iso_off = 0;
    uint32_t i;

    if (hdr == (const extmap_hdr *)0 || map == (const uint8_t *)0 ||
        out == (wj_extent *)0 || n == (uint32_t *)0)
        return WJ_ERR_BAD_ARG;
    if (map_len < extmap_size(hdr->n_extents))
        return WJ_ERR_FORMAT;
    if (hdr->n_extents > cap)
        return WJ_ERR_NOSPACE;

    part_base = hdr->part_lba * hdr->block_size;
    for (i = 0; i < hdr->n_extents; i++) {
        extmap_extent e;

        extmap_extent_at(map, i, &e);
        out[i].iso_offset = iso_off;
        out[i].disk_offset = part_base + e.offset;
        out[i].length = e.length;
        iso_off += e.length;
    }
    *n = hdr->n_extents;
    return WJ_OK;
}
