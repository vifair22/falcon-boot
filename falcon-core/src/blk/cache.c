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
    c->clock = (uint64_t (*)(void))0;
    c->budget = 0;
    memset(c->devs, 0, sizeof c->devs);
    for (i = 0; i < c->n; i++) {
        memset(&slots[i], 0, sizeof slots[i]);
        slots[i].data = mem + (size_t)i * win;
    }
}

void blk_cache_set_clock(blk_cache *c, uint64_t (*clock)(void),
                         uint64_t budget)
{
    c->clock = clock;
    c->budget = (clock == (uint64_t (*)(void))0) ? 0u : budget;
}

uint32_t blk_cache_next_win(uint32_t max, uint32_t len, uint64_t ticks,
                            uint64_t budget)
{
    uint64_t want;
    uint32_t win = BLK_CACHE_MIN_WIN;

    if (max <= BLK_CACHE_MIN_WIN)
        return max;
    if (ticks == 0 || budget == 0)
        return max;
    /* len times a budget under 2^32 fits 64 bits; a larger budget is
     * divided first, losing only precision a window never needs. */
    want = (budget <= 0xFFFFFFFFu) ? (uint64_t)len * budget / ticks
                                   : (uint64_t)len * (budget / ticks);
    while (win < max && (uint64_t)win * 2u <= want)
        win *= 2u;
    return win;
}

void blk_cache_flush(blk_cache *c)
{
    unsigned i;

    for (i = 0; i < c->n; i++)
        c->slots[i].dev = (const void *)0;
    memset(c->devs, 0, sizeof c->devs);
}

/* The device's entry, made at the cache's window when it has none. */
static blk_cache_dev *dev_entry(blk_cache *c, const void *dev, uint32_t gen)
{
    blk_cache_dev *victim = &c->devs[0];
    unsigned i;

    for (i = 0; i < BLK_CACHE_DEVS; i++) {
        blk_cache_dev *e = &c->devs[i];

        if (e->dev == dev && e->gen == gen)
            return e;
        if (e->dev == (const void *)0 ||
            (victim->dev != (const void *)0 && e->used < victim->used))
            victim = e;
    }
    victim->dev = dev;
    victim->gen = gen;
    victim->win = c->win;
    victim->used = c->stamp;
    return victim;
}

/* The slot holding off of the device, filled on a miss with the window of
 * e->win bytes around it. NULL when the read fails. Windows of one device
 * can differ in size once its window has moved, so a hit is any slot whose
 * bytes cover off. */
static blk_cache_slot *window(blk_cache *c, blk_cache_dev *e, uint64_t dev_len,
                              blk_read_fn read, void *ctx, uint64_t off)
{
    blk_cache_slot *victim = &c->slots[0];
    uint64_t woff = off - off % e->win;
    uint64_t len;
    uint64_t t0 = 0;
    unsigned i;

    c->stamp++;
    e->used = c->stamp;
    for (i = 0; i < c->n; i++) {
        blk_cache_slot *s = &c->slots[i];

        if (s->dev == e->dev && s->gen == e->gen && off >= s->off &&
            off - s->off < s->len) {
            s->used = c->stamp;
            return s;
        }
        if (s->dev == (const void *)0 ||
            (victim->dev != (const void *)0 && s->used < victim->used))
            victim = s;
    }
    len = dev_len - woff;
    if (len > e->win)
        len = e->win;
    victim->dev = (const void *)0;
    if (c->budget != 0)
        t0 = c->clock();
    if (read(ctx, woff, len, victim->data) != 0)
        return (blk_cache_slot *)0;
    /* A short fill at the device's end says little about a full one. */
    if (c->budget != 0 && len == e->win)
        e->win = blk_cache_next_win(c->win, e->win, c->clock() - t0,
                                    c->budget);
    victim->dev = e->dev;
    victim->gen = e->gen;
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
    blk_cache_dev *e;

    if (len == 0)
        return 0;
    if (c == (blk_cache *)0 || c->n == 0 || dev == (const void *)0 ||
        len >= c->win || off > dev_len || len > dev_len - off)
        return read(ctx, off, len, buf);
    e = dev_entry(c, dev, gen);
    if (len >= e->win)
        return read(ctx, off, len, buf);
    while (len > 0) {
        blk_cache_slot *s = window(c, e, dev_len, read, ctx, off);
        uint64_t skip;
        uint64_t take;

        if (s == (blk_cache_slot *)0)
            return -1;
        skip = off - s->off;
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
