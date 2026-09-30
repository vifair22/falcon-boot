/* The ext4 journal, replayed in memory. jbd2.h argues what is accepted and
 * why; the passes below follow fs/jbd2/recovery.c's do_one_pass closely
 * enough that a difference from it should be read as a bug here. Every
 * journal structure is big-endian, unlike the filesystem it journals; the
 * fast-commit area alone is ext4's, and little-endian.
 */

#include <fs/jbd2.h>
#include <fs/crc32c.h>
#include <lib/string.h>

#define JBD2_MAGIC            0xC03B3998u

#define JBD2_DESCRIPTOR_BLOCK 1u
#define JBD2_COMMIT_BLOCK     2u
#define JBD2_SUPERBLOCK_V2    4u
#define JBD2_REVOKE_BLOCK     5u

/* Every block's header. */
#define JH_MAGIC      0x00u
#define JH_BLOCKTYPE  0x04u
#define JH_SEQUENCE   0x08u
#define JH_SIZE       12u

/* The journal superblock. */
#define JS_BLOCKSIZE  0x0Cu
#define JS_MAXLEN     0x10u
#define JS_FIRST      0x14u
#define JS_SEQUENCE   0x18u
#define JS_START      0x1Cu
#define JS_COMPAT     0x24u
#define JS_INCOMPAT   0x28u
#define JS_RO_COMPAT  0x2Cu
#define JS_UUID       0x30u
#define JS_CSUM_TYPE  0x50u
#define JS_NUM_FC     0x54u
#define JS_CHECKSUM   0xFCu
#define JS_SIZE       1024u

#define COMPAT_CHECKSUM      0x01u  /* v1: crc32 over each transaction */
#define INCOMPAT_REVOKE      0x01u
#define INCOMPAT_64BIT       0x02u
#define INCOMPAT_ASYNC       0x04u
#define INCOMPAT_CSUM_V2     0x08u
#define INCOMPAT_CSUM_V3     0x10u
#define INCOMPAT_FAST_COMMIT 0x20u
#define INCOMPAT_KNOWN (INCOMPAT_REVOKE | INCOMPAT_64BIT | INCOMPAT_ASYNC | \
                        INCOMPAT_CSUM_V2 | INCOMPAT_CSUM_V3 | \
                        INCOMPAT_FAST_COMMIT)
#define CSUM_TYPE_CRC32C     4u

/* The kernel's floor for a journal, and the fast-commit area it gives one
 * whose superblock names no size. */
#define MIN_JOURNAL_BLOCKS   1024u
#define DEFAULT_FC_BLOCKS    256u

/* A descriptor block's tags. Two layouts: csum v3's is four big-endian
 * words (block, flags, block high, checksum); the older one is block,
 * a 16-bit checksum, 16-bit flags, block high, with the checksum present
 * only under csum v2 and the high word only under 64bit. The flags land at
 * bytes 6-7 in both, which is how the kernel reads them with one struct. */
#define TAG_BLOCKNR   0x00u
#define TAG_CSUM16    0x04u
#define TAG_FLAGS16   0x06u
#define TAG_BLOCKNR_HI 0x08u
#define TAG3_CSUM     0x0Cu
#define TAG_UUID_SIZE 16u

#define FLAG_ESCAPE    0x1u
#define FLAG_SAME_UUID 0x2u
#define FLAG_LAST_TAG  0x8u

/* A commit block. */
#define JC_CHKSUM0    0x10u
#define JC_COMMIT_SEC 0x30u
#define JC_HDR_SIZE   0x3Cu  /* sizeof(struct commit_header) */

/* A revoke block. */
#define JR_COUNT      0x0Cu
#define JR_HDR_SIZE   0x10u

#define TAIL_SIZE     4u     /* jbd2_journal_block_tail */

/* The first record of a fast-commit block: ext4_fc_tl then ext4_fc_head. */
#define FC_TAG_HEAD   0x0009u
#define FC_HEAD_LEN   8u
#define FC_HEAD_TID   0x08u

