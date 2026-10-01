/* falcon-boot: the virtual-media override layer (DESIGN Sections 6.3, 6.4
 * and 9).
 *
 * A patched medium keeps its bytes untouched on p2. What changes is the VIEW
 * the loader publishes of it: named byte ranges of the virtual image are
 * answered from memory the loader holds (or from another p2 file) instead of
 * from the image, and the medium may run PAST the end of the image file into
 * virtual sectors that exist only in the override. Ventoy calls the two halves
 * "override chunks" and "virt chunks"; DESIGN 6.3 calls them the override list
 * and the append region.
 *
 * This header is the whole of the address arithmetic, and it is deliberately
 * everything the layer knows: it is pure data with no format knowledge at all.
 * No WIM or UDF knowledge enters this layer: the byte map is computed by
 * falcon-efi/src/win with falcon-core's wim and udf, and applied here.
 *
 * Two reasons it is a header rather than a .c, both worth stating:
 *
 *  - it is EFI-free by construction (no UEFI types, no allocator, no I/O; the
 *    fs_file pointers are carried and never dereferenced), which is what makes
 *    the arithmetic testable on the host, exactly as
 *    falcon-efi/src/bless/blesscore.c is split out from
 *    falcon-efi/src/bless/bless.c for the same reason;
 *  - header-only keeps it out of the top-level Makefile's SRCS list, which is
 *    outside this change's scope. Turning it into falcon-efi/src/iso/overlay.c
 *    later is one SRCS line plus dropping `inline`; nothing here depends on
 *    being inlined.
 *
 * THREE ADDRESS SPACES, and confusing them is where the silent corruption
 * lives. Every offset below is labelled with which one it is in:
 *
 *  - VIRTUAL: the medium the firmware sees. Block reads are in this space.
 *    It runs [0, media_size) and is the ONLY space in which "is this read in
 *    range" may be decided.
 *  - IMAGE: the .iso/.img file on p2. Runs [0, image_size). Identical to the
 *    virtual space for offsets below image_size and undefined above it, which
 *    is why an append region makes the naive "bytes left = image_size - off"
 *    underflow rather than clamp.
 *  - SIDECAR: a staged file on p2, its own space entirely, reached only
 *    through an override entry's src_off.
 *
 * UNTRUSTED INPUT. The override set is computed at boot (the Windows patch
 * in falcon-efi/src/win, D3b's grub.cfg and hook in src/direct, the BSD
 * edits in src/bsd), and every one of those computations reads its input
 * off p2, a writable exFAT partition anybody can edit. iso_overlay_check is
 * a fail-closed gate, not a sanity check: an invalid set is refused
 * outright rather than partially honoured, because the alternative is
 * serving attacker-shaped bytes inside a medium the user believes is the
 * ISO they dropped on.
 */

#ifndef FALCON_ISO_OVERLAY_H
#define FALCON_ISO_OVERLAY_H

#include <stdint.h>
#include <stddef.h>

#include <fs/fs.h>

/* How many substitutions one medium may carry.
 *
 * The largest set today is Windows (DESIGN Section 11.2.13): one or two
 * Partition Descriptors, boot.wim's File Entry and its header, and the
 * appended tail, five at most. D3b and BSD use two each. Eight is that
 * plus room, and it is a BOUND rather than a capacity target: the set is
 * computed from attacker-editable bytes, it is copied by value into a
 * static device instance, and the split below is O(entries) per block
 * read. A number that cannot grow without someone re-reading this comment
 * is the point. */
#define ISO_MAX_OVERRIDES 8u

/* Worst case segments a single request can be cut into: every override
 * contributes at most one segment and at most one gap before it, plus a
 * trailing gap. Independent of how large the request is, because overrides are
 * the only boundaries in the address space. */
#define ISO_MAX_SEGMENTS (2u * ISO_MAX_OVERRIDES + 1u)

/* Status codes, repo convention: OK zero, errors negative. They are kept
 * distinct rather than collapsed into one "bad set" because the set is
 * derived from attacker-editable bytes and the refusal message has to say
 * which of these it is. */
enum {
    ISO_OVL_OK          =  0,
    ISO_OVL_ERR_BAD_ARG = -1,  /* null argument, or a request that wraps */
    ISO_OVL_ERR_COUNT   = -2,  /* more entries than ISO_MAX_OVERRIDES */
    ISO_OVL_ERR_EMPTY   = -3,  /* a zero-length entry */
    ISO_OVL_ERR_ORDER   = -4,  /* not ascending, or two entries overlap */
    ISO_OVL_ERR_RANGE   = -5,  /* outside the virtual medium */
    ISO_OVL_ERR_SOURCE  = -6,  /* no sidecar, or the sidecar is too short */
    ISO_OVL_ERR_GEOM    = -7,  /* append region is not a whole block count */
    ISO_OVL_ERR_NOSPACE = -8   /* caller's segment array too small */
};

