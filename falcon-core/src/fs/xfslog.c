/* falcon-boot: an xfs log replayed in memory, never written (DESIGN
 * Section 5.4).
 *
 * The kernel's recovery (fs/xfs/xfs_log_recover.c and the item recovery
 * files), for what this reader reads. The log from the last record's tail
 * to the head is walked four times, every walk streaming records through
 * the window with nothing held but the overlay:
 *
 *   1. count the transactions, the buffer cancels, the intents and an
 *      upper bound on the 512-byte blocks the replay can touch, which is
 *      what the one request for memory is sized by;
 *   2. note which transactions committed (only those are replayed);
 *   3. the committed ones' buffer cancels, and their bmap and exchange
 *      intents with the done items that finish them;
 *   4. replay buffer and inode items into the overlay.
 *
 * A buffer item is its dirty 128-byte chunks; an inode item is the inode
 * core in the log's own (host) byte order, converted to the disk's, and
 * the data fork. On v5 a block or inode whose own LSN is past the
 * transaction's is newer on disk and left alone. Intents to free extents
 * and to update the rmap and refcount trees change nothing this reader
 * reads and are not finished; an unfinished bmap or exchange intent
 * changes what a file maps, and the volume is refused instead
 * (XFS_ERR_NEEDS_RECOVERY). Inode buffers (unlinked lists) and dquot
 * buffers are not replayed either.
 *
 * Transactions are applied in the order their items reach the log, which
 * is commit order for the checkpoints a kernel writes. Log items are in the
 * writer's byte order; this reads little-endian ones, as both loaders' and
 * the host tools' machines are.
 */

#include <fs/xfs.h>
#include <fs/xfsint.h>
#include <fs/crc32c.h>

#include <lib/string.h>

#define BB             512u
#define LOG_MAGIC      0xFEEDBABEu
#define REC_HDR_LEN    328u   /* sizeof the header struct: 324, padded to 8 */
#define REC_CRC        32u
#define EXT_HDR_LEN    260u
#define HDR_BBS_MAX    8u
#define CYCLE_WORDS    64u
#define OP_HDR         12u
/* A torn record within this many blocks of the head is the head. */
#define LOG_TORN       4096u

#define OP_START    0x01u
#define OP_COMMIT   0x02u
#define OP_CONTINUE 0x04u
#define OP_WAS_CONT 0x08u
#define OP_UNMOUNT  0x20u

#define LI_INODE 0x123bu
#define LI_BUF   0x123cu
#define LI_BUI   0x1244u
#define LI_BUD   0x1245u
#define LI_XMI   0x1248u
#define LI_XMD   0x1249u

#define BLF_INODE_BUF 0x1u
#define BLF_CANCEL    0x4u
#define BLF_DQUOT     (0x8u | 0x10u | 0x20u)
#define BLF_CHUNK     128u
#define BLF_MAP_MAX   16u

#define ILOG_DFORK  (0x2u | 0x4u | 0x8u)
#define ILOG_DBROOT 0x8u
#define DIFLAG2_BIGTIME 0x8u

#define IN_MAGIC  0x494eu
#define IN_CORE   176u
#define BMBT_HDR  72u

#define OPEN_MAX  16u
#define FMT_MAX   128u
#define BROOT_MAX 2048u

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)(v >> 16));
    put16(p + 2, (uint16_t)v);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

/* ---- the overlay ---- */

static uint32_t ovl_slot(const xfs_vol *v, uint64_t bb, int *found)
{
    uint32_t lo = 0;
    uint32_t hi = v->ovl_n;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;

        if (v->ovl_bb[v->ovl_idx[mid]] < bb)
            lo = mid + 1u;
        else
            hi = mid;
    }
    *found = lo < v->ovl_n && v->ovl_bb[v->ovl_idx[lo]] == bb;
    return lo;
}

int xfslog_covers(const xfs_vol *v, uint64_t at, uint32_t len)
{
    int found;
    uint32_t i;

    if (v->ovl_n == 0)
        return 0;
    i = ovl_slot(v, at / BB, &found);
    return i < v->ovl_n && v->ovl_bb[v->ovl_idx[i]] * BB < at + len;
}

/* The device's bytes with the overlay laid over them: every read of a
 * replayed volume, the window's included. */