/* jbd2_ent.loc: the block in the low 48 bits, flags above. */
#define LOC_BLK_MASK  0x0000FFFFFFFFFFFFull
#define LOC_ESCAPE    (1ull << 63)
#define LOC_DEAD      (1ull << 62)
#define LOC_VERIFIED  (1ull << 61)

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

/* The kernel's tid_geq: transaction IDs wrap, and are compared by the sign
 * of their difference. */
static int tid_geq(uint32_t x, uint32_t y)
{
    return (int32_t)(x - y) >= 0;
}

static int read_jblk(jbd2_log *j, uint32_t jblk, uint8_t *buf)
{
    uint64_t fsblk;

    if (jblk >= j->maxlen || j->map(j->map_ctx, jblk, &fsblk) != 0)
        return JBD2_ERR_IO;
    if (j->read(j->read_ctx, fsblk * j->bs, j->bs, buf) != 0)
        return JBD2_ERR_IO;
    return JBD2_OK;
}

/* The next log block after b, wrapping from the end of the log to its
 * start; the kernel's wrap(). */
static uint32_t log_next(const jbd2_log *j, uint32_t b, uint32_t by)
{
    uint64_t n = (uint64_t)b + by;

    while (n >= j->last)
        n -= (uint64_t)(j->last - j->first);
    return (uint32_t)n;
}

/* A descriptor or revoke block's tail checksum: the whole block with the
 * tail's four bytes zeroed. */
static int tail_ok(const jbd2_log *j, const uint8_t *b)
{
    uint32_t c;

    if (j->csum_ver == 0)
        return 1;
    c = crc32c(j->csum_seed, b, j->bs - TAIL_SIZE);
    c = crc32c_zeros(c, TAIL_SIZE);
    return c == be32(b + j->bs - TAIL_SIZE);
}

/* A commit block's checksum, the whole block with h_chksum[0] zeroed; or,
 * as the kernel also accepts, the same over the header alone with the rest
 * of the block taken as zeroes (jbd2_commit_block_csum_verify_partial: a
 * commit block written before the zeroing of its tail reached the disk). */
static int commit_ok(const jbd2_log *j, const uint8_t *b)
{
    uint32_t want;
    uint32_t c;

    if (j->csum_ver == 0)
        return 1;
    want = be32(b + JC_CHKSUM0);
    c = crc32c(j->csum_seed, b, JC_CHKSUM0);
    c = crc32c_zeros(c, 4);
    if (crc32c(c, b + JC_CHKSUM0 + 4u, j->bs - (JC_CHKSUM0 + 4u)) == want)
        return 1;
    c = crc32c(c, b + JC_CHKSUM0 + 4u, JC_HDR_SIZE - (JC_CHKSUM0 + 4u));
    return crc32c_zeros(c, j->bs - JC_HDR_SIZE) == want;
}

/* Walks a descriptor block's tags, as count_tags does: up to the one
 * flagged last, or to the end of the block (less its tail). With e non-NULL
 * each is recorded as the table entry for the log block that follows it,
 * starting at *b; either way *b ends past the last of them. Returns the tag
 * count, which is at most bs / 8. */
static uint32_t desc_tags(jbd2_log *j, const uint8_t *d, uint32_t seq,
                          uint32_t *b, jbd2_ent *e)
{
    uint32_t end = j->bs - (j->csum_ver != 0 ? TAIL_SIZE : 0u);
    uint32_t off = JH_SIZE;
    uint32_t n = 0;

    while (off + j->tag_bytes <= end) {
        const uint8_t *t = d + off;
        uint32_t flags = be16(t + TAG_FLAGS16);

        if (e != (jbd2_ent *)0) {
            jbd2_ent *x = &e[n];

            x->fsblk = be32(t + TAG_BLOCKNR);
            if ((j->incompat & INCOMPAT_64BIT) != 0)
                x->fsblk |= (uint64_t)be32(t + TAG_BLOCKNR_HI) << 32;
            x->loc = *b;
            if ((flags & FLAG_ESCAPE) != 0)
                x->loc |= LOC_ESCAPE;
            x->seq = seq;
            x->csum = (j->csum_ver == 3) ? be32(t + TAG3_CSUM)
                                         : be16(t + TAG_CSUM16);
        }
        n++;
        *b = log_next(j, *b, 1);
        off += j->tag_bytes;
        if ((flags & FLAG_SAME_UUID) == 0)
            off += TAG_UUID_SIZE;
        if ((flags & FLAG_LAST_TAG) != 0)
            break;
    }
    return n;
}

