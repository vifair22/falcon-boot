/* falcon-boot: a read cache of aligned windows under the read callback
 * (docs/DESIGN.md Section 3.4).
 *
 * Some firmware charges for a read call, not for its bytes: an ASUS B550's
 * AMI USB driver takes about 70 ms for any read up to 64 KiB of a Kingston
 * stick. The drivers above the callback read what their formats lay out, a
 * few KiB at a time (a superblock, a directory block, an inode table block,
 * an ISO's volume descriptors), and those small reads are most of the calls
 * a boot makes before the UKI. A read smaller than a window is therefore
 * served from a window-aligned copy of the device, and a miss reads the
 * whole window in one call; a read of a window or more goes straight
 * through, so a UKI's large reads are neither slowed nor cached.
 *
 * A window costs its fill on whatever reads it: a read that misses waits
 * for the whole window, and a loader that lets a key through only between
 * reads waits that long too. The window is therefore sized per device from
 * what its fills cost, when the caller gives the cache a clock: each full
 * fill's time scales the device's next window toward a budget (the caller's,
 * 100 ms on the UEFI loader), a power of two from BLK_CACHE_MIN_WIN up to
 * the cache's window. A firmware that charges per call reads a full window
 * well inside the budget and keeps it; a slow medium shrinks it until a fill
 * fits. A device starts at the cache's window, so its first fill is what
 * measures it.
 *
 * A window is keyed on the device (an opaque pointer) and its generation (a
 * number the caller changes when the medium may have, a Block I/O MediaId),
 * so a medium swapped under the same pointer misses. The caller flushes
 * everything when it re-probes its devices, which is the other way a pointer
 * comes to mean something else. Nothing here writes a device, and the
 * loaders write none they read, so a window never needs updating.
 *
 * No allocation: the slots and their memory are the caller's.
 */

#ifndef FALCON_BLK_CACHE_H
#define FALCON_BLK_CACHE_H

#include <stdint.h>

#include <blk/rebl.h>

typedef struct {
    const void *dev;      /* NULL: the slot is empty */
    uint32_t    gen;
    uint32_t    len;      /* bytes held: the window, less at the device's end */
    uint64_t    off;      /* the window's first byte on the device */
                          /* (windows of one device may differ in len) */
    uint32_t    used;     /* stamp of the last use, for eviction */
    uint8_t    *data;     /* window bytes of the caller's memory */
} blk_cache_slot;

/* The smallest window a device is shrunk to. */
#define BLK_CACHE_MIN_WIN 4096u

/* The devices whose window the cache remembers; past that the least
 * recently filled one is forgotten and starts again at the cache's window. */
#define BLK_CACHE_DEVS 8u

typedef struct {
    const void *dev;      /* NULL: the entry is empty */
    uint32_t    gen;
    uint32_t    win;      /* this device's window */
    uint32_t    used;
} blk_cache_dev;

typedef struct {
    blk_cache_slot *slots;
    unsigned        n;
    uint32_t        win;
    uint32_t        stamp;
    uint64_t      (*clock)(void);
    uint64_t        budget;   /* clock ticks a fill may take; 0: fixed */
    blk_cache_dev   devs[BLK_CACHE_DEVS];
} blk_cache;

/* n slots, each given win bytes of mem (n * win in all). win is a multiple
 * of every sector size the inner reads will be asked for. n of 0 makes a
 * cache that passes every read through. */
void blk_cache_init(blk_cache *c, blk_cache_slot *slots, unsigned n,
                    uint8_t *mem, uint32_t win);

/* Sizes each device's window from its fills: clock() before and after each
 * one, and budget, in the clock's ticks, what one may take. A NULL clock or
 * a budget of 0 keeps every window at the cache's (the state after init). */
void blk_cache_set_clock(blk_cache *c, uint64_t (*clock)(void),
                         uint64_t budget);

/* The window that follows a fill of len bytes which took ticks: len scaled
 * by budget / ticks, rounded down to a power of two, held within
 * BLK_CACHE_MIN_WIN and max (max itself when it is smaller). A fill that
 * took no ticks gives max. */
uint32_t blk_cache_next_win(uint32_t max, uint32_t len, uint64_t ticks,
                            uint64_t budget);

/* Forgets every window, and every device's size. */
void blk_cache_flush(blk_cache *c);

/* len bytes at off of the device dev (generation gen, dev_len bytes long)
 * into buf, through read(ctx, ...), which takes any length at any offset
 * (blk_read_fn). Through the windows when len is under the device's window
 * and the range lies inside the device; otherwise one
 * read(ctx, off, len, buf).
 * 0 on success; on failure the caller's buffer may hold part of the data
 * and the window that failed is left empty. */
int blk_cache_read(blk_cache *c, const void *dev, uint32_t gen,
                   uint64_t dev_len, blk_read_fn read, void *ctx,
                   uint64_t off, uint64_t len, void *buf);

#endif /* FALCON_BLK_CACHE_H */