static int ovl_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    xfs_vol *v = ctx;
    uint8_t *out = buf;
    int found;
    uint32_t i;

    if (v->dev_read(v->dev_ctx, off, len, buf) != 0)
        return -1;
    for (i = ovl_slot(v, off / BB, &found); i < v->ovl_n; i++) {
        uint32_t e = v->ovl_idx[i];
        uint64_t at = v->ovl_bb[e] * BB;
        uint64_t lo = at > off ? at : off;
        uint64_t hi = at + BB < off + len ? at + BB : off + len;

        if (at >= off + len)
            break;
        if (lo < hi)
            memcpy(out + (lo - off), v->ovl_data + (size_t)e * BB + (lo - at),
                   (size_t)(hi - lo));
    }
    return 0;
}

/* The overlay's copy of block bb, made from the device's when it has
 * none. */
static uint8_t *ovl_get(xfs_vol *v, uint64_t bb)
{
    int found;
    uint32_t i = ovl_slot(v, bb, &found);
    uint32_t e;

    if (found)
        return v->ovl_data + (size_t)v->ovl_idx[i] * BB;
    if (v->ovl_n == v->ovl_cap)
        return (uint8_t *)0;
    e = v->ovl_n;
    if (v->dev_read(v->dev_ctx, bb * BB, BB, v->ovl_data + (size_t)e * BB) != 0)
        return (uint8_t *)0;
    v->ovl_bb[e] = bb;
    memmove(v->ovl_idx + i + 1, v->ovl_idx + i,
            (size_t)(v->ovl_n - i) * sizeof v->ovl_idx[0]);
    v->ovl_idx[i] = e;
    v->ovl_n++;
    return v->ovl_data + (size_t)e * BB;
}

static int ovl_write(xfs_vol *v, uint64_t at, const uint8_t *src, uint32_t n)
{
    v->win_off = ~(uint64_t)0;
    while (n > 0) {
        uint8_t *b = ovl_get(v, at / BB);
        uint32_t o = (uint32_t)(at % BB);
        uint32_t k = BB - o < n ? BB - o : n;

        if (b == (uint8_t *)0)
            return XFS_ERR_NOMEM;
        memcpy(b + o, src, k);
        at += k;
        src += k;
        n -= k;
    }
    return XFS_OK;
}

/* ---- records ---- */

typedef struct {
    uint64_t bb;
    uint32_t hdr_bbs;
    uint32_t len;
    uint32_t nops;
    uint64_t lsn;
    uint64_t tail_lsn;
    uint8_t  hdr[HDR_BBS_MAX * BB];
} lrec;

/* len bytes from byte off of the log's block bb on, the log wrapping. */
static int log_read(xfs_vol *v, uint64_t bb, uint64_t off, uint32_t len,
                    uint8_t *out)
{
    while (len > 0) {
        uint64_t b = (bb + off / BB) % v->log_bbs;
        uint32_t o = (uint32_t)(off % BB);
        uint32_t k = BB - o < len ? BB - o : len;
        int rc = xfs_rd(v, v->log_off + b * BB + o, k, out);

        if (rc != XFS_OK)
            return rc;
        off += k;
        out += k;
        len -= k;
    }
    return XFS_OK;
}