int jbd2_open(jbd2_log *j, jbd2_read_fn read, void *read_ctx, jbd2_map_fn map,
              void *map_ctx, uint32_t bs, uint32_t nblocks)
{
    const uint8_t *s;
    uint32_t compat;
    uint32_t ro;
    int rc;

    if (j == (jbd2_log *)0 || read == (jbd2_read_fn)0 ||
        map == (jbd2_map_fn)0 || bs < JS_SIZE || bs > JBD2_BLOCK_MAX ||
        (bs & (bs - 1u)) != 0)
        return JBD2_ERR_BAD_ARG;

    memset(j, 0, sizeof(*j));
    j->read = read;
    j->read_ctx = read_ctx;
    j->map = map;
    j->map_ctx = map_ctx;
    j->bs = bs;
    j->maxlen = nblocks;

    rc = read_jblk(j, 0, j->blk);
    if (rc != JBD2_OK)
        return rc;
    s = j->blk;

    if (be32(s + JH_MAGIC) != JBD2_MAGIC)
        return JBD2_ERR_CORRUPT;
    if (be32(s + JH_BLOCKTYPE) != JBD2_SUPERBLOCK_V2)
        return JBD2_ERR_UNSUPPORTED;
    if (be32(s + JS_BLOCKSIZE) != bs)
        return JBD2_ERR_UNSUPPORTED;

    compat = be32(s + JS_COMPAT);
    j->incompat = be32(s + JS_INCOMPAT);
    ro = be32(s + JS_RO_COMPAT);
    if ((j->incompat & ~INCOMPAT_KNOWN) != 0 || ro != 0)
        return JBD2_ERR_UNSUPPORTED;

    if ((j->incompat & INCOMPAT_CSUM_V2) != 0 &&
        (j->incompat & INCOMPAT_CSUM_V3) != 0)
        return JBD2_ERR_CORRUPT;
    if ((j->incompat & (INCOMPAT_CSUM_V2 | INCOMPAT_CSUM_V3)) != 0) {
        uint32_t c;

        j->csum_ver = ((j->incompat & INCOMPAT_CSUM_V3) != 0) ? 3u : 2u;
        if (s[JS_CSUM_TYPE] != CSUM_TYPE_CRC32C)
            return JBD2_ERR_UNSUPPORTED;
        c = crc32c(0xFFFFFFFFu, s, JS_CHECKSUM);
        c = crc32c_zeros(c, 4);
        c = crc32c(c, s + JS_CHECKSUM + 4u, JS_SIZE - (JS_CHECKSUM + 4u));
        if (c != be32(s + JS_CHECKSUM))
            return JBD2_ERR_CSUM;
        j->csum_seed = crc32c(0xFFFFFFFFu, s + JS_UUID, 16);
    } else if ((compat & COMPAT_CHECKSUM) != 0) {
        return JBD2_ERR_UNSUPPORTED;
    }

    /* journal_check_superblock's bounds. s_maxlen may be shorter than the
     * inode (the kernel takes the smaller), never longer. */
    if (be32(s + JS_MAXLEN) > nblocks || be32(s + JS_MAXLEN) == 0)
        return JBD2_ERR_CORRUPT;
    j->maxlen = be32(s + JS_MAXLEN);
    j->first = be32(s + JS_FIRST);
    if (j->first == 0 || j->first >= j->maxlen)
        return JBD2_ERR_CORRUPT;
    j->last = j->maxlen;

    if ((j->incompat & INCOMPAT_FAST_COMMIT) != 0) {
        uint32_t fc = be32(s + JS_NUM_FC);

        if (fc == 0)
            fc = DEFAULT_FC_BLOCKS;
        if (j->maxlen < MIN_JOURNAL_BLOCKS ||
            j->maxlen - MIN_JOURNAL_BLOCKS < fc)
            return JBD2_ERR_CORRUPT;
        j->last = j->maxlen - fc;
        /* The kernel starts the area one past the log's end
         * (journal.c: j_fc_first = j_last + 1). */
        j->fc_first = j->last + 1u;
    }
    if (j->first >= j->last)
        return JBD2_ERR_CORRUPT;

    if ((j->incompat & INCOMPAT_CSUM_V3) != 0)
        j->tag_bytes = 16u;
    else
        j->tag_bytes = 8u + ((j->csum_ver == 2) ? 2u : 0u) +
                       (((j->incompat & INCOMPAT_64BIT) != 0) ? 4u : 0u);

    j->seq0 = be32(s + JS_SEQUENCE);
    j->start = be32(s + JS_START);
    if (j->start == 0)
        return JBD2_EMPTY;
    if (j->start < j->first || j->start >= j->last)
        return JBD2_ERR_CORRUPT;
    return JBD2_OK;
}

