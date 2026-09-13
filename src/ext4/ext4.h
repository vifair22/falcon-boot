/* falcon-boot: read-only ext4 (docs/DESIGN.md Sections 3, 9).
 *
 * Sibling of src/exfat/exfat.c and shaped the same way: freestanding, EFI-free,
 * all device access through a byte-addressed read callback, caller-allocated
 * contexts, no allocator. The same code reads a decrypted p3 in the loader and
 * a plain image file in host-side tests.
 *
 * Scope is p3 only (DESIGN Section 3): a ~1 GB filesystem that OUR mkstick
 * creates and that holds a couple of large UKIs plus the encrypted MOK private
 * key. Nothing on it is exotic, so this driver supports exactly the feature set
 * a stock `mkfs.ext4` produces on a 1 GiB partition and refuses everything else
 * rather than guessing.
 *
 *   Supported, verified against mke2fs 1.47.4 defaults on a 1 GiB image:
 *     compat    has_journal ext_attr resize_inode dir_index orphan_file
 *               (compat bits never change how data is read; all ignored)
 *     incompat  filetype extent 64bit flex_bg metadata_csum_seed
 *     ro_compat sparse_super large_file huge_file dir_nlink extra_isize
 *               metadata_csum
 *
 *   Refused with EXT4_ERR_UNSUPPORTED: every other incompat or ro_compat bit,
 *   including meta_bg, bigalloc, inline_data, encrypt, casefold, largedir,
 *   ea_inode, mmp and journal_dev.
 *
 *   Refused as not-cleanly-shut-down, which is a different answer because it
 *   describes the filesystem rather than the format: incompat recover as
 *   EXT4_ERR_NEEDS_RECOVERY, and ro_compat orphan_present or an unclean or
 *   error-flagged s_state as EXT4_ERR_DIRTY. A read-only driver that ignored
 *   any of them would hand back stale pre-journal data, which is worse than
 *   refusing to boot. The three are genuinely independent: an image snapshotted
 *   mid-write carries recover and orphan_present while s_state still reads
 *   clean, so s_state on its own would miss it.
 *
 * Journal contents are never read. That is sound only because a needs-recovery
 * filesystem is refused outright; on a cleanly unmounted one the journal holds
 * nothing the main filesystem does not already have.
 *
 * Checksum policy (metadata_csum, crc32c). Verified on read:
 *     superblock          yes  (seed ~0, covers the first 0x3FC bytes)
 *     group descriptor    yes  (per descriptor actually used)
 *     inode               yes  (i_checksum_lo/hi, per inode read)
 *     extent tree node    yes  (nodes read off disk; the 60-byte root inside
 *                               the inode has no tail and is covered by the
 *                               inode's own checksum)
 *     directory block     yes, when the block carries an ext4_dir_entry_tail.
 *                              htree interior blocks (the root's ".." entry
 *                              spans to the end of the block, and dx_node
 *                              blocks are one fake entry) carry a dx_tail
 *                              instead, which is NOT verified: it hashes the
 *                              index entries this driver deliberately never
 *                              interprets. Telling the two apart needs the
 *                              record chain, not the tail's signature; see
 *                              dir_block_has_tail() before touching it.
 *     bitmaps, journal    not read at all, so not verified.
 * When metadata_csum is absent nothing is verified and the mount still
 * succeeds; every image mkstick creates has it.
 *
 * Deliberate non-features, each with its own status so a caller can say why:
 *   - htree hash lookup. Indexed directories are walked linearly, which is
 *     valid for any ext4 directory: the interior blocks are shaped as empty
 *     directory entries precisely so that a linear reader skips them.
 *   - inline_data inodes (EXT4_ERR_INLINE), symlinks (EXT4_ERR_SYMLINK),
 *     sparse holes and uninitialized extents (EXT4_ERR_HOLE), and the old
 *     indirect block map (EXT4_ERR_UNSUPPORTED).
 */

#ifndef FALCON_EXT4_H
#define FALCON_EXT4_H

#include <stdint.h>
#include <stddef.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    EXT4_OK                 = 0,
    EXT4_ERR_IO             = -1,  /* read callback failed */
    EXT4_ERR_BAD_SUPER      = -2,  /* not ext4 / superblock unusable */
    EXT4_ERR_CORRUPT        = -3,  /* on-disk structure violates the format */
    EXT4_ERR_BAD_ARG        = -4,
    EXT4_ERR_RANGE          = -5,  /* read outside the file's size */
    EXT4_ERR_UNSUPPORTED    = -6,  /* feature outside the supported set */
    EXT4_ERR_NEEDS_RECOVERY = -7,  /* journal replay pending; would read stale */
    EXT4_ERR_DIRTY          = -8,  /* not cleanly unmounted, or errors flagged */
    EXT4_ERR_CSUM           = -9,  /* metadata_csum mismatch */
    EXT4_ERR_INLINE         = -10, /* data lives in the inode; unsupported */
    EXT4_ERR_SYMLINK        = -11, /* symlink: not a byte stream to read */
    EXT4_ERR_HOLE           = -12, /* sparse hole or uninitialized extent */
};