static int rec_open(xfs_vol *v, uint64_t bb, lrec *r)
{
    uint8_t chunk[BB];
    uint32_t crc;
    uint32_t size;
    uint32_t k;
    int rc;

    r->bb = bb;
    rc = log_read(v, bb, 0, BB, r->hdr);
    if (rc != XFS_OK)
        return rc;
    if (be32(r->hdr) != LOG_MAGIC || (be32(r->hdr + 8) & 2u) == 0)
        return XFS_ERR_JOURNAL;
    size = be32(r->hdr + 320);
    r->hdr_bbs = size > 32768u ? (size + 32767u) / 32768u : 1u;
    r->len = be32(r->hdr + 12);
    r->nops = be32(r->hdr + 40);
    r->lsn = be64(r->hdr + 16);
    r->tail_lsn = be64(r->hdr + 24);
    if (r->hdr_bbs > HDR_BBS_MAX ||
        r->len > r->hdr_bbs * CYCLE_WORDS * BB)
        return XFS_ERR_JOURNAL;
    if (r->hdr_bbs > 1u) {
        rc = log_read(v, bb + 1u, 0, (r->hdr_bbs - 1u) * BB, r->hdr + BB);
        if (rc != XFS_OK)
            return rc;
    }

    /* The record's crc32c: its header (the checksum as zeros), each
     * extended header, and the data as it lies, cycle-stamped. */
    crc = crc32c(0xFFFFFFFFu, r->hdr, REC_CRC);
    crc = crc32c_zeros(crc, 4);
    crc = crc32c(crc, r->hdr + REC_CRC + 4u, REC_HDR_LEN - REC_CRC - 4u);
    for (k = 1; k < r->hdr_bbs; k++)
        crc = crc32c(crc, r->hdr + (size_t)k * BB, EXT_HDR_LEN);
    for (k = 0; k < r->len; k += BB) {
        uint32_t n = r->len - k < BB ? r->len - k : BB;

        rc = log_read(v, bb + r->hdr_bbs, k, n, chunk);
        if (rc != XFS_OK)
            return rc;
        crc = crc32c(crc, chunk, n);
    }
    return (crc ^ 0xFFFFFFFFu) == le32(r->hdr + REC_CRC) ? XFS_OK
                                                         : XFS_ERR_CSUM;
}

/* Bytes of the record's data with each block's first word put back: the
 * writer stamped it with the cycle and kept it in the headers. */
static int rec_data(xfs_vol *v, const lrec *r, uint32_t off, uint32_t len,
                    uint8_t *out)
{
    uint32_t j;
    int rc;

    if (off > r->len || len > r->len - off)
        return XFS_ERR_JOURNAL;
    if (len == 0)
        return XFS_OK;
    rc = log_read(v, r->bb + r->hdr_bbs, off, len, out);
    if (rc != XFS_OK)
        return rc;
    for (j = off / BB; j <= (off + len - 1u) / BB; j++) {
        uint32_t s = j * BB;
        const uint8_t *w = j < CYCLE_WORDS
                               ? r->hdr + 44u + (size_t)j * 4u
                               : r->hdr + (size_t)(j / CYCLE_WORDS) * BB + 4u +
                                     (size_t)(j % CYCLE_WORDS) * 4u;
        uint32_t x;

        for (x = s; x < s + 4u; x++) {
            if (x >= off && x < off + len)
                out[x - off] = w[x - s];
        }
    }
    return XFS_OK;
}

/* ---- transactions ---- */

enum { ROLE_HDR, ROLE_FMT, ROLE_REG };

typedef struct {
    uint8_t  used;
    uint8_t  apply;
    uint8_t  have_hdr;
    uint8_t  role;
    uint32_t tid;
    uint64_t key_lsn;           /* the START op: its record's LSN, */
    uint32_t key_op;            /* and its index there */
    uint64_t lsn;

    uint16_t type;              /* the item */
    uint16_t total;
    uint16_t idx;
    uint32_t reg_off;
    uint8_t  fmt[FMT_MAX];
    uint32_t fmt_len;
    uint8_t  skip;

    uint64_t blkno;             /* a buffer item */
    uint32_t map[BLF_MAP_MAX];
    uint32_t map_bits;
    uint32_t bit;
    uint32_t run_start;
    uint32_t run_len;

    uint64_t at;                /* an inode item */
    uint32_t fields;
    uint32_t dsize;
    uint32_t dfork;
    uint8_t  core[IN_CORE];
} trans;

typedef struct {
    xfs_vol *v;
    int      pass;              /* 1..4 */
    trans    open[OPEN_MAX];

    /* pass 1's counts */
    uint32_t nstart;
    uint32_t ncancel;
    uint32_t nint;
    uint64_t nbb;

    /* the arena's tables */
    struct start { uint64_t lsn; uint32_t op; uint32_t pad; } *committed;
    uint32_t ncommitted;
    struct cancel { uint64_t blkno; uint32_t len; uint32_t ref; } *cancels;
    uint32_t ncancels;
    uint64_t *intents;
    uint32_t nintents;

    uint8_t  broot[BROOT_MAX];
} replay;

static int committed_has(const replay *p, uint64_t lsn, uint32_t opi)
{
    uint32_t i;

    for (i = 0; i < p->ncommitted; i++) {
        if (p->committed[i].lsn == lsn && p->committed[i].op == opi)
            return 1;
    }
    return 0;
}