/* PASS_SCAN. The log ends at the first block that is not the next expected
 * one: a wrong magic or sequence, an unknown block type, or a commit that
 * fails its checksum. A descriptor or revoke block that fails its own is
 * only suspicious here, as in the kernel: lazy journal init leaves stale
 * blocks from an older use of the journal in place, and whether this one is
 * stale or corrupt is settled by the commit time of what follows. */
int jbd2_scan(jbd2_log *j)
{
    uint32_t seq;
    uint32_t b;
    uint64_t last_time = 0;
    uint64_t ntags = 0;
    uint64_t steps = 0;
    int suspect = 0;
    int rc;

    if (j == (jbd2_log *)0 || j->start == 0)
        return JBD2_ERR_BAD_ARG;

    seq = j->seq0;
    b = j->start;
    for (;;) {
        const uint8_t *h = j->blk;
        uint32_t type;

        /* A log that never breaks its own chain would be walked forever;
         * nothing valid visits more blocks than the log holds. */
        if (++steps > j->last - j->first)
            return JBD2_ERR_CORRUPT;

        rc = read_jblk(j, b, j->blk);
        if (rc != JBD2_OK)
            return rc;
        b = log_next(j, b, 1);

        if (be32(h + JH_MAGIC) != JBD2_MAGIC || be32(h + JH_SEQUENCE) != seq)
            break;
        type = be32(h + JH_BLOCKTYPE);

        if (type == JBD2_DESCRIPTOR_BLOCK) {
            uint32_t n;

            if (!tail_ok(j, h))
                suspect = 1;
            n = desc_tags(j, h, seq, &b, (jbd2_ent *)0);
            ntags += n;
            steps += n;
        } else if (type == JBD2_COMMIT_BLOCK) {
            uint64_t t = be64(h + JC_COMMIT_SEC);

            if (suspect) {
                /* A suspect block followed by a commit that moves time
                 * forward belongs to this journal's own log: corruption.
                 * One that moves it back is a stale leftover. */
                if (t >= last_time)
                    return JBD2_ERR_CSUM;
                break;
            }
            /* A commit that fails its checksum ends the log, whether it is
             * stale (its time goes back) or the transaction a power cut
             * interrupted; async_commit only changes whether the kernel
             * keeps scanning to report it. */
            if (!commit_ok(j, h))
                break;
            last_time = t;
            seq++;
            if (ntags > 0xFFFFFFFFu)
                return JBD2_ERR_CORRUPT;
            j->ntags = (uint32_t)ntags;
        } else if (type == JBD2_REVOKE_BLOCK) {
            if (!tail_ok(j, h))
                suspect = 1;
        } else {
            break;
        }
    }
    j->end_seq = seq;

    /* Fast commits are live when the area opens with a head naming the
     * transaction after the last full commit, which is what the kernel's
     * ext4_fc_replay_scan expects to find. */
    if (j->fc_first != 0 && j->fc_first < j->maxlen) {
        rc = read_jblk(j, j->fc_first, j->blk);
        if (rc != JBD2_OK)
            return rc;
        if (get_le16(j->blk) == FC_TAG_HEAD &&
            get_le16(j->blk + 2) == FC_HEAD_LEN &&
            get_le32(j->blk + FC_HEAD_TID) == j->end_seq)
            j->fc_live = 1;
    }
    return JBD2_OK;
}