/* One byte-range substitution.
 *
 * iso_off and len are VIRTUAL; src_off is SIDECAR. Both spaces are bytes with
 * arbitrary alignment: nothing here is required to fall on a block boundary,
 * and the split below is what reconciles that with the firmware's 2048-byte
 * (or 512-byte) block reads.
 *
 * src is an fs_file the CALLER opened and the caller owns. fs_file has no
 * close, so "owns" means only that the object and the volume it was opened on
 * must outlive the published device; nothing is released here. It must not be
 * the image's own handle - the two streams interleave on every read, and a
 * single exFAT file cursor forced to seek backwards between them restarts the
 * FAT walk each time (falcon-core/src/fs/exfat.h). iso.c refuses that case. */
typedef struct {
    uint64_t iso_off;   /* VIRTUAL: first byte this entry replaces */
    uint64_t len;       /* bytes */
    fs_file *src;       /* the sidecar these bytes come from */
    uint64_t src_off;   /* SIDECAR: where in it they start */
    /* Or bytes the loader computed at boot (the Windows patch of
     * falcon-core/src/wim/wim.h): mem is used when src is NULL, and src_off
     * is then an offset into it. The caller owns the memory for as long as
     * the device is published. */
    const uint8_t *mem;
} iso_override;

/* The whole modified view of one medium.
 *
 * extra is the append region: bytes of virtual medium past the end of the
 * image file. It is a SIZE and nothing else, because the backing is already
 * expressible as an ordinary override entry at iso_off >= image_size - which
 * is the entire reason the append region costs one field here and one line in
 * iso_read_blocks rather than a second mechanism. Appended bytes that no entry
 * covers read as zeros, exactly like the short tail of a partial final block.
 * Zero for the MVP's size-preserving Windows shape, which needs no append
 * region at all. */
typedef struct {
    iso_override ovl[ISO_MAX_OVERRIDES];
    size_t       n;
    uint64_t     extra;
} iso_overlay;

/* One contiguous piece of a request, after splitting.
 *
 * src NULL means "the image file itself", in which case src_off equals
 * virt_off: below image_size the two spaces are the same space. Non-NULL means
 * the sidecar, and src_off is then in SIDECAR coordinates. */
typedef struct {
    uint64_t virt_off;  /* VIRTUAL: where this piece starts */
    uint64_t len;
    fs_file *src;       /* NULL = serve from the image (or from mem) */
    uint64_t src_off;   /* IMAGE when both are NULL, else SIDECAR or MEMORY */
    const uint8_t *mem; /* non-NULL = serve from these bytes */
} iso_segment;

/* The medium's size in the virtual address space, given the image file's size
 * and the block size the device will advertise.
 *
 * Truncating to whole blocks is what the no-overlay path has always done
 * (LastBlock = size / block_size - 1), and it is kept so that adding an empty
 * overlay cannot change a medium's geometry by a byte. The consequence is that
 * an append region whose total is not a whole number of blocks would have its
 * tail silently dropped, so that case is REFUSED here instead: a preparer that
 * computed a region the device cannot advertise has made an arithmetic error,
 * and quietly shortening its data is the worst of the available answers. */
static inline int iso_overlay_media_size(const iso_overlay *ovl,
                                         uint64_t image_size,
                                         uint32_t block_size, uint64_t *out)
{
    uint64_t total;
    uint64_t extra = (ovl != NULL) ? ovl->extra : 0;

    if (out == NULL || block_size == 0)
        return ISO_OVL_ERR_BAD_ARG;
    *out = 0;
    if (extra > UINT64_MAX - image_size)
        return ISO_OVL_ERR_RANGE;
    total = image_size + extra;
    if (extra != 0 && total % block_size != 0)
        return ISO_OVL_ERR_GEOM;
    total -= total % block_size;
    if (total == 0)
        return ISO_OVL_ERR_RANGE;
    *out = total;
    return ISO_OVL_OK;
}

/* Validates an override set against the medium it is about to be installed on.
 * Fail-closed: any nonzero return means the set is refused entirely.
 *
 * src_sizes[i] is the size of entry i's sidecar file, supplied by the caller
 * (which is the only side that can call fs_file_size) so that the arithmetic
 * stays testable without a mounted volume. It may be NULL only for an empty
 * set.
 *
 * What is checked, and why each one matters:
 *  - the count fits the bound, so nothing walks off the fixed array;
 *  - no zero-length entry, which would otherwise be an invisible no-op that
 *    still consumed a slot and confused the ordering check;
 *  - neither the virtual range nor the sidecar range wraps uint64;
 *  - every entry lies wholly inside the virtual medium, so no read can be
 *    steered past what the device advertises;
 *  - entries ascend and do not overlap, which is what lets the split walk the
 *    set once per request and what makes "which entry wins here" have exactly
 *    one answer;
 *  - every entry names a sidecar that actually holds the bytes it promises, so
 *    the read path never has to invent data for a short source.
 *
 * media_size is iso_overlay_media_size's answer, NOT the image file's size:
 * an entry backing the append region legitimately sits past the end of the
 * image. */