/* Read len bytes at byte offset off (volume-relative). Returns 0 on success,
 * nonzero on failure. Every offset and length handed to this callback is a
 * multiple of 512: metadata is fetched a filesystem block at a time and file
 * data in whole blocks, the sole exception being the 1024-byte superblock read
 * at offset 1024. */
typedef int (*ext4_read_fn)(void *ctx, uint64_t off, uint64_t len, void *buf);

/* 1 KiB, 2 KiB and 4 KiB blocks are accepted; mkfs picks 4 KiB for a 1 GiB
 * p3, and the 1 KiB case (where the superblock is block 1 and the descriptor
 * table block 2) is exercised by the host tests because it is the only one
 * where s_first_data_block is not zero. */
#define EXT4_BLOCK_MAX 4096u
#define EXT4_NAME_MAX  255u   /* bytes, format limit */
#define EXT4_ROOT_INO  2u

typedef struct {
    ext4_read_fn read;
    void *read_ctx;

    uint32_t block_size;
    uint32_t inode_size;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t first_data_block;
    uint32_t desc_size;        /* 32, or 64 with the 64bit feature */
    uint32_t inodes_count;
    uint32_t group_count;
    uint64_t blocks_count;
    uint64_t gdt_off;          /* byte offset of the group descriptor table */

    uint32_t csum_seed;        /* crc32c seed for per-inode/per-group sums */
    uint8_t  has_csum;         /* metadata_csum present */

    /* One-descriptor cache; ~0 marks it invalid. */
    uint32_t gd_cache_group;
    uint8_t  gd_cache[64];

    /* One-block metadata cache (inode table and extent tree nodes), keyed by
     * volume byte offset so it stays correct however callers interleave; ~0
     * marks it invalid. */
    uint64_t blk_cache_off;
    uint8_t  blk_cache[EXT4_BLOCK_MAX];
} ext4_vol;

/* The parts of an on-disk inode this driver acts on, plus the cursor that
 * makes a sequential read cheap. One resolved extent covers up to 128 MiB with
 * 4 KiB blocks, so walking a UKI end to end costs a couple of tree descents in
 * total rather than one per block. */
typedef struct {
    uint32_t ino;
    uint32_t generation;
    uint32_t flags;
    uint16_t mode;
    uint64_t size;
    uint32_t csum_seed;        /* fs seed folded with ino and generation */
    uint8_t  root[60];         /* i_block: the extent tree root, verbatim */

    /* Last resolved extent; ext_len 0 means the cursor is empty. */
    uint32_t ext_lblk;
    uint32_t ext_len;
    uint64_t ext_pblk;
} ext4_inode;

typedef struct {
    uint8_t  name[EXT4_NAME_MAX + 1]; /* NUL-terminated; bytes, not UCS-2 */
    uint8_t  name_len;
    uint8_t  is_dir;
    uint8_t  is_regular;
    uint8_t  file_type;               /* raw ext4 type, so a caller can tell a
                                         symlink from a device node */
    uint32_t ino;
} ext4_dirent;

typedef struct {
    ext4_vol *vol;
    ext4_inode inode;
    uint64_t pos;                     /* byte position in the directory */
    uint32_t buf_lblk;                /* logical block in buf; ~0 invalid */
    uint8_t  buf[EXT4_BLOCK_MAX];
} ext4_dir;

typedef struct {
    ext4_vol *vol;
    ext4_inode inode;
    /* One-block cache, for the ragged ends of an unaligned request; keyed by
     * volume byte offset, ~0 invalid. */
    uint64_t buf_off;
    uint8_t  buf[EXT4_BLOCK_MAX];
} ext4_file;

/* Validates the superblock, the feature set and the clean-unmount state. */
int ext4_mount(ext4_vol *v, ext4_read_fn read, void *read_ctx);

int ext4_dir_open_root(ext4_vol *v, ext4_dir *d);
int ext4_dir_open(ext4_vol *v, ext4_dir *d, const ext4_dirent *e);
int ext4_dir_open_ino(ext4_vol *v, ext4_dir *d, uint32_t ino);
/* 1 = entry produced, 0 = end of directory, negative = error. "." and ".."
 * are never produced, and neither are the empty entries that pad a directory
 * block or stand in for an htree interior node. */
int ext4_dir_next(ext4_dir *d, ext4_dirent *out);

int ext4_file_open(ext4_vol *v, ext4_file *f, const ext4_dirent *e);
int ext4_file_open_ino(ext4_vol *v, ext4_file *f, uint32_t ino);
/* Reads len bytes at file offset off. Wholly inside the file or nothing: a
 * short read is never reported as success, and anything past i_size is
 * EXT4_ERR_RANGE. */
int ext4_file_read(ext4_file *f, uint64_t off, uint64_t len, void *buf);
uint64_t ext4_file_size(const ext4_file *f);

#endif /* FALCON_EXT4_H */