size_t jbd2_need(const jbd2_log *j)
{
    if (j == (const jbd2_log *)0)
        return 0;
    return (size_t)j->bs + (size_t)j->ntags * sizeof(jbd2_ent);
}

/* The table's order: by block, then by transaction, oldest first. Offsets
 * from seq0 rather than the IDs themselves, since the IDs wrap. */
static int ent_less(const jbd2_log *j, const jbd2_ent *a, const jbd2_ent *b)
{
    if (a->fsblk != b->fsblk)
        return a->fsblk < b->fsblk;
    return (a->seq - j->seq0) < (b->seq - j->seq0);
}

static void ent_swap(jbd2_ent *a, jbd2_ent *b)
{
    jbd2_ent t = *a;

    *a = *b;
    *b = t;
}

/* Heapsort: in place, no recursion, and n log n whatever the log holds. */
static void ent_sift(const jbd2_log *j, jbd2_ent *e, size_t root, size_t n)
{
    for (;;) {
        size_t c = 2u * root + 1u;

        if (c >= n)
            return;
        if (c + 1u < n && ent_less(j, &e[c], &e[c + 1u]))
            c++;
        if (!ent_less(j, &e[root], &e[c]))
            return;
        ent_swap(&e[root], &e[c]);
        root = c;
    }
}

static void ent_sort(const jbd2_log *j, jbd2_ent *e, size_t n)
{
    size_t i;

    if (n < 2u)
        return;
    for (i = n / 2u; i-- > 0;)
        ent_sift(j, e, i, n);
    for (i = n - 1u; i > 0; i--) {
        ent_swap(&e[0], &e[i]);
        ent_sift(j, e, 0, i);
    }
}

