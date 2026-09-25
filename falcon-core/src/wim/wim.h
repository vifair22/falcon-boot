/* falcon-boot: reading a WIM, and the one patch the loader makes to Windows
 * install media's boot.wim (docs/DESIGN.md Section 6.3, moved into the loader
 * by Section 6.4; docs/notes/coverage-roadmap.md M6).
 *
 * The patch is DESIGN 6.3's: in the boot image, \Windows\System32\
 * winpeshl.exe becomes the jump binary (falcon-winjump), the original is
 * kept beside it as winpeshl_orig.exe for the jump binary to run once it has
 * attached the ISO, and falcon-winjump.cfg beside both says which ISO.
 *
 * The way it is made is Ventoy's, which suits a loader that serves the WIM
 * rather than rewrites it: nothing of the original is moved. The jump
 * binary, its configuration, a new metadata resource for the boot image and
 * a new lookup table are appended after the WIM's end, all stored
 * uncompressed, and only the 208-byte header changes in place, to point at
 * them. So the served WIM is the original bytes, a new header, and a tail.
 *
 * Inside the new metadata resource, the boot image's metadata is the original
 * with these changes: System32's list of children is copied to the end
 * (the old list stays, unreferenced) and System32's dentry points at the
 * copy; in the copy, winpeshl.exe's stream hash is the jump binary's, and
 * winpeshl_orig.exe (the original stream) and falcon-winjump.cfg are
 * added.
 *
 * Input is untrusted: every offset and length read from the WIM is checked
 * against what it points into before it is used.
 */

#ifndef FALCON_WIM_H
#define FALCON_WIM_H

#include <stddef.h>
#include <stdint.h>

enum {
    WIM_OK            = 0,
    WIM_ERR_BAD_ARG   = -1,
    WIM_ERR_IO        = -2,
    WIM_ERR_FORMAT    = -3,  /* not a WIM, or one this reader does not take */
    WIM_ERR_CORRUPT   = -4,  /* a structure inside it is inconsistent */
    WIM_ERR_NOSPACE   = -5,
    WIM_ERR_NOT_FOUND = -6,  /* no winpeshl.exe in the boot image */
    WIM_ERR_HASH      = -7   /* a resource did not hash to its name */
};

typedef int (*wim_read_fn)(void *ctx, uint64_t off, uint64_t len, void *buf);

#define WIM_HDR_LEN      208u
#define WIM_RESHDR_LEN   24u
#define WIM_LOOKUP_LEN   50u   /* one lookup table entry */
#define WIM_HASH_LEN     20u

#define WIM_RES_FREE       0x01u
#define WIM_RES_METADATA   0x02u
#define WIM_RES_COMPRESSED 0x04u

typedef struct {
    uint64_t size;        /* stored size in the WIM */
    uint8_t  flags;
    uint64_t offset;
    uint64_t orig;        /* uncompressed size */
} wim_reshdr;

typedef struct {
    uint8_t    hdr[WIM_HDR_LEN];
    uint64_t   file_size;
    uint32_t   flags;
    uint32_t   chunk_size;
    uint32_t   image_count;
    uint32_t   boot_index;
    wim_reshdr lookup;
    wim_reshdr xml;
    wim_reshdr boot_meta;
} wim_info;

/* Reads and checks the header of the size-byte WIM at read. Takes an
 * LZX-compressed or uncompressed, single-part, non-solid WIM with 32 KiB
 * chunks and a boot image; anything else is WIM_ERR_FORMAT. */
int wim_open(wim_read_fn read, void *ctx, uint64_t size, wim_info *w);

/* Reads one resource whole into out (exactly r->orig bytes, cap at least
 * that), decompressing chunk by chunk. scratch holds one compressed chunk and
 * its chunk table, and must be WIM_SCRATCH_LEN bytes. */
#define WIM_SCRATCH_LEN (2u * 32768u + 8u)
int wim_read_resource(const wim_info *w, wim_read_fn read, void *ctx,
                      const wim_reshdr *r, uint8_t *out, size_t cap,
                      uint8_t *scratch);

/* The first chunk of a resource: the first 32 KiB of what it holds, or all
 * of it when it is shorter, into out (WIM_CHUNK_LEN bytes). *len is how many.
 * Enough to read a file's headers without reading the file. */
#define WIM_CHUNK_LEN 32768u
int wim_read_head(const wim_info *w, wim_read_fn read, void *ctx,
                  const wim_reshdr *r, uint8_t *out, size_t *len,
                  uint8_t *scratch);

/* The lookup table's entry i, from the table's bytes. */
void wim_lookup_entry(const uint8_t *table, size_t i, wim_reshdr *r,
                      uint32_t *refcnt, uint8_t hash[WIM_HASH_LEN]);

/* ---- the patch ---- */

/* Where the boot image's \Windows\System32\winpeshl.exe is stored: its
 * lookup table entry's resource header, found by the stream hash its dentry
 * carries. It is the file the patch replaces with the jump binary, so its
 * headers say which processor the image is for, and therefore which build
 * of the jump binary it can run (a 32-bit WinPE cannot start a 64-bit
 * program). lookup and meta as for wim_patch_build. WIM_ERR_NOT_FOUND when
 * there is no such file or no lookup entry for it. */
int wim_winpeshl(const wim_info *w, const uint8_t *lookup,
                 const uint8_t *meta, size_t meta_len, wim_reshdr *out);

/* Bytes the tail can need, for a boot image metadata resource of meta_len
 * bytes, a lookup table of lookup_len, a jump binary of jump_len and its
 * configuration of cfg_len. */
size_t wim_patch_tail_max(size_t meta_len, size_t lookup_len,
                          size_t jump_len, size_t cfg_len);

/* Builds the patch. lookup is the original lookup table (w->lookup.orig
 * bytes), meta the boot image's metadata, decompressed (meta_len bytes);
 * jump is the jump binary and cfg its falcon-winjump.cfg, which goes beside
 * it in System32. Writes the new header into hdr and the tail, which follows
 * the original WIM's last byte, into tail (*tail_len bytes).
 * WIM_ERR_NOT_FOUND when the boot image has no \Windows\System32\
 * winpeshl.exe. */
int wim_patch_build(const wim_info *w, const uint8_t *lookup,
                    const uint8_t *meta, size_t meta_len, const void *jump,
                    size_t jump_len, const void *cfg, size_t cfg_len,
                    uint8_t hdr[WIM_HDR_LEN], uint8_t *tail, size_t cap,
                    size_t *tail_len);

#endif /* FALCON_WIM_H */