static inline int iso_overlay_check(const iso_overlay *ovl, uint64_t media_size,
                                    const uint64_t *src_sizes)
{
    uint64_t prev_end = 0;
    size_t i;

    if (ovl == NULL)
        return ISO_OVL_ERR_BAD_ARG;
    if (ovl->n > ISO_MAX_OVERRIDES)
        return ISO_OVL_ERR_COUNT;
    if (ovl->n != 0 && src_sizes == NULL)
        return ISO_OVL_ERR_BAD_ARG;

    for (i = 0; i < ovl->n; i++) {
        const iso_override *e = &ovl->ovl[i];

        if (e->len == 0)
            return ISO_OVL_ERR_EMPTY;
        if (e->len > UINT64_MAX - e->iso_off)
            return ISO_OVL_ERR_RANGE;
        if (e->iso_off + e->len > media_size)
            return ISO_OVL_ERR_RANGE;
        /* Ascending and disjoint. Touching ranges (prev_end == iso_off) are
         * legal: two adjacent substitutions from different sidecars are a
         * perfectly ordinary map, and nothing downstream needs them merged. */
        if (i != 0 && e->iso_off < prev_end)
            return ISO_OVL_ERR_ORDER;
        prev_end = e->iso_off + e->len;

        if (e->src == NULL && e->mem == NULL)
            return ISO_OVL_ERR_SOURCE;
        if (e->len > UINT64_MAX - e->src_off)
            return ISO_OVL_ERR_SOURCE;
        if (e->src_off + e->len > src_sizes[i])
            return ISO_OVL_ERR_SOURCE;
    }
    return ISO_OVL_OK;
}

/* Cuts one request in the VIRTUAL address space into the pieces that answer
 * it, in ascending order, covering [off, off + len) exactly and with no gaps.
 *
 * This is the whole point of the layer. A block read is 2048 bytes (or 512)
 * and an override is an arbitrary byte range, so a single block can be part
 * image and part sidecar, or can straddle the end of one override and the
 * start of the next. Every boundary is cut precisely; nothing is rounded to a
 * block anywhere in here, and the caller reassembles the pieces into its own
 * buffer by (virt_off - off).
 *
 * The set must already have passed iso_overlay_check. Given that, the walk is
 * a single forward pass: i never moves backwards, so the cost is O(entries +
 * segments) per read rather than a search per byte.
 *
 * Returns ISO_OVL_ERR_NOSPACE if cap is smaller than the cut needs; cap of
 * ISO_MAX_SEGMENTS can never hit that. A zero-length request is success with
 * *count == 0, which is the honest answer rather than an error: the caller's
 * own contract decides whether a zero-length read is legal. */
static inline int iso_overlay_split(const iso_overlay *ovl, uint64_t media_size,
                                    uint64_t off, uint64_t len,
                                    iso_segment *out, size_t cap,
                                    size_t *count)
{
    uint64_t cur = off;
    uint64_t left = len;
    size_t i = 0;
    size_t n = 0;

    if (ovl == NULL || count == NULL)
        return ISO_OVL_ERR_BAD_ARG;
    *count = 0;
    if (ovl->n > ISO_MAX_OVERRIDES)
        return ISO_OVL_ERR_COUNT;
    if (len == 0)
        return ISO_OVL_OK;
    if (out == NULL)
        return ISO_OVL_ERR_BAD_ARG;
    /* Range is decided in the virtual space and nowhere else: the image file's
     * size says nothing about how far the medium runs once an append region
     * exists, and the sidecar's size says nothing about either. */
    if (len > UINT64_MAX - off)
        return ISO_OVL_ERR_BAD_ARG;
    if (off + len > media_size)
        return ISO_OVL_ERR_RANGE;

    while (left > 0) {
        uint64_t take;

        /* Drop every entry that ends at or before the cursor. Sorted and
         * disjoint, so this only ever moves forward, across the whole
         * request rather than per segment. */
        while (i < ovl->n && ovl->ovl[i].iso_off + ovl->ovl[i].len <= cur)
            i++;

        if (n == cap)
            return ISO_OVL_ERR_NOSPACE;

        if (i < ovl->n && ovl->ovl[i].iso_off <= cur) {
            /* Inside an override. Starting mid-entry is the straddle case:
             * skip is how far into the entry the cursor already is, and it
             * offsets the SIDECAR read by the same amount. */
            const iso_override *e = &ovl->ovl[i];
            uint64_t skip = cur - e->iso_off;

            take = e->len - skip;
            if (take > left)
                take = left;
            out[n].src = e->src;
            out[n].mem = e->mem;
            out[n].src_off = e->src_off + skip;
        } else {
            /* Image bytes, running up to the next override's start (or to the
             * end of the request when none is left). */
            take = left;
            if (i < ovl->n && ovl->ovl[i].iso_off - cur < take)
                take = ovl->ovl[i].iso_off - cur;
            out[n].src = NULL;
            out[n].mem = NULL;
            out[n].src_off = cur;
        }

        out[n].virt_off = cur;
        out[n].len = take;
        n++;
        cur += take;
        left -= take;
    }

    *count = n;
    return ISO_OVL_OK;
}

#endif /* FALCON_ISO_OVERLAY_H */
