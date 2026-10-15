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
    uint32_t    used;     /* stamp of the last use, for eviction */
    uint8_t    *data;     /* window bytes of the caller's memory */
} blk_cache_slot;

typedef struct {
    blk_cache_slot *slots;
    unsigned        n;
    uint32_t        win;
    uint32_t        stamp;
} blk_cache;

/* n slots, each given win bytes of mem (n * win in all). win is a multiple
 * of every sector size the inner reads will be asked for. n of 0 makes a
 * cache that passes every read through. */
void blk_cache_init(blk_cache *c, blk_cache_slot *slots, unsigned n,
                    uint8_t *mem, uint32_t win);

/* Forgets every window. */
void blk_cache_flush(blk_cache *c);

/* len bytes at off of the device dev (generation gen, dev_len bytes long)
 * into buf, through read(ctx, ...), which takes any length at any offset
 * (blk_read_fn). Through the windows when len is under a window and the
 * range lies inside the device; otherwise one read(ctx, off, len, buf).
 * 0 on success; on failure the caller's buffer may hold part of the data
 * and the window that failed is left empty. */
int blk_cache_read(blk_cache *c, const void *dev, uint32_t gen,
                   uint64_t dev_len, blk_read_fn read, void *ctx,
                   uint64_t off, uint64_t len, void *buf);

#endif /* FALCON_BLK_CACHE_H */