static struct cancel *cancel_find(replay *p, uint64_t blkno, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < p->ncancels; i++) {
        if (p->cancels[i].blkno == blkno && p->cancels[i].len == len)
            return &p->cancels[i];
    }
    return (struct cancel *)0;
}

/* The LSN a metadata block carries, by its kind; 0 when it is a kind this
 * reader never reads, whose replay is then unconditional. */
static int block_lsn(xfs_vol *v, uint64_t at, uint64_t *lsn)
{
    uint8_t h[248];
    uint32_t magic;
    int rc = xfs_rd(v, at, sizeof h, h);

    if (rc != XFS_OK)
        return rc;
    magic = be32(h);
    if (magic == 0x58465342u)                        /* superblock */
        *lsn = be64(h + 240);
    else if (magic == 0x58444233u || magic == 0x58444433u)  /* dir data */
        *lsn = be64(h + 16);
    else if (magic == 0x424d4133u)                   /* bmbt */
        *lsn = be64(h + 32);
    else if (be16(h) == IN_MAGIC && h[4] >= 3u)      /* an inode cluster */
        *lsn = be64(h + 112);
    else
        *lsn = 0;
    return XFS_OK;
}

static int newer_on_disk(uint64_t disk, uint64_t lsn)
{
    return disk != 0 && disk != ~(uint64_t)0 && disk > lsn;
}

static uint32_t chunk_bbs(const uint32_t *map, uint32_t bits)
{
    uint32_t b;
    uint32_t n = 0;
    uint32_t last = ~0u;

    for (b = 0; b < bits; b++) {
        if ((map[b / 32u] >> (b % 32u)) & 1u) {
            if (b * BLF_CHUNK / BB != last)
                n++;
            last = b * BLF_CHUNK / BB;
        }
    }
    return n;
}

/* The item's format region is whole: what it is and what the pass does
 * with it. */
static int item_start(replay *p, trans *t)
{
    xfs_vol *v = p->v;
    const uint8_t *f = t->fmt;

    if (t->fmt_len < 4u)
        return XFS_ERR_JOURNAL;
    t->type = le16(f);
    t->total = le16(f + 2);
    t->idx = 0;
    t->skip = 1;
    if (t->total == 0)
        return XFS_ERR_JOURNAL;

    if (t->type == LI_BUF) {
        uint16_t flags;
        uint32_t words;
        uint32_t i;

        if (t->fmt_len < 20u)
            return XFS_ERR_JOURNAL;
        flags = le16(f + 4);
        t->blkno = le64(f + 8);
        words = le32(f + 16);
        if (words > BLF_MAP_MAX || 20u + 4u * words > t->fmt_len ||
            20u + 4u * words > FMT_MAX)
            return XFS_ERR_JOURNAL;
        for (i = 0; i < words; i++)
            t->map[i] = le32(f + 20u + (size_t)i * 4u);
        t->map_bits = words * 32u;
        t->bit = 0;
        if (p->pass == 1) {
            p->nbb += chunk_bbs(t->map, t->map_bits);
            if ((flags & BLF_CANCEL) != 0)
                p->ncancel++;
            return XFS_OK;
        }
        if (!t->apply)
            return XFS_OK;
        if (p->pass == 3 && (flags & BLF_CANCEL) != 0) {
            struct cancel *c = cancel_find(p, t->blkno, le16(f + 6));

            if (c == (struct cancel *)0) {
                if (p->ncancels == p->ncancel)
                    return XFS_ERR_JOURNAL;
                c = &p->cancels[p->ncancels++];
                c->blkno = t->blkno;
                c->len = le16(f + 6);
                c->ref = 0;
            }
            c->ref++;
            return XFS_OK;
        }
        if (p->pass != 4)
            return XFS_OK;
        if ((flags & BLF_CANCEL) != 0) {
            struct cancel *c = cancel_find(p, t->blkno, le16(f + 6));

            if (c != (struct cancel *)0 && c->ref > 0)
                c->ref--;
            return XFS_OK;
        }
        if ((flags & (BLF_INODE_BUF | BLF_DQUOT)) != 0)
            return XFS_OK;
        {
            struct cancel *c = cancel_find(p, t->blkno, le16(f + 6));
            uint64_t lsn;
            int rc;

            if (c != (struct cancel *)0 && c->ref > 0)
                return XFS_OK;
            rc = block_lsn(v, t->blkno * BB, &lsn);
            if (rc != XFS_OK)
                return rc;
            if (newer_on_disk(lsn, t->lsn))
                return XFS_OK;
        }
        t->skip = 0;
        return XFS_OK;
    }

    if (t->type == LI_INODE) {
        uint64_t blkno;
        uint32_t boffset;

        /* The 64-bit layout, or the packed one a 32-bit kernel wrote. */
        if (t->fmt_len == 56u) {
            t->fields = le32(f + 4);
            t->dsize = le16(f + 10);
            blkno = le64(f + 40);
            boffset = le32(f + 52);
        } else if (t->fmt_len == 52u) {
            t->fields = le32(f + 4);
            t->dsize = le16(f + 10);
            blkno = le64(f + 36);
            boffset = le32(f + 48);
        } else {
            return XFS_ERR_JOURNAL;
        }
        if (boffset % v->inodesize != 0)
            return XFS_ERR_JOURNAL;
        t->at = blkno * BB + boffset;
        if (p->pass == 1)
            p->nbb += v->inodesize / BB + 1u;
        else if (p->pass == 4 && t->apply)
            t->skip = 0;
        return XFS_OK;
    }

    if ((t->type == LI_BUI || t->type == LI_XMI) && t->fmt_len >= 16u) {
        if (p->pass == 1)
            p->nint++;
        else if (p->pass == 3 && t->apply) {
            if (p->nintents == p->nint)
                return XFS_ERR_JOURNAL;
            p->intents[p->nintents++] = le64(f + 8);
        }
        return XFS_OK;
    }
    if ((t->type == LI_BUD || t->type == LI_XMD) && t->fmt_len >= 16u &&
        p->pass == 3 && t->apply) {
        uint32_t i;

        for (i = 0; i < p->nintents; i++) {
            if (p->intents[i] == le64(f + 8)) {
                p->intents[i] = p->intents[--p->nintents];
                break;
            }
        }
    }
    return XFS_OK;
}

