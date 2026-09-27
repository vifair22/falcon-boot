/* A 4096-byte-sector disk over any byte-granular test image, for the suites
 * that prove the drivers mount and unlock on a 4Kn disk (docs/DESIGN.md
 * Section 3.4).
 *
 * kn_strict_read is the platform callback as a 4Kn disk's firmware gives it:
 * whole sectors at sector-aligned offsets, and a refusal (counted) for
 * anything else, which is what Block I/O and INT 13h do. kn_dev puts the
 * core's re-blocking layer on top of it, which is what the loaders hand the
 * drivers. A suite reads its existing fixture through kn_dev and asserts
 * both the result and that the strict device never saw a request it would
 * refuse; the same image read through kn_strict_read alone is the failure
 * the layer exists to remove.
 */

#ifndef FALCON_TEST_KNDEV_H
#define FALCON_TEST_KNDEV_H

#include <stdint.h>

#include <blk/rebl.h>

#define KN_SECTOR 4096u

typedef struct {
    blk_read_fn inner;       /* the image, any offset and length */
    void       *inner_ctx;
    uint32_t    sector;
    unsigned    calls;       /* requests that reached the image */
    unsigned    refused;     /* requests a real disk would have failed */
} kn_strict;

typedef struct {
    kn_strict strict;
    blk_rebl  rebl;
    uint8_t   bounce[KN_SECTOR];
} kn_dev;

int kn_strict_read(void *ctx, uint64_t off, uint64_t len, void *buf);

/* A 4Kn disk over (inner, inner_ctx). The drivers take blk_rebl_read with
 * &k->rebl as their context. */
void kn_init(kn_dev *k, blk_read_fn inner, void *inner_ctx);

#endif /* FALCON_TEST_KNDEV_H */
