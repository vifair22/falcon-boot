/* falcon-boot: the ext4 journal (jbd2), replayed in memory and never written.
 *
 * Why this exists. A shelf's OS mounts it read-write, and while it is mounted
 * the superblock carries incompat RECOVER: the journal may hold committed
 * blocks the filesystem proper does not. Hibernation leaves it that way every
 * time (the OS is asleep, not unmounted) and so does a power cut. A reader
 * that ignored the journal would hand back stale metadata, so ext4.c used to
 * refuse such a shelf outright, which left a hibernated machine unable to
 * boot the very UKI that would resume it.
 *
 * What it does instead. The journal's committed transactions are read the way
 * the kernel's own recovery reads them (fs/jbd2/recovery.c: a scan pass that
 * finds where the valid log ends, then the revoke records, then the blocks),
 * but the result is a table rather than writes: for every filesystem block
 * the log holds a newer copy of, where that copy is. ext4.c then reads the
 * volume through jbd2_read, which serves a listed block from the journal and
 * everything else from the device. Nothing is written, so the OS that resumes
 * finds the disk exactly as it left it, and its own recovery (or its resumed
 * page cache) is not disturbed.
 *
 * What is accepted, and what is not, with the kernel's reasoning:
 *
 *   A torn last transaction (no commit block, or one whose checksum fails
 *   after the previous transaction's commit time) is where the log ends. It
 *   is normal after a power cut and is not an error: jbd2 guarantees the
 *   state at the last good commit is consistent.
 *
 *   A checksum failure inside a transaction that did commit is corruption
 *   and is JBD2_ERR_CSUM. Stopping before it is not an option: later
 *   transactions may already be partly checkpointed into place, so the
 *   older view would not be consistent. A data block's own tag checksum is
 *   checked when that block is first read through jbd2_read, not at mount,
 *   so a copy that is superseded or revoked never costs a read.
 *
 *   Fast commits (incompat fast_commit) are ext4's own logical records, not
 *   block copies, and are not replayed. The state at the last full commit is
 *   what is served. That is consistent: a block freed after a full commit is
 *   only released for reuse by the next full commit (ext4_journal_commit_
 *   callback runs from jbd2's full-commit path alone, never after a fast
 *   commit), so no block the older view names has been overwritten.
 *   Hibernation never leaves a live fast commit, since its sync forces a
 *   full commit; a power cut after an fsync can. jbd2_log.fc_live says so,
 *   and the loader says it on the page.
 *
 *   Refused as unsupported: v1 journal checksums (compat checksum without
 *   csum v2/v3, which no current kernel writes), unknown incompat or
 *   ro_compat bits, a version 1 journal superblock, and a journal block size
 *   other than the filesystem's (an internal journal always matches).
 *
 * Memory. The table is one entry per block tag in the transactions replayed,
 * counted by the scan, plus one block of scratch for verifying a journal
 * block's checksum; jbd2_need says how many bytes. The caller supplies them
 * (ext4_mount_journal's jbd2_mem), since no allocator exists in the loaders
 * this runs in, and keeps them for as long as the volume is read.
 *
 * All device access goes through the same byte-addressed read callback the
 * ext4 driver uses; the journal is an inode of the filesystem it journals, so
 * the caller also supplies the map from a journal block to a filesystem
 * block (its extent tree), which this module never walks itself.
 */

#ifndef FALCON_JBD2_H
#define FALCON_JBD2_H

#include <stdint.h>
#include <stddef.h>

enum {
    JBD2_OK              = 0,
    JBD2_ERR_IO          = -1, /* read callback or block map failed */
    JBD2_ERR_CORRUPT     = -2, /* the journal violates its format */
    JBD2_ERR_UNSUPPORTED = -3, /* a journal feature outside the set above */
    JBD2_ERR_CSUM        = -4, /* a committed transaction fails its checksum */
    JBD2_ERR_NOMEM       = -5, /* the caller's memory is short of jbd2_need */
    JBD2_ERR_BAD_ARG     = -6
};

/* jbd2_open's answer for a journal with nothing in it (s_start zero): the
 * last unmount, or the kernel's own recovery, emptied it. */
#define JBD2_EMPTY 1

#define JBD2_BLOCK_MAX 4096u