/* The log's inode core, in the writer's order, as the disk has it. A
 * timestamp is one 64-bit count with bigtime and two 32-bit halves
 * without. */
static void core_to_disk(const uint8_t *l, uint8_t *d, uint64_t lsn)
{
    static const uint8_t f16[] = { 0, 2, 6, 20, 22, 80, 88, 90 };
    static const uint8_t f32[] = { 8, 12, 16, 72, 76, 84, 92, 96, 128 };
    static const uint8_t f64[] = { 24, 56, 64, 104, 120, 152 };
    static const uint8_t ts[] = { 32, 40, 48, 144 };
    int bigtime = (le64(l + 120) & DIFLAG2_BIGTIME) != 0;
    size_t i;

    memset(d, 0, IN_CORE);
    for (i = 0; i < sizeof f16; i++)
        put16(d + f16[i], le16(l + f16[i]));
    for (i = 0; i < sizeof f32; i++)
        put32(d + f32[i], le32(l + f32[i]));
    for (i = 0; i < sizeof f64; i++)
        put64(d + f64[i], le64(l + f64[i]));
    for (i = 0; i < sizeof ts; i++) {
        if (bigtime) {
            put64(d + ts[i], le64(l + ts[i]));
        } else {
            put32(d + ts[i], le32(l + ts[i]));
            put32(d + ts[i] + 4, le32(l + ts[i] + 4));
        }
    }
    d[4] = l[4];
    d[5] = l[5];
    d[82] = l[82];
    d[83] = l[83];
    memcpy(d + 132, l + 132, 12);
    memcpy(d + 160, l + 160, 16);
    put64(d + 112, lsn);
}

/* The in-core root of a B+tree data fork, as the log has it (a long
 * btree block header, keys and pointers at its own capacity), laid into
 * the inode's fork at the fork's capacity. */
