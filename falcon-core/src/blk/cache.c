/* falcon-boot: a read cache of aligned windows. See cache.h. */

#include <blk/cache.h>

#include <lib/string.h>

void blk_cache_init(blk_cache *c, blk_cache_slot *slots, unsigned n,
                    uint8_t *mem, uint32_t win)
{
    unsigned i;

    c->slots = slots;
    c->n = (slots == (blk_cache_slot *)0 || mem == (uint8_t *)0 || win == 0)
               ? 0u : n;
    c->win = win;
    c->stamp = 0;
    for (i = 0; i < c->n; i++) {
        memset(&slots[i], 0, sizeof slots[i]);
        slots[i].data = mem + (size_t)i * win;
    }
}

void blk_cache_flush(blk_cache *c)
{
    unsigned i;

    for (i = 0; i < c->n; i++)
        c->slots[i].dev = (const void *)0;
}

/* The slot holding the window at woff, filled from the device on a miss.
 * NULL when the read fails. */
static blk_cache_slot *window(blk_cache *c, const void *dev, uint32_t gen,
                              uint64_t dev_len, blk_read_fn read, void *ctx,
                              uint64_t woff)
{
    blk_cache_slot *victim = &c->slots[0];
    uint64_t len;
    unsigned i;

    c->stamp++;
    for (i = 0; i < c->n; i++) {
        blk_cache_slot *s = &c->slots[i];

        if (s->dev == dev && s->gen == gen && s->off == woff) {
            s->used = c->stamp;
            return s;
        }
        if (s->dev == (const void *)0 ||
            (victim->dev != (const void *)0 && s->used < victim->used))
            victim = s;
    }
    len = dev_len - woff;
    if (len > c->win)
        len = c->win;
    victim->dev = (const void *)0;
    if (read(ctx, woff, len, victim->data) != 0)
        return (blk_cache_slot *)0;
    victim->dev = dev;
    victim->gen = gen;
    victim->off = woff;
    victim->len = (uint32_t)len;
    victim->used = c->stamp;
    return victim;
}

int blk_cache_read(blk_cache *c, const void *dev, uint32_t gen,
                   uint64_t dev_len, blk_read_fn read, void *ctx,
                   uint64_t off, uint64_t len, void *buf)
{
    uint8_t *out = buf;

    if (len == 0)
        return 0;
    if (c == (blk_cache *)0 || c->n == 0 || dev == (const void *)0 ||
        len >= c->win || off > dev_len || len > dev_len - off)
        return read(ctx, off, len, buf);
    while (len > 0) {
        uint64_t woff = off - off % c->win;
        uint64_t skip = off - woff;
        blk_cache_slot *s = window(c, dev, gen, dev_len, read, ctx, woff);
        uint64_t take;

        if (s == (blk_cache_slot *)0)
            return -1;
        take = s->len - skip;
        if (take > len)
            take = len;
        memcpy(out, s->data + skip, (size_t)take);
        out += take;
        off += take;
        len -= take;
    }
    return 0;
}
