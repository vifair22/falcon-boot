/* falcon-boot: re-blocking under the read callback (docs/DESIGN.md
 * Section 3.4).
 *
 * A platform's read callback moves whole device sectors: Block I/O takes a
 * block count, INT 13h a sector count. The drivers above it ask for what the
 * on-disk format lays out, which is not a sector on every disk: a LUKS2
 * keyslot area is streamed in 512-byte units, the ext4 superblock is 1024
 * bytes at 1024, an exFAT or FAT boot sector is read as its first 512 bytes.
 * On a 512-byte-sector disk those are whole sectors by coincidence; on a
 * 4096-byte-sector (4Kn) disk none of them is, and every unlock and every
 * mount failed on the first read.
 *
 * This is the one place that difference is absorbed. blk_rebl_read has the
 * callback shape every driver takes and serves any length at any offset
 * from an inner read that takes only whole sectors: the sector-aligned
 * middle of a request goes to the inner read straight into the caller's
 * buffer, and only a ragged head or tail is read whole into the bounce
 * buffer and copied out. A request that is already whole sectors is one
 * inner call with the caller's own arguments, so a 512-byte disk costs
 * nothing it did not cost before.
 *
 * Why under the callback rather than in each driver: the drivers are right
 * about what they need, and the sector size is the device's, not the
 * format's. Fixing each driver would mean every driver learning the device
 * sector size and carrying a sector-sized buffer, and the next driver
 * forgetting to. One layer at the platform boundary fixes all of them and
 * every driver written after them.
 *
 * No state, no allocation and no .bss: the bounce buffer is the caller's
 * (falcon-bios stage 3 and the UEFI loader each own one), and a blk_rebl is
 * a handful of fields a loader's callback fills on its stack per call. There
 * is deliberately no cache of the last bounced sector: a cache keyed on a
 * context pointer goes stale when a disk is re-probed or a medium swapped
 * under the same pointer, and re-blocking already costs the same number of
 * device calls as a 512-byte disk makes.
 */

#ifndef FALCON_BLK_REBL_H
#define FALCON_BLK_REBL_H

#include <stdint.h>

/* The shape of every read callback in the tree (fs_read_fn, luks_read_fn,
 * gpt_read_fn, ...). 0 on success, nonzero on failure. */
typedef int (*blk_read_fn)(void *ctx, uint64_t off, uint64_t len, void *buf);

typedef struct {
    /* Reads whole sectors at sector-aligned offsets; is never asked for
     * anything else, and never for zero bytes. */
    blk_read_fn read;
    void       *ctx;
    /* The device's sector size, which need not be a power of two. */
    uint32_t    sector_size;
    /* The caller's, at least one sector. A bounce shorter than a sector
     * still passes whole-sector requests through, and fails the others. */
    uint8_t    *bounce;
    uint32_t    bounce_len;
} blk_rebl;

/* The callback: ctx is a blk_rebl. Any offset, any length; zero bytes is
 * success without an inner read. Fails on a bad context, a range that wraps
 * past 2^64, a ragged request whose sector will not fit the bounce, and any
 * inner failure, which is passed on without a retry. On failure the
 * caller's buffer may hold part of the data. */
int blk_rebl_read(void *ctx, uint64_t off, uint64_t len, void *buf);

#endif /* FALCON_BLK_REBL_H */