static int broot_to_disk(xfs_vol *v, trans *t, const uint8_t *src)
{
    uint8_t d[1872];
    uint32_t smax;
    uint32_t dmax;
    uint16_t nr;
    uint32_t i;

    if (t->dsize < BMBT_HDR || t->dfork < 4u || t->dfork > sizeof d)
        return XFS_ERR_JOURNAL;
    smax = (t->dsize - BMBT_HDR) / 16u;
    dmax = (t->dfork - 4u) / 16u;
    nr = be16(src + 6);
    if (nr > smax || nr > dmax)
        return XFS_ERR_JOURNAL;
    memset(d, 0, t->dfork);
    put16(d, be16(src + 4));
    put16(d + 2, nr);
    for (i = 0; i < nr; i++) {
        memcpy(d + 4u + (size_t)i * 8u, src + BMBT_HDR + (size_t)i * 8u, 8);
        memcpy(d + 4u + (size_t)dmax * 8u + (size_t)i * 8u,
               src + BMBT_HDR + (size_t)smax * 8u + (size_t)i * 8u, 8);
    }
    return ovl_write(v, t->at + IN_CORE, d, t->dfork);
}

/* n bytes of an item's region, the region's bytes from t->reg_off on. */
static int region_bytes(replay *p, trans *t, const uint8_t *b, uint32_t n)
{
    xfs_vol *v = p->v;
    uint32_t off = t->reg_off;

    if (t->role == ROLE_HDR)
        return XFS_OK;
    if (t->role == ROLE_FMT) {
        uint32_t k = off < FMT_MAX ? FMT_MAX - off : 0;

        memcpy(t->fmt + off, b, n < k ? n : k);
        t->fmt_len = off + n;
        return XFS_OK;
    }
    if (t->skip || p->pass != 4)
        return XFS_OK;

    if (t->type == LI_BUF) {
        if (off == 0) {
            while (t->bit < t->map_bits &&
                   !((t->map[t->bit / 32u] >> (t->bit % 32u)) & 1u))
                t->bit++;
            if (t->bit >= t->map_bits)
                return XFS_ERR_JOURNAL;
            t->run_start = t->bit;
            while (t->bit < t->map_bits &&
                   ((t->map[t->bit / 32u] >> (t->bit % 32u)) & 1u))
                t->bit++;
            t->run_len = t->bit - t->run_start;
        }
        if (off + n > t->run_len * BLF_CHUNK)
            return XFS_ERR_JOURNAL;
        return ovl_write(v, t->blkno * BB + (uint64_t)t->run_start * BLF_CHUNK +
                                off, b,
                         n);
    }

    if (t->type == LI_INODE) {
        if (t->idx == 1) {
            if (off + n > IN_CORE)
                return XFS_ERR_JOURNAL;
            memcpy(t->core + off, b, n);
            return XFS_OK;
        }
        if (t->idx == 2 && (t->fields & ILOG_DFORK) != 0) {
            if ((t->fields & ILOG_DBROOT) != 0) {
                if (off + n > BROOT_MAX)
                    return XFS_ERR_JOURNAL;
                memcpy(p->broot + off, b, n);
                return XFS_OK;
            }
            if (off + n > t->dfork)
                return XFS_ERR_JOURNAL;
            return ovl_write(v, t->at + IN_CORE + off, b, n);
        }
    }
    return XFS_OK;
}

/* The region is whole. */
static int region_end(replay *p, trans *t)
{
    xfs_vol *v = p->v;
    int rc;

    if (t->role == ROLE_HDR) {
        t->have_hdr = 1;
        return XFS_OK;
    }
    if (t->role == ROLE_FMT)
        return item_start(p, t);
    if (t->skip || p->pass != 4 || t->type != LI_INODE)
        return XFS_OK;

    if (t->idx == 1) {
        uint8_t disk[IN_CORE];

        if (t->reg_off != IN_CORE)
            return XFS_ERR_JOURNAL;
        rc = xfs_rd(v, t->at, IN_CORE, disk);
        if (rc != XFS_OK)
            return rc;
        if (be16(disk) == IN_MAGIC && disk[4] >= 3u &&
            newer_on_disk(be64(disk + 112), t->lsn)) {
            t->skip = 1;
            return XFS_OK;
        }
        core_to_disk(t->core, disk, t->lsn);
        t->dfork = disk[82] != 0 ? disk[82] * 8u
                                 : (uint32_t)v->inodesize - IN_CORE;
        if (t->dfork > (uint32_t)v->inodesize - IN_CORE)
            return XFS_ERR_JOURNAL;
        return ovl_write(v, t->at, disk, IN_CORE);
    }
    if (t->idx == 2 && (t->fields & ILOG_DBROOT) != 0) {
        if (t->reg_off != t->dsize)
            return XFS_ERR_JOURNAL;
        return broot_to_disk(v, t, p->broot);
    }
    return XFS_OK;
}

