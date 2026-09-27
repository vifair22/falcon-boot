/* A 4096-byte-sector disk over a test image (kndev.h). */

#include "kndev.h"

#include <string.h>

int kn_strict_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    kn_strict *s = ctx;

    if (len == 0 || off % s->sector != 0 || len % s->sector != 0) {
        s->refused++;
        return -1;
    }
    s->calls++;
    return s->inner(s->inner_ctx, off, len, buf);
}

void kn_init(kn_dev *k, blk_read_fn inner, void *inner_ctx)
{
    memset(k, 0, sizeof *k);
    k->strict.inner = inner;
    k->strict.inner_ctx = inner_ctx;
    k->strict.sector = KN_SECTOR;
    k->rebl.read = kn_strict_read;
    k->rebl.ctx = &k->strict;
    k->rebl.sector_size = KN_SECTOR;
    k->rebl.bounce = k->bounce;
    k->rebl.bounce_len = sizeof k->bounce;
}