typedef int (*jbd2_read_fn)(void *ctx, uint64_t off, uint64_t len, void *buf);

/* Journal block jblk (0 is the journal superblock) to the filesystem block
 * that holds it. 0 on success, nonzero when jblk is unmapped. */
typedef int (*jbd2_map_fn)(void *ctx, uint32_t jblk, uint64_t *fsblk);

/* Where the caller's memory for the table comes from. get returns bytes,
 * 8-byte aligned, or NULL when it cannot be had; put takes it back and may
 * be NULL. The UEFI loader answers with AllocatePages, falcon-bios with its
 * heap, a host tool with malloc. */
typedef struct {
    void *(*get)(void *ctx, size_t bytes);
    void  (*put)(void *ctx, void *mem, size_t bytes);
    void  *ctx;
} jbd2_mem;

/* One block the log holds a copy of. `loc` is private: a journal block
 * while the table is built, the filesystem block of the copy afterwards,
 * with flags in its top bits. */
typedef struct {
    uint64_t fsblk;
    uint64_t loc;
    uint32_t seq;   /* the transaction that wrote the copy */
    uint32_t csum;  /* its tag checksum, checked on first read */
} jbd2_ent;

/* The journal while it is being read. Large (a block of scratch), and only
 * needed during the mount, so ext4_mount_journal keeps it on its stack. */
typedef struct {
    jbd2_read_fn read;
    void *read_ctx;
    jbd2_map_fn map;
    void *map_ctx;
    uint32_t bs;          /* block size, the filesystem's */

    /* From the journal superblock. The log proper is blocks [first, last);
     * a fast-commit area, when there is one, follows it. */
    uint32_t maxlen;
    uint32_t first;
    uint32_t last;
    uint32_t start;       /* s_start: where the oldest live transaction is */
    uint32_t seq0;        /* s_sequence: that transaction's ID */
    uint32_t incompat;
    uint32_t csum_seed;
    uint32_t tag_bytes;
    uint32_t fc_first;    /* 0 when the journal has no fast-commit area */
    uint8_t  csum_ver;    /* 0, 2 or 3 */

    /* From jbd2_scan. */
    uint32_t end_seq;     /* the first transaction NOT replayed */
    uint32_t ntags;       /* block tags in the replayed transactions */
    uint8_t  fc_live;     /* fast commits newer than end_seq - 1 were left */

    uint8_t  blk[JBD2_BLOCK_MAX];
} jbd2_log;

/* The replayed view: what jbd2_read serves. Lives inside the ext4 volume
 * for as long as it is mounted; e is NULL when there is nothing to serve. */
typedef struct {
    jbd2_ent *e;
    uint32_t n;
    uint32_t bs;
    uint8_t *scratch;     /* one block, from the caller's memory */
    jbd2_read_fn read;
    void *read_ctx;
    uint32_t csum_seed;
    uint8_t  csum_ver;
} jbd2_map;

/* Reads and checks the journal superblock. nblocks is the journal inode's
 * length in blocks. JBD2_OK when there is a log to replay, JBD2_EMPTY when
 * there is not, otherwise an error. */
int jbd2_open(jbd2_log *j, jbd2_read_fn read, void *read_ctx, jbd2_map_fn map,
              void *map_ctx, uint32_t bs, uint32_t nblocks);

/* Finds where the valid log ends and counts what the table will hold. */
int jbd2_scan(jbd2_log *j);

/* The bytes of memory jbd2_build needs, after jbd2_scan. */
size_t jbd2_need(const jbd2_log *j);

/* Builds the table into mem (len bytes, at least jbd2_need) and fills out.
 * Every block the table lists is below fs_blocks, or JBD2_ERR_CORRUPT. */
int jbd2_build(jbd2_log *j, void *mem, size_t len, uint64_t fs_blocks,
               jbd2_map *out);

/* The filesystem as replayed: len bytes at volume offset off, both
 * multiples of 512, from the journal where the table lists a block and from
 * the device everywhere else. JBD2_ERR_CSUM when a journal copy fails its
 * tag checksum. */
int jbd2_read(jbd2_map *m, uint64_t off, uint64_t len, void *buf);

#endif /* FALCON_JBD2_H */
