/* falcon-core: a gzip member, inflated (RFC 1952 around RFC 1951).
 *
 * For falcon jump (docs/DESIGN.md Section 13.3): stage 3 goes on the floppy
 * compressed, since on a floppy the bytes read are the time taken, and stage
 * 2 inflates it. Written for that reader: no allocation, no state between
 * calls, every buffer the caller's, and no .bss, since stage 2 has no loader
 * to clear one. The Huffman tables live on the caller's stack, about 1.3 KB.
 *
 * The gzip wrapper is not decoration: its CRC-32 and length cover the whole
 * output, so a floppy that read wrong is caught before the result runs.
 */

#ifndef FALCON_GZ_H
#define FALCON_GZ_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    GZ_OK        =  0,
    GZ_ERR_ARG   = -1,
    GZ_ERR_FORMAT = -2,   /* not a gzip member, or a method it cannot be */
    GZ_ERR_DATA  = -3,    /* the deflate stream is malformed or cut short */
    GZ_ERR_SPACE = -4,    /* the output does not fit the buffer */
    GZ_ERR_CHECK = -5     /* inflated, but the CRC-32 or length disagree */
} gz_status;

/* A raw deflate stream (RFC 1951) of `n` bytes into `out` (`cap` bytes).
 * *out_len the bytes written, *used the input bytes the stream took. */
gz_status gz_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                     size_t *out_len, size_t *used);

/* One gzip member (RFC 1952) of `n` bytes into `out`, its CRC-32 and length
 * checked. *out_len the bytes written. */
gz_status gz_member(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                    size_t *out_len);

/* CRC-32 (the gzip and zip one) of `n` bytes, continuing from `crc` (0 to
 * start). Bitwise, no table: stage 2 cannot keep one in .bss, and a floppy's
 * worth of bytes takes milliseconds. */
uint32_t gz_crc32(uint32_t crc, const uint8_t *p, size_t n);

#endif /* FALCON_GZ_H */
