/* airies-boot: read-only exFAT (docs/DESIGN.md Sections 6, 9).
 *
 * Deliberately freestanding and EFI-free: all device access goes through a
 * byte-addressed read callback, so the same code mounts an EFI BlockIO
 * partition in the loader and a plain image file in host-side tests. Offsets
 * handed to the callback are always multiples of the volume's sector size.
 *
 * Scope for now is what the menu needs: mount, walk directories, read file
 * bytes at arbitrary offsets. The extent-map API (injection framework, DESIGN
 * Section 6.2) lands here later.
 */

#ifndef AIRIES_EXFAT_H
#define AIRIES_EXFAT_H

#include <stdint.h>
#include <stddef.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    EXFAT_OK             = 0,
    EXFAT_ERR_IO         = -1, /* read callback failed */
    EXFAT_ERR_BAD_SUPER  = -2, /* not an exFAT volume / corrupt VBR */
    EXFAT_ERR_CORRUPT    = -3, /* on-disk structure violates the spec */
    EXFAT_ERR_BAD_ARG    = -4,
    EXFAT_ERR_RANGE      = -5, /* read outside the file's valid data */
};

/* Read len bytes at byte offset off (volume-relative). Returns 0 on
 * success, nonzero on failure. */
typedef int (*exfat_read_fn)(void *ctx, uint64_t off, uint64_t len,
                             void *buf);

#define EXFAT_MAX_SECTOR 4096u   /* spec: BytesPerSectorShift <= 12 */
#define EXFAT_NAME_MAX   255u    /* UTF-16 units, spec limit */

typedef struct {
    exfat_read_fn read;
    void *read_ctx;

    uint8_t  bps_shift;          /* log2 bytes per sector, 9..12 */
    uint8_t  spc_shift;          /* log2 sectors per cluster */
    uint32_t fat_offset;         /* sectors */
    uint32_t cluster_heap_offset;/* sectors */
    uint32_t cluster_count;
    uint32_t root_cluster;

    /* One-sector FAT cache; ~0 marks it invalid. */
    uint64_t fat_cache_off;
    uint8_t  fat_cache[EXFAT_MAX_SECTOR];
} exfat_vol;

typedef struct {
    uint16_t name[EXFAT_NAME_MAX + 1];
    uint8_t  name_len;
    uint8_t  is_dir;
    uint8_t  no_fat_chain;       /* clusters are contiguous, FAT not used */
    uint32_t first_cluster;      /* 0 = no data allocated */
    uint64_t size;               /* valid data length for files,
                                    allocation length for directories */
} exfat_dirent;

typedef struct {
    exfat_vol *vol;
    uint32_t first_cluster;
    uint8_t  no_fat_chain;
    uint64_t size;               /* ~0 for the root directory (unbounded,
                                    terminated by the end-of-dir marker) */
    /* iteration state */
    uint64_t pos;                /* byte position in the directory stream */
    uint32_t cur_cluster;
    uint64_t cluster_pos;        /* pos at which cur_cluster began */
    uint8_t  sec_buf[EXFAT_MAX_SECTOR];
    uint64_t sec_buf_off;        /* volume byte offset cached, ~0 invalid */
} exfat_dir;

/* Open file, read at arbitrary byte offsets. The virtual CD (DESIGN Section 6
 * stage 2) serves every firmware block read through this, walking forward
 * through multi-GB ISOs, so the chain position is cached between calls: a
 * forward-moving read resumes the walk where the last one stopped and only a
 * backward seek restarts it from the first cluster. */
typedef struct {
    exfat_vol *vol;
    uint32_t first_cluster;
    uint8_t  no_fat_chain;       /* contiguous: cluster N is first + N */
    uint64_t size;               /* valid data length; reads past it fail */

    /* Chain cursor: cur_cluster is the cur_index'th cluster of the file. */
    uint32_t cur_cluster;
    uint64_t cur_index;

    /* One-sector cache, for requests that are not sector-aligned. */
    uint8_t  sec_buf[EXFAT_MAX_SECTOR];
    uint64_t sec_buf_off;        /* volume byte offset cached, ~0 invalid */
} exfat_file;

int exfat_mount(exfat_vol *v, exfat_read_fn read, void *read_ctx);
int exfat_dir_open_root(exfat_vol *v, exfat_dir *d);
int exfat_dir_open(exfat_vol *v, exfat_dir *d, const exfat_dirent *e);
/* 1 = entry produced, 0 = end of directory, negative = error. */
int exfat_dir_next(exfat_dir *d, exfat_dirent *out);

int exfat_file_open(exfat_vol *v, exfat_file *f, const exfat_dirent *e);
/* Reads len bytes at file offset off. Wholly inside the file or nothing:
 * a short read is never reported as success. */
int exfat_file_read(exfat_file *f, uint64_t off, uint64_t len, void *buf);

#endif /* AIRIES_EXFAT_H */
