/* falcon-boot: read-only UDF (docs/DESIGN.md Sections 6.4 and 9).
 *
 * Sibling of falcon-core/src/fs/iso9660.c. Windows install media is a UDF
 * bridge disc whose ISO9660 side is a one-file stub (Microsoft's oscdimg puts
 * a README there and everything else only in UDF), so reading any of it -
 * boot.wim for the loader's patch of Section 6.4, the edition list for the
 * sign tool - needs this driver. Before it, the sign tool shelled out to 7-Zip
 * and the loader could not read Windows media at all.
 *
 * Scope, matched to what oscdimg and mkudffs actually write:
 *   - 2048-byte logical blocks, the only size an ISO image has; anything else
 *     is UDF_ERR_UNSUPPORTED
 *   - one type 1 (physical) partition map; the metadata, sparable and
 *     virtual maps of UDF 2.x are refused, since no ISO in the corpus uses
 *     them and each is a second address translation to get wrong
 *   - File Entries and Extended File Entries, strategy 4 ICBs
 *   - short and long allocation descriptors, allocation extent chains, and
 *     data embedded in the entry itself
 *   - unrecorded and unallocated extents, which read as zeros
 *
 * Every descriptor tag read is checked: its checksum, its recorded location,
 * and its CRC over the descriptor body. A UDF image carries all three so a
 * reader can tell a descriptor from a coincidence, and there is no reason to
 * trust bytes off p2 more than the format does.
 */

#ifndef FALCON_UDF_H
#define FALCON_UDF_H

#include <stdint.h>
#include <stddef.h>

enum {
    UDF_OK              = 0,
    UDF_ERR_IO          = -1,
    UDF_ERR_BAD_SUPER   = -2, /* no UDF volume recognition sequence / anchor */
    UDF_ERR_CORRUPT     = -3,
    UDF_ERR_BAD_ARG     = -4,
    UDF_ERR_RANGE       = -5,
    UDF_ERR_NOSPACE     = -6,
    UDF_ERR_UNSUPPORTED = -7,
    UDF_ERR_SYMLINK     = -8
};

typedef int (*udf_read_fn)(void *ctx, uint64_t off, uint64_t len, void *buf);

#define UDF_BLOCK     2048u
#define UDF_NAME_MAX  255u

/* Allocation descriptors a file may have. A recorded extent is at most 1 GiB
 * less a block, so a contiguous file needs one per GiB: Windows' install.wim
 * takes seven. Fragmented files take more, and a file needing more than this
 * is refused rather than half read. */
#define UDF_MAX_EXTENTS 128u

typedef struct {
    udf_read_fn read;
    void *read_ctx;

    uint32_t part_start;        /* partition's first block, volume-relative */
    uint32_t part_len;          /* in blocks */
    uint32_t root_lbn;          /* root directory ICB, partition-relative */

    /* The Logical Volume Descriptor's identifier (the PVD's when that is
     * empty), narrowed to ASCII with '?' for anything wider: what blkid
     * reports as the medium's label. Truncated to 32 characters. */
    char     label[33];

    uint64_t scratch_off;       /* ~0 invalid */
    uint8_t  scratch[UDF_BLOCK];
} udf_vol;

typedef struct {
    uint16_t name[UDF_NAME_MAX + 1];
    uint16_t name_len;
    uint8_t  is_dir;
    uint8_t  is_regular;
    uint64_t size;
    uint32_t icb_lbn;           /* the entry's File Entry, partition-relative */
} udf_dirent;

/* One allocation descriptor, reduced to what reading needs: where (in
 * partition blocks), how many bytes, and whether the bytes are recorded. */
typedef struct {
    uint32_t lbn;
    uint32_t len;               /* bytes */
    uint8_t  recorded;
} udf_extent_rec;

typedef struct {
    udf_vol *vol;
    uint64_t size;
    uint8_t  is_dir;
    uint8_t  embedded;          /* the data lives inside the File Entry */
    uint32_t icb_lbn;
    uint16_t inline_off;        /* embedded only: where in the entry's block */
    uint16_t nextents;
    udf_extent_rec ext[UDF_MAX_EXTENTS];
    uint64_t buf_off;           /* ~0 invalid */
    uint8_t  buf[UDF_BLOCK];
} udf_file;

typedef struct {
    udf_file dir;               /* the directory's own byte stream */
    uint64_t pos;
} udf_dir;

typedef struct {
    uint64_t vol_off;
    uint64_t len;
} udf_extent;

int udf_probe(udf_read_fn read, void *read_ctx);
int udf_mount(udf_vol *v, udf_read_fn read, void *read_ctx);
int udf_dir_open_root(udf_vol *v, udf_dir *d);
int udf_dir_open(udf_vol *v, udf_dir *d, const udf_dirent *e);
/* 1 = entry produced, 0 = end, negative = error. The parent entry and
 * deleted entries are never produced. */
int udf_dir_next(udf_dir *d, udf_dirent *out);
int udf_file_open(udf_vol *v, udf_file *f, const udf_dirent *e);
int udf_file_read(udf_file *f, uint64_t off, uint64_t len, void *buf);
/* The recorded runs in volume bytes, adjacent runs coalesced. A file with an
 * unrecorded or unallocated extent, or with its data embedded in the entry,
 * has no physical runs to report and is UDF_ERR_UNSUPPORTED. */
int udf_file_extents(udf_vol *v, const udf_dirent *e, udf_extent *out,
                     size_t cap, size_t *count);

#endif /* FALCON_UDF_H */
