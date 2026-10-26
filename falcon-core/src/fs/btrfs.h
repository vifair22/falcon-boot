/* falcon-boot: a read-only btrfs reader, for a shelf (DESIGN Section 3).
 *
 * The set is what `mkfs.btrfs` makes on one device and what a kernel writes
 * to a shelf: SINGLE and DUP chunks, crc32c checksums, the default
 * subvolume, inline, regular and preallocated extents, holes with or
 * without no-holes. Refused: more than one device, a striped or parity
 * profile, a checksum other than crc32c, the zoned, extent-tree-v2 and
 * raid-stripe-tree formats, and a compressed extent, which is refused when
 * a read reaches it (BTRFS_ERR_UNSUPPORTED) rather than at mount, since a
 * volume mounted with compress= can still hold the UKI uncompressed.
 *
 * Nothing is replayed. btrfs is copy-on-write and its superblock names a
 * tree that was complete when it was written; what a crash can leave is a
 * log tree of fsyncs since that commit, which is not read
 * (BTRFS_LOG_SKIPPED), so a shelf is read as it was at its last commit.
 *
 * Memory is the volume's 4 KiB window: a node (16 KiB by default) is
 * never held whole. Its checksum is streamed through the window the first
 * time a mount visits it and remembered (BTRFS_MEMO nodes), since a node
 * a commit wrote is never rewritten in place; every key, item and
 * directory entry after that is copied out of the window as it is read.
 *
 * Every driver in the registry reads through the same byte-read callback.
 */

#ifndef FALCON_FS_BTRFS_H
#define FALCON_FS_BTRFS_H

#include <stdint.h>
#include <stddef.h>

typedef int (*btrfs_read_fn)(void *ctx, uint64_t off, uint64_t len,
                             void *buf);

enum {
    BTRFS_OK              = 0,
    BTRFS_ERR_IO          = -1,
    BTRFS_ERR_NOT_BTRFS   = -2,
    BTRFS_ERR_CORRUPT     = -3,
    BTRFS_ERR_BAD_ARG     = -4,
    BTRFS_ERR_RANGE       = -5,
    BTRFS_ERR_UNSUPPORTED = -6,
    BTRFS_ERR_CSUM        = -7,
    BTRFS_ERR_NOT_FOUND   = -8
};

#define BTRFS_SUPER_OFF  0x10000u
#define BTRFS_WIN        4096u
#define BTRFS_CHUNKS_MAX 16u
#define BTRFS_MEMO       8u
#define BTRFS_NAME_MAX   255u

/* What mount found: a log tree it did not read. */
#define BTRFS_LOG_SKIPPED 0x1u

typedef struct {
    uint64_t objectid;
    uint8_t  type;
    uint64_t offset;
} btrfs_key;

/* One chunk: a run of logical bytes and where its first copy is. */
typedef struct {
    uint64_t logical;
    uint64_t length;
    uint64_t physical;
} btrfs_chunk;

typedef struct {
    btrfs_read_fn read;
    void *ctx;

    uint32_t sectorsize;
    uint32_t nodesize;
    uint64_t total_bytes;
    uint8_t  fsid[16];          /* the one tree blocks carry */
    uint32_t flags;             /* BTRFS_LOG_SKIPPED */

    uint64_t root_tree;         /* logical, and its level */
    uint8_t  root_level;
    uint64_t fs_tree;           /* the default subvolume's tree */
    uint8_t  fs_level;
    uint64_t root_dir;          /* its top directory's inode */

    btrfs_chunk chunks[BTRFS_CHUNKS_MAX];
    uint32_t nchunks;

    /* Nodes whose checksum this mount has verified, a ring. */
    uint64_t memo[BTRFS_MEMO];
    uint32_t memo_next;

    /* The window: BTRFS_WIN bytes at physical win_off; ~0 invalid. */
    uint64_t win_off;
    uint8_t  win[BTRFS_WIN];
} btrfs_vol;

typedef struct {
    uint8_t  name[BTRFS_NAME_MAX + 1]; /* NUL-terminated bytes */
    uint8_t  name_len;
    uint8_t  is_dir;
    uint8_t  is_regular;
    uint64_t ino;
    uint64_t size;
} btrfs_dirent;

typedef struct {
    btrfs_vol *vol;
    uint64_t ino;
    uint64_t next_index;        /* the DIR_INDEX key offset to look for */
} btrfs_dir;

typedef struct {
    btrfs_vol *vol;
    uint64_t ino;
    uint64_t size;
} btrfs_file;

/* The magic at 64 KiB, one read. */
int btrfs_probe(btrfs_read_fn read, void *ctx);

/* The superblock, the chunk map and the default subvolume. */
int btrfs_mount(btrfs_vol *v, btrfs_read_fn read, void *ctx);

int btrfs_dir_open_root(btrfs_vol *v, btrfs_dir *d);
int btrfs_dir_open(btrfs_vol *v, btrfs_dir *d, const btrfs_dirent *e);
/* 1 = entry produced, 0 = end, negative = BTRFS_ERR_*. In index order,
 * which is creation order. A subvolume inside the tree is passed over. */
int btrfs_dir_next(btrfs_dir *d, btrfs_dirent *out);

int btrfs_file_open(btrfs_vol *v, btrfs_file *f, const btrfs_dirent *e);
int btrfs_file_read(btrfs_file *f, uint64_t off, uint64_t len, void *buf);

#endif /* FALCON_FS_BTRFS_H */
