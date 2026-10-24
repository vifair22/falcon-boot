/* falcon-boot: a read-only xfs reader, for a shelf (DESIGN Section 3).
 *
 * The set is the v5 format `mkfs.xfs` makes: CRCs on, an internal log,
 * directory blocks one filesystem block each, and the incompat features
 * mkfs turns on or offers (ftype, sparse inodes, the metadata UUID, bigtime,
 * 64-bit extent counts, exchange-range, parent pointers, the metadata
 * directory). Data forks in extents and B+tree format; directories in
 * short form and in block, leaf and node form, whose entries are all in
 * the data blocks below XFS_DIR2_LEAF_OFFSET, which is where they are read
 * (the leaf and free blocks above it are indexes this reader needs none
 * of). Refused: v4, an external or realtime device, larger directory
 * blocks, needsrepair and anything newer.
 *
 * A log that is not clean (no unmount record at its head) is
 * XFS_ERR_NEEDS_RECOVERY from xfs_mount, and replayed in memory by
 * xfs_mount_journal (xfslog.c).
 *
 * Memory is the volume's 4 KiB window; a structure that crosses it is read
 * across, and every checksum is streamed through it (crc32c, with the
 * field it covers counted as zeros).
 *
 * On-disk xfs is big-endian except its checksums.
 */

#ifndef FALCON_FS_XFS_H
#define FALCON_FS_XFS_H

#include <stdint.h>
#include <stddef.h>

typedef int (*xfs_read_fn)(void *ctx, uint64_t off, uint64_t len, void *buf);

enum {
    XFS_OK                 = 0,
    XFS_ERR_IO             = -1,
    XFS_ERR_NOT_XFS        = -2,
    XFS_ERR_CORRUPT        = -3,
    XFS_ERR_BAD_ARG        = -4,
    XFS_ERR_RANGE          = -5,
    XFS_ERR_UNSUPPORTED    = -6,
    XFS_ERR_CSUM           = -7,
    XFS_ERR_NEEDS_RECOVERY = -8,
    XFS_ERR_JOURNAL        = -9,
    XFS_ERR_NOMEM          = -10
};

/* Where a log replay's memory comes from (xfs_mount_journal): asked for
 * once, sized exactly, put back when the mount fails, and the caller's to
 * release after the last read of the volume, which reads through it. */
typedef struct {
    void *(*get)(void *ctx, size_t bytes);
    void  (*put)(void *ctx, void *mem, size_t bytes);
    void  *ctx;
} xfs_mem;

#define XFS_REPLAYED 0x1u       /* read through its replayed log */

#define XFS_WIN         4096u
#define XFS_NAME_MAX    255u
#define XFS_FORK_MAX    1872u   /* a 2048-byte inode's literal area */

typedef struct {
    xfs_read_fn read;
    void *ctx;

    uint32_t blocksize;
    uint8_t  blocklog;
    uint16_t sectsize;
    uint16_t inodesize;
    uint8_t  inopblog;
    uint8_t  agblklog;
    uint32_t agblocks;
    uint32_t agcount;
    uint64_t dblocks;
    uint8_t  ftype;             /* directory entries carry a file type */
    uint8_t  uuid[16];          /* the one metadata carries */
    uint64_t rootino;

    uint64_t log_off;           /* the internal log, bytes */
    uint64_t log_bbs;           /* in 512-byte basic blocks */

    uint32_t flags;             /* XFS_REPLAYED */

    /* A replayed log's overlay: 512-byte blocks, ovl_idx sorting them by
     * ovl_bb; read and ctx then read through it, the device's are kept. */
    xfs_read_fn dev_read;
    void    *dev_ctx;
    uint64_t *ovl_bb;
    uint32_t *ovl_idx;
    uint8_t  *ovl_data;
    uint32_t ovl_n;
    uint32_t ovl_cap;
    void    *mem;
    size_t   mem_len;
    void   (*mem_put)(void *ctx, void *mem, size_t bytes);
    void    *mem_ctx;

    /* The window: XFS_WIN bytes at win_off; ~0 invalid. */
    uint64_t win_off;
    uint8_t  win[XFS_WIN];
} xfs_vol;

/* The inode as the reader needs it: its data fork copied out. */
typedef struct {
    uint64_t ino;
    uint16_t mode;
    uint8_t  format;            /* 1 local, 2 extents, 3 btree */
    uint64_t size;
    uint64_t nextents;
    uint16_t fork_len;
    uint8_t  fork[XFS_FORK_MAX];
} xfs_inode;

typedef struct {
    uint8_t  name[XFS_NAME_MAX + 1]; /* NUL-terminated bytes */
    uint8_t  name_len;
    uint8_t  is_dir;
    uint8_t  is_regular;
    uint64_t ino;
} xfs_dirent;

typedef struct {
    xfs_vol  *vol;
    xfs_inode inode;
    /* Short form: the entry index and its byte in the fork. Block form:
     * the byte in the directory's data space, and where this block's
     * entries end. */
    uint64_t pos;
    uint32_t index;
    uint32_t blk_end;
    uint64_t blk_phys;          /* the block pos is in */
} xfs_dir;

typedef struct {
    xfs_vol  *vol;
    xfs_inode inode;
} xfs_file;

/* The magic at byte 0, one read. */
int xfs_probe(xfs_read_fn read, void *ctx);

/* The superblock, and the log's head. */
int xfs_mount(xfs_vol *v, xfs_read_fn read, void *ctx);

/* xfs_mount, except that a log that is not clean is replayed in memory,
 * never written: XFS_REPLAYED in v->flags. XFS_ERR_NOMEM with *need the
 * bytes mem was asked for; XFS_ERR_JOURNAL for a log that is damaged;
 * XFS_ERR_NEEDS_RECOVERY for one whose unfinished intents only the kernel
 * finishes. */
int xfs_mount_journal(xfs_vol *v, xfs_read_fn read, void *ctx,
                      const xfs_mem *mem, size_t *need);

int xfs_dir_open_root(xfs_vol *v, xfs_dir *d);
int xfs_dir_open(xfs_vol *v, xfs_dir *d, const xfs_dirent *e);
/* 1 = entry produced, 0 = end, negative = XFS_ERR_*. "." and ".." are not
 * produced. */
int xfs_dir_next(xfs_dir *d, xfs_dirent *out);

int xfs_file_open(xfs_vol *v, xfs_file *f, const xfs_dirent *e);
int xfs_file_read(xfs_file *f, uint64_t off, uint64_t len, void *buf);

#endif /* FALCON_FS_XFS_H */