/* The first entry at or above fsblk. */
static size_t ent_lower(const jbd2_ent *e, size_t n, uint64_t fsblk)
{
    size_t lo = 0;
    size_t hi = n;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;

        if (e[mid].fsblk < fsblk)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

/* One walk over the replayed transactions. With e non-NULL it records every
 * tag (PASS_REPLAY's half that finds the blocks); with it NULL it applies
 * every revoke record to the sorted table in (PASS_REVOKE). The scan has
 * already found where the log ends, so anything unexpected before end_seq
 * means the journal changed under it, or is damaged. */
static int walk(jbd2_log *j, jbd2_ent *e, size_t cap, size_t *count,
                jbd2_ent *sorted, size_t nsorted)
{
    uint32_t seq = j->seq0;
    uint32_t b = j->start;
    uint64_t steps = 0;
    size_t n = 0;
    int rc;

    while (seq != j->end_seq) {
        const uint8_t *h = j->blk;
        uint32_t type;

        if (++steps > j->last - j->first)
            return JBD2_ERR_CORRUPT;
        rc = read_jblk(j, b, j->blk);
        if (rc != JBD2_OK)
            return rc;
        b = log_next(j, b, 1);
        if (be32(h + JH_MAGIC) != JBD2_MAGIC || be32(h + JH_SEQUENCE) != seq)
            return JBD2_ERR_CORRUPT;
        type = be32(h + JH_BLOCKTYPE);

        if (type == JBD2_DESCRIPTOR_BLOCK) {
            uint32_t k;

            if (!tail_ok(j, h))
                return JBD2_ERR_CSUM;
            if (e != (jbd2_ent *)0) {
                uint32_t probe = b;

                k = desc_tags(j, h, seq, &probe, (jbd2_ent *)0);
                if (k > cap - n)
                    return JBD2_ERR_CORRUPT;
                (void)desc_tags(j, h, seq, &b, e + n);
                n += k;
            } else {
                k = desc_tags(j, h, seq, &b, (jbd2_ent *)0);
            }
            steps += k;
        } else if (type == JBD2_COMMIT_BLOCK) {
            seq++;
        } else if (type == JBD2_REVOKE_BLOCK) {
            uint32_t used;
            uint32_t rec = ((j->incompat & INCOMPAT_64BIT) != 0) ? 8u : 4u;
            uint32_t off;

            if (e != (jbd2_ent *)0)
                continue;
            if (!tail_ok(j, h))
                return JBD2_ERR_CSUM;
            used = be32(h + JR_COUNT);
            if (used > j->bs - (j->csum_ver != 0 ? TAIL_SIZE : 0u))
                return JBD2_ERR_CORRUPT;
            /* A block revoked in transaction seq has its copies in seq and
             * every earlier transaction dropped (jbd2_journal_test_revoke:
             * revoked unless the copy's transaction is newer). */
            for (off = JR_HDR_SIZE; off + rec <= used; off += rec) {
                uint64_t blk = (rec == 8u) ? be64(h + off) : be32(h + off);
                size_t i = ent_lower(sorted, nsorted, blk);

                for (; i < nsorted && sorted[i].fsblk == blk; i++) {
                    if (tid_geq(seq, sorted[i].seq))
                        sorted[i].loc |= LOC_DEAD;
                }
            }
        } else {
            return JBD2_ERR_CORRUPT;
        }
    }
    if (count != (size_t *)0)
        *count = n;
    return JBD2_OK;
}

int jbd2_build(jbd2_log *j, void *mem, size_t len, uint64_t fs_blocks,
               jbd2_map *out)
{
    uint8_t *scratch = mem;
    jbd2_ent *e;
    size_t n = 0;
    size_t w = 0;
    size_t i;
    int rc;

    if (j == (jbd2_log *)0 || out == (jbd2_map *)0 || mem == (void *)0)
        return JBD2_ERR_BAD_ARG;
    memset(out, 0, sizeof(*out));
    if (len < jbd2_need(j))
        return JBD2_ERR_NOMEM;
    /* The scratch block first: bs is a power of two of at least 1024, so
     * the table after it keeps the caller's 8-byte alignment. */
    e = (jbd2_ent *)(void *)(scratch + j->bs);

    rc = walk(j, e, j->ntags, &n, (jbd2_ent *)0, 0);
    if (rc != JBD2_OK)
        return rc;
    ent_sort(j, e, n);
    /* Not gated on incompat revoke. jbd2 sets that bit in memory at the
     * first revoke and writes it to the journal superblock later, so a
     * journal left by a crash or a hibernation can hold revoke blocks the
     * superblock does not admit to yet; the kernel's scan_revoke_records
     * never asks. Skipping them serves a block's old copy after the block
     * was freed and reused, which the fixture caught. */
    rc = walk(j, (jbd2_ent *)0, 0, (size_t *)0, e, n);
    if (rc != JBD2_OK)
        return rc;

    /* Keep the newest live copy of each block, and turn its journal block
     * into the filesystem block the copy sits in. */
    for (i = 0; i < n;) {
        size_t k = i;
        size_t keep = n;

        for (; k < n && e[k].fsblk == e[i].fsblk; k++) {
            if ((e[k].loc & LOC_DEAD) == 0)
                keep = k;
        }
        if (keep != n) {
            jbd2_ent x = e[keep];
            uint64_t at;

            if (x.fsblk >= fs_blocks)
                return JBD2_ERR_CORRUPT;
            if (j->map(j->map_ctx, (uint32_t)(x.loc & LOC_BLK_MASK), &at) != 0)
                return JBD2_ERR_IO;
            if (at > LOC_BLK_MASK)
                return JBD2_ERR_CORRUPT;
            x.loc = at | (x.loc & LOC_ESCAPE);
            e[w++] = x;
        }
        i = k;
    }

    out->e = (w > 0) ? e : (jbd2_ent *)0;
    out->n = (uint32_t)w;
    out->bs = j->bs;
    out->scratch = scratch;
    out->read = j->read;
    out->read_ctx = j->read_ctx;
    out->csum_seed = j->csum_seed;
    out->csum_ver = j->csum_ver;
    return JBD2_OK;
}

/* A journal copy's tag checksum: the transaction ID, then the block as the
 * journal holds it (still escaped). Checked once per entry, into scratch. */
static int copy_verify(jbd2_map *m, jbd2_ent *x)
{
    uint8_t seq[4];
    uint32_t c;

    if (m->csum_ver == 0 || (x->loc & LOC_VERIFIED) != 0)
        return JBD2_OK;
    if (m->read(m->read_ctx, (x->loc & LOC_BLK_MASK) * m->bs, m->bs,
                m->scratch) != 0)
        return JBD2_ERR_IO;
    seq[0] = (uint8_t)(x->seq >> 24);
    seq[1] = (uint8_t)(x->seq >> 16);
    seq[2] = (uint8_t)(x->seq >> 8);
    seq[3] = (uint8_t)x->seq;
    c = crc32c(m->csum_seed, seq, sizeof seq);
    c = crc32c(c, m->scratch, m->bs);
    if (m->csum_ver == 2)
        c &= 0xFFFFu;
    if (c != x->csum)
        return JBD2_ERR_CSUM;
    x->loc |= LOC_VERIFIED;
    return JBD2_OK;
}

int jbd2_read(jbd2_map *m, uint64_t off, uint64_t len, void *buf)
{
    uint8_t *dst = buf;

    if (m == (jbd2_map *)0 || (buf == (void *)0 && len != 0))
        return JBD2_ERR_BAD_ARG;

    while (len > 0) {
        uint64_t blk = off / m->bs;
        uint32_t in = (uint32_t)(off % m->bs);
        size_t i = ent_lower(m->e, m->n, blk);
        uint64_t take;

        if (i < m->n && m->e[i].fsblk == blk) {
            jbd2_ent *x = &m->e[i];
            int rc = copy_verify(m, x);

            if (rc != JBD2_OK)
                return rc;
            take = m->bs - in;
            if (take > len)
                take = len;
            if (m->read(m->read_ctx, (x->loc & LOC_BLK_MASK) * m->bs + in,
                        take, dst) != 0)
                return JBD2_ERR_IO;
            /* An escaped copy began with the journal's magic, which the
             * journal stores zeroed so the block cannot be mistaken for
             * one of its own; put it back. */
            if ((x->loc & LOC_ESCAPE) != 0 && in < 4u) {
                static const uint8_t magic[4] = { 0xC0, 0x3B, 0x39, 0x98 };
                uint32_t k;

                for (k = in; k < 4u && k - in < take; k++)
                    dst[k - in] = magic[k];
            }
        } else {
            /* Straight from the device, up to the next block the journal
             * holds. */
            take = len;
            if (i < m->n) {
                uint64_t next = m->e[i].fsblk * m->bs;

                if (next - off < take)
                    take = next - off;
            }
            if (m->read(m->read_ctx, off, take, dst) != 0)
                return JBD2_ERR_IO;
        }
        dst += take;
        off += take;
        len -= take;
    }
    return JBD2_OK;
}