static trans *trans_of(replay *p, uint32_t tid)
{
    uint32_t i;

    for (i = 0; i < OPEN_MAX; i++) {
        if (p->open[i].used && p->open[i].tid == tid)
            return &p->open[i];
    }
    return (trans *)0;
}

/* One op of a record. */
static int op(replay *p, const lrec *r, uint32_t index, uint32_t tid,
              uint8_t flags, uint32_t data, uint32_t len)
{
    xfs_vol *v = p->v;
    trans *t;
    int rc;

    if ((flags & OP_UNMOUNT) != 0)
        return XFS_OK;
    if ((flags & OP_START) != 0) {
        uint32_t i;

        for (i = 0; i < OPEN_MAX && p->open[i].used; i++)
            ;
        if (i == OPEN_MAX)
            return XFS_ERR_UNSUPPORTED;
        t = &p->open[i];
        memset(t, 0, sizeof *t);
        t->used = 1;
        t->tid = tid;
        t->key_lsn = r->lsn;
        t->key_op = index;
        t->lsn = r->lsn;
        t->apply = (uint8_t)(p->pass >= 3 &&
                             committed_has(p, t->key_lsn, t->key_op));
        if (p->pass == 1)
            p->nstart++;
        return XFS_OK;
    }
    /* An op of a transaction that began before the tail is not replayed. */
    t = trans_of(p, tid);
    if (t == (trans *)0)
        return XFS_OK;
    if ((flags & OP_COMMIT) != 0) {
        if (p->pass == 2) {
            if (p->ncommitted == p->nstart)
                return XFS_ERR_JOURNAL;
            p->committed[p->ncommitted].lsn = t->key_lsn;
            p->committed[p->ncommitted].op = t->key_op;
            p->committed[p->ncommitted].pad = 0;
            p->ncommitted++;
        }
        t->used = 0;
        return XFS_OK;
    }

    if ((flags & OP_WAS_CONT) == 0) {
        t->reg_off = 0;
        if (!t->have_hdr) {
            t->role = ROLE_HDR;
        } else if (t->role == ROLE_HDR || t->idx + 1u >= t->total) {
            t->role = ROLE_FMT;
            t->fmt_len = 0;
        } else {
            t->role = ROLE_REG;
            t->idx++;
        }
    }
    while (len > 0) {
        uint8_t b[BB];
        uint32_t n = len < BB ? len : BB;

        rc = rec_data(v, r, data, n, b);
        if (rc == XFS_OK)
            rc = region_bytes(p, t, b, n);
        if (rc != XFS_OK)
            return rc;
        t->reg_off += n;
        data += n;
        len -= n;
    }
    if ((flags & OP_CONTINUE) == 0) {
        rc = region_end(p, t);
        if (rc != XFS_OK)
            return rc;
        if (t->role == ROLE_FMT && t->total == 1u)
            t->idx = 0;
        else if (t->role == ROLE_FMT)
            t->role = ROLE_REG;     /* its regions follow */
    }
    return XFS_OK;
}

/* One walk of the log from the tail to the head. */
static int walk(replay *p, uint64_t tail, uint64_t head)
{
    xfs_vol *v = p->v;
    uint64_t n = v->log_bbs;
    uint64_t pos = tail;
    static lrec r;
    int rc;

    memset(p->open, 0, sizeof p->open);
    while (pos != head) {
        uint64_t left = (head + n - pos) % n;
        uint64_t span;
        uint32_t i;
        uint32_t off = 0;

        rc = rec_open(v, pos, &r);
        if (rc == XFS_ERR_CSUM && left <= LOG_TORN)
            return XFS_OK;          /* a torn write at the head */
        if (rc != XFS_OK)
            return rc == XFS_ERR_CSUM ? XFS_ERR_JOURNAL : rc;
        span = r.hdr_bbs + ((uint64_t)r.len + BB - 1u) / BB;
        if (span > left)
            return XFS_OK;
        for (i = 0; i < r.nops; i++) {
            uint8_t h[OP_HDR];
            uint32_t len;

            rc = rec_data(v, &r, off, OP_HDR, h);
            if (rc != XFS_OK)
                return rc;
            len = be32(h + 4);
            if (len > r.len - off - OP_HDR)
                return XFS_ERR_JOURNAL;
            rc = op(p, &r, i, be32(h), h[9], off + OP_HDR, len);
            if (rc != XFS_OK)
                return rc;
            off += OP_HDR + len;
        }
        pos = (pos + span) % n;
    }
    return XFS_OK;
}

