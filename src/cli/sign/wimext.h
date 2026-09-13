/* falcon-boot-sign: finding a WIM's extent inside a Windows install ISO.
 *
 * The prepared-media shape of DESIGN Section 6.3 serves one byte range of the
 * virtual ISO out of a sidecar file, so preparation has to answer one question
 * exactly: where in the ISO's own address space does \sources\boot.wim start,
 * and is it one contiguous run. A wrong answer here is served to Windows as if
 * it were right, so the module refuses rather than guesses.
 *
 * Why this is not a UDF parser. The bytes we need are 8 bytes of magic and a
 * 208-byte header that self-describes the file's internal layout, and the
 * whole extent is available for comparison against an extracted copy. So the
 * offset is found by scanning for a plausible WIM header at a sector boundary
 * and then *proved* by comparing every byte of the candidate range against the
 * copy the ISO reader extracted. A full-length byte match is what contiguity
 * means, which makes the proof stronger than any descriptor walk and costs a
 * sequential read of the range. Writing a UDF allocation-descriptor reader to
 * get an answer we would then have to verify this way anyway would be strictly
 * more code for strictly less assurance (DESIGN Section 8.1's vendoring line
 * makes the same argument about not reimplementing a UDF reader in-repo).
 *
 * Deliberately EFI-free and I/O-free: every byte arrives through a read
 * callback, exactly as src/exfat and src/luks take their device access, so the
 * scan and the comparison are exercised in test/wimext_test.c against synthetic
 * in-memory images with no ISO and no subprocess involved.
 */

#ifndef FALCON_CLI_SIGN_WIMEXT_H
#define FALCON_CLI_SIGN_WIMEXT_H

#include <stdint.h>
#include <stddef.h>

/* The on-disk WIM header is exactly this long, and the field is checked
 * against it rather than assumed: a candidate whose header does not claim 208
 * bytes is not a WIM header, whatever its first 8 bytes say. */
#define WIM_HEADER_SIZE 208u

/* Data extents of a Windows install ISO are laid out on 2048-byte sectors,
 * which both ISO 9660 and UDF use as their logical block size on this medium.
 * The scan only ever considers a candidate on that boundary; an extent that is
 * not sector-aligned is not one this route can serve, because the loader's
 * override layer addresses the virtual ISO in the same units. */
#define WIMEXT_ALIGN 2048u

/* How much of the extracted copy's head a caller may ask the scan to match.
 * The reference spike (tools/win-spike/patch-bootwim.sh) matched 4096 bytes
 * and that is the number this inherits: the 208-byte header alone is not a
 * discriminator, because install.wim sits beside boot.wim on the same medium
 * with the same magic at the same kind of boundary and a header that passes
 * every structural check. Four kilobytes of the actual file cannot collide. */
#define WIMEXT_HEAD_MAX 4096u

/* Reads up to len bytes at byte offset off. *got receives what was actually
 * delivered, which is short only at end of medium. Returns 0 on success and
 * nonzero when the medium could not be read at all, which is the same contract
 * family_probe's read callback uses. */
typedef int (*wimext_read_fn)(void *ctx, uint64_t off, void *buf, size_t len,
                              size_t *got);

/* One resource header as it appears in the WIM header: a 56-bit compressed
 * size with its flag byte, then the offset and the uncompressed size. An
 * absent resource is all zeroes. */
typedef struct {
    uint64_t size_in_wim;
    uint64_t offset;
    uint64_t uncompressed;
    uint8_t  flags;
} wim_reshdr;

typedef struct {
    uint32_t   header_size;
    uint32_t   version;
    uint32_t   flags;
    uint32_t   chunk_size;
    uint16_t   part_number;
    uint16_t   total_parts;
    uint32_t   image_count;
    /* dwBootIndex: which image WinPE actually boots. Measured as 2 on Win11
     * 25H2 media (DESIGN Section 6.3), and read from the header rather than
     * hard-coded so a build that moves it patches the right image. Zero means
     * the WIM names no bootable image. */
    uint32_t   boot_index;
    wim_reshdr offset_table;
    wim_reshdr xml;
    wim_reshdr boot_metadata;
    wim_reshdr integrity;
} wim_header;

/* Parses and validates a WIM header out of the first WIM_HEADER_SIZE bytes of
 * buf.
 *
 * total_len is the length the ISO's directory entry claims for the file. When
 * it is nonzero, every resource the header names must lie inside it and none
 * may start inside the header itself: that is the "length agrees with the
 * directory entry" check, and it is what makes a false positive during the
 * scan essentially impossible, since the four resource offsets of a real WIM
 * land within a few kilobytes of its exact end. Pass 0 to parse a header whose
 * containing file's length is not known yet.
 *
 * SGN_OK, or SGN_ERR_FORMAT with the error buffer saying which check failed.
 * The caller may treat SGN_ERR_FORMAT as "not a WIM header here" rather than
 * as a failure; the scan does exactly that. */
int wim_header_parse(const void *buf, size_t len, uint64_t total_len,
                     wim_header *out);

/* Finds the single sector-aligned offset in the image at which a WIM of
 * exactly want_len bytes begins, and writes it to *out_off.
 *
 * want_head is the first head_len bytes of the extracted copy (at least
 * WIM_HEADER_SIZE, at most WIMEXT_HEAD_MAX) and a candidate must reproduce
 * them exactly as well as parse as a header for a file of want_len bytes. The
 * head match is what tells boot.wim apart from the install.wim beside it; the
 * header parse is what ties the candidate to the length the ISO's directory
 * entry claims.
 *
 * This is still a candidate search rather than a proof: it establishes that
 * the file begins there, not that it continues there. Contiguity is only
 * established by wimext_compare over the whole range, and the caller must run
 * it. SGN_ERR_FORMAT when no candidate is found. */
int wimext_locate(wimext_read_fn rd, void *ctx, uint64_t image_size,
                  const void *want_head, size_t head_len,
                  uint64_t want_len, uint64_t *out_off);

/* Compares len bytes of two media and, on a mismatch, reports the offset of
 * the first differing byte relative to the start of the compared range.
 *
 * This is the contiguity proof: when the ISO's bytes at the located offset
 * equal the whole of the file the ISO reader extracted, the file occupies one
 * unbroken run there, which is exactly what a single splice can serve.
 * SGN_ERR_FORMAT on a mismatch or a short read, SGN_ERR_IO when either medium
 * fails. */
int wimext_compare(wimext_read_fn a, void *a_ctx, uint64_t a_off,
                   wimext_read_fn b, void *b_ctx, uint64_t b_off,
                   uint64_t len, uint64_t *first_diff);

#endif /* FALCON_CLI_SIGN_WIMEXT_H */
