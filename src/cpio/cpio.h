/* airies-boot: newc cpio writer for initrd append (docs/DESIGN.md Section
 * 6.2).
 *
 * The injection path hands the kernel the ISO's own initrd with a segment of
 * ours concatenated after it. Linux unpacks an initramfs image as a sequence
 * of archives, so an extra newc archive appended to the end is simply
 * unpacked last and its files win; this is the same early-initramfs override
 * mechanism microcode blobs and systemd-stub's credentials use.
 *
 * Uncompressed on purpose. The kernel accepts an uncompressed archive
 * anywhere in that sequence (each segment is sniffed for a compression magic
 * and taken as raw cpio when there is none), so compressing ours would buy a
 * few kilobytes at the price of a compressor in EFI-land. Our segments are
 * hook scripts and an extent table; there is nothing to gain.
 *
 * Freestanding and EFI-free: the caller supplies the buffer, so the writer
 * needs no allocator and is testable on a host.
 */

#ifndef AIRIES_CPIO_H
#define AIRIES_CPIO_H

#include <stdint.h>
#include <stddef.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    CPIO_OK           = 0,
    CPIO_ERR_BAD_ARG  = -1,
    CPIO_ERR_NOSPACE  = -2, /* buffer too small; *out_size has what is needed */
    CPIO_ERR_RANGE    = -3, /* name or data too large for a newc header field */
};

/* Modes for the common cases. newc stores the full POSIX mode, file type
 * bits included, so the type of an entry is carried here and nowhere else. */
#define CPIO_MODE_FILE 0100644u
#define CPIO_MODE_DIR  0040755u

typedef struct {
    /* Archive-relative path: '/'-separated components, each one a non-empty
     * name that is not "..". That rules out a leading or trailing '/' and an
     * empty component ("a//b"), which name nothing, and any path the kernel's
     * unpacker would resolve to somewhere outside the initramfs root this
     * archive describes. A "." component is legal; it resolves to the
     * directory it sits in and cannot leave the root. Anything else is
     * CPIO_ERR_BAD_ARG for the whole archive.
     *
     * Every parent directory must appear as its own CPIO_MODE_DIR entry
     * earlier in the list: the kernel's unpacker creates only what the archive
     * names, so a file under a missing directory is silently dropped. */
    const char *path;
    const void *data;      /* NULL for a directory, or for an empty file */
    size_t      len;       /* 0 for a directory */
    uint32_t    mode;      /* CPIO_MODE_*, or any POSIX mode with type bits */
} cpio_entry;

/* Bytes cpio_write would produce, trailer included. */
int cpio_size(const cpio_entry *entries, size_t count, size_t *out_size);

/* Writes the archive into buf. On CPIO_ERR_NOSPACE nothing is written and
 * *out_size carries the size required. */
int cpio_write(const cpio_entry *entries, size_t count, void *buf,
               size_t cap, size_t *out_size);

#endif /* AIRIES_CPIO_H */