static size_t round8(size_t x)
{
    return (x + 7u) & ~(size_t)7u;
}

void xfslog_release(xfs_vol *v)
{
    if (v->mem != (void *)0 && v->mem_put != 0)
        v->mem_put(v->mem_ctx, v->mem, v->mem_len);
    v->mem = (void *)0;
    v->mem_len = 0;
    v->ovl_n = 0;
    v->ovl_cap = 0;
    if (v->dev_read != (xfs_read_fn)0) {
        v->read = v->dev_read;
        v->ctx = v->dev_ctx;
    }
    v->win_off = ~(uint64_t)0;
}

int xfslog_replay(xfs_vol *v, const xfs_mem *mem, size_t *need)
{
    static replay p;
    uint64_t head;
    uint64_t last;
    uint64_t tail;
    size_t at;
    uint8_t *m;
    int rc;

    *need = 0;
    rc = xfs_log_last(v, &head, &last);
    if (rc != XFS_OK)
        return rc;
    if (last == ~(uint64_t)0)
        return XFS_ERR_JOURNAL;
    {
        static lrec r;

        rc = rec_open(v, last, &r);
        if (rc != XFS_OK)
            return rc == XFS_ERR_CSUM ? XFS_ERR_JOURNAL : rc;
        tail = r.tail_lsn & 0xFFFFFFFFu;
        if (tail >= v->log_bbs)
            return XFS_ERR_JOURNAL;
    }

    memset(&p, 0, sizeof p);
    p.v = v;
    p.pass = 1;
    rc = walk(&p, tail, head);
    if (rc != XFS_OK)
        return rc;

    at = round8((size_t)p.nstart * sizeof(struct start));
    at += round8((size_t)p.ncancel * sizeof(struct cancel));
    at += round8((size_t)p.nint * 8u);
    at += round8((size_t)p.nbb * 8u) + round8((size_t)p.nbb * 4u);
    at += (size_t)p.nbb * BB;
    *need = at;
    if (mem == (const xfs_mem *)0 || mem->get == 0)
        return XFS_ERR_NOMEM;
    m = mem->get(mem->ctx, at);
    if (m == (uint8_t *)0)
        return XFS_ERR_NOMEM;
    v->mem = m;
    v->mem_len = at;
    v->mem_put = mem->put;
    v->mem_ctx = mem->ctx;

    at = 0;
    p.committed = (struct start *)(void *)(m + at);
    at += round8((size_t)p.nstart * sizeof(struct start));
    p.cancels = (struct cancel *)(void *)(m + at);
    at += round8((size_t)p.ncancel * sizeof(struct cancel));
    p.intents = (uint64_t *)(void *)(m + at);
    at += round8((size_t)p.nint * 8u);
    v->ovl_bb = (uint64_t *)(void *)(m + at);
    at += round8((size_t)p.nbb * 8u);
    v->ovl_idx = (uint32_t *)(void *)(m + at);
    at += round8((size_t)p.nbb * 4u);
    v->ovl_data = m + at;
    v->ovl_n = 0;
    v->ovl_cap = (uint32_t)p.nbb;
    v->dev_read = v->read;
    v->dev_ctx = v->ctx;
    v->read = ovl_read;
    v->ctx = v;
    v->win_off = ~(uint64_t)0;

    p.pass = 2;
    rc = walk(&p, tail, head);
    if (rc == XFS_OK) {
        p.pass = 3;
        rc = walk(&p, tail, head);
    }
    if (rc == XFS_OK && p.nintents != 0)
        rc = XFS_ERR_NEEDS_RECOVERY;    /* an unfinished mapping change */
    if (rc == XFS_OK) {
        p.pass = 4;
        rc = walk(&p, tail, head);
    }
    if (rc != XFS_OK) {
        xfslog_release(v);
        return rc;
    }
    v->flags |= XFS_REPLAYED;
    return XFS_OK;
}
