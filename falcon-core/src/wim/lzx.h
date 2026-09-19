/* falcon-boot: LZX decompression as a WIM uses it (docs/notes/
 * coverage-roadmap.md M6).
 *
 * The loader reads a Windows boot.wim to patch it at boot, and Microsoft
 * compresses its resources with LZX. This is the WIM flavour of the format
 * ([MS-PATCH] LZX DELTA without the delta, as wimlib documents it): every
 * chunk of up to 32 KiB is compressed on its own with a 32 KiB window, a block
 * header carries a 1-bit "default size" flag before its 16-bit size, and the
 * x86 CALL (E8) translation is always on, against a fixed file size of
 * 12,000,000.
 *
 * Decompression only, and only of one chunk at a time: the caller splits a
 * resource into chunks (falcon-core/src/wim/wim.c). Input is untrusted; every
 * table built from it and every match copied is bounded, and anything that
 * does not decode is an error, never a partial chunk passed off as whole.
 */

#ifndef FALCON_LZX_H
#define FALCON_LZX_H

#include <stddef.h>
#include <stdint.h>

enum {
    LZX_OK          = 0,
    LZX_ERR_BAD_ARG = -1,
    LZX_ERR_CORRUPT = -2   /* not a valid LZX chunk, or not this size */
};

/* The largest chunk this decoder takes: WIM's default, and its window. */
#define LZX_CHUNK_MAX 32768u

/* Decompresses in[0..in_len) into exactly out_len bytes (out_len is the
 * chunk's uncompressed size, known from the WIM, at most LZX_CHUNK_MAX). */
int lzx_decompress(const uint8_t *in, size_t in_len, uint8_t *out,
                   size_t out_len);

#endif /* FALCON_LZX_H */
