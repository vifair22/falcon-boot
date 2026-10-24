/* falcon-boot: a read-only xfs reader. See xfs.h for the set it reads.
 *
 * The layout is the kernel's (fs/xfs/libxfs/xfs_format.h, xfs_da_format.h,
 * xfs_log_format.h). An address in a fork or a pointer is a filesystem
 * block number, the allocation group in its high bits (agblklog wide
 * below them), and an inode number is that block's with the inode's index
 * in the block below it (inopblog wide).
 */

#include <fs/xfs.h>
#include <fs/crc32c.h>

#include <lib/string.h>

/* ---- the format ---- */

#define SB_MAGIC     0x58465342u     /* "XFSB" */
#define IN_MAGIC     0x494eu         /* "IN" */
#define BMA3_MAGIC   0x424d4133u
#define XDB3_MAGIC   0x58444233u     /* a block-form directory's one block */
#define XDD3_MAGIC   0x58444433u     /* a data block of a larger one */
#define LOG_MAGIC    0xFEEDBABEu

#define SB_CRC       224u
#define IN_CORE      176u
#define IN_CRC       100u
#define BMBT_HDR     72u
#define BMBT_CRC     64u
#define DIR_HDR      64u
#define DIR_CRC      4u
#define BB           512u

/* Everything a directory names lives below this byte of its data space
 * (XFS_DIR2_LEAF_OFFSET); the leaf and free indexes are above. */
#define DIR_LEAF_OFF (32ull << 30)

/* ftype, sparse inodes, metadata UUID, bigtime, nrext64, exchange-range,
 * parent pointers, metadir. needsrepair (0x10) is refused by not being
 * here. */
#define INCOMPAT_OK  (0x1u | 0x2u | 0x4u | 0x8u | 0x20u | 0x40u | 0x80u | \
                      0x100u)
#define INCOMPAT_FTYPE     0x1u
#define INCOMPAT_META_UUID 0x4u
#define DIFLAG2_NREXT64    0x10u

#define FMT_LOCAL   1u
#define FMT_EXTENTS 2u
#define FMT_BTREE   3u

#define FT_REG 1u
#define FT_DIR 2u
#define S_IFMT_  0xF000u
#define S_IFREG_ 0x8000u
#define S_IFDIR_ 0x4000u

#define BTREE_DEPTH 8u
/* How far back from the head the log's last record can start, in basic
 * blocks: the largest iclog (256 KiB) and its headers, twice. */
#define LOG_SCAN    1100u
/* The iclogs in flight at a crash, whose blocks before the head can still
 * carry the cycle before (XLOG_MAX_ICLOGS of 256 KiB). */
#define LOG_TORN    4096u
#define OP_UNMOUNT  0x20u

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int pow2_in(uint32_t x, uint32_t lo, uint32_t hi)
{
    return x >= lo && x <= hi && (x & (x - 1u)) == 0;
}

static uint8_t log2u(uint32_t x)
{
    uint8_t n = 0;

    while (x > 1u) {
        x >>= 1;
        n++;
    }
    return n;
}

/* ---- the window ---- */

static const uint8_t *win_at(xfs_vol *v, uint64_t at, uint32_t *avail)
{
    uint64_t base = at & ~(uint64_t)(XFS_WIN - 1u);

    if (v->win_off != base) {
        v->win_off = ~(uint64_t)0;
        if (v->read(v->ctx, base, XFS_WIN, v->win) != 0)
            return (const uint8_t *)0;
        v->win_off = base;
    }
    *avail = XFS_WIN - (uint32_t)(at - base);
    return v->win + (at - base);
}

static int rd(xfs_vol *v, uint64_t at, uint32_t len, void *out)
{
    uint8_t *o = out;

    while (len > 0) {
        uint32_t avail;
        const uint8_t *p = win_at(v, at, &avail);
        uint32_t n;

        if (p == (const uint8_t *)0)
            return XFS_ERR_IO;
        n = len < avail ? len : avail;
        memcpy(o, p, n);
        o += n;
        at += n;
        len -= n;
    }
    return XFS_OK;
}

/* The crc32c xfs keeps of the `len` bytes at `at`, its own four bytes at
 * `field` counted as zeros, against what they hold. */
static int crc_check(xfs_vol *v, uint64_t at, uint32_t len, uint32_t field)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t done = 0;
    uint8_t stored[4];
    int rc = rd(v, at + field, 4, stored);

    if (rc != XFS_OK)
        return rc;
    while (done < len) {
        uint32_t avail = 0;
        const uint8_t *p = win_at(v, at + done, &avail);
        uint32_t n;

        if (p == (const uint8_t *)0)
            return XFS_ERR_IO;
        n = len - done < avail ? len - done : avail;
        if (done <= field && field < done + n) {
            uint32_t pre = field - done;
            uint32_t z = n - pre < 4u ? n - pre : 4u;

            crc = crc32c(crc, p, pre);
            crc = crc32c_zeros(crc, z);
            n = pre + z;
        } else if (field < done && done < field + 4u) {
            uint32_t z = field + 4u - done;

            if (z > n)
                z = n;
            crc = crc32c_zeros(crc, z);
            n = z;
        } else {
            crc = crc32c(crc, p, n);
        }
        done += n;
    }
    return (crc ^ 0xFFFFFFFFu) == le32(stored) ? XFS_OK : XFS_ERR_CSUM;
}

/* ---- addresses ---- */

static int fsb_byte(const xfs_vol *v, uint64_t fsb, uint64_t *out)
{
    uint64_t agno = fsb >> v->agblklog;
    uint64_t agbno = fsb & (((uint64_t)1 << v->agblklog) - 1u);

    if (agno >= v->agcount || agbno >= v->agblocks)
        return XFS_ERR_CORRUPT;
    *out = (agno * v->agblocks + agbno) << v->blocklog;
    return XFS_OK;
}

static int ino_byte(const xfs_vol *v, uint64_t ino, uint64_t *out)
{
    uint64_t at;
    int rc = fsb_byte(v, ino >> v->inopblog, &at);

    if (rc != XFS_OK)
        return rc;
    *out = at + (ino & (((uint64_t)1 << v->inopblog) - 1u)) * v->inodesize;
    return XFS_OK;
}

/* ---- inodes ---- */

static int inode_read(xfs_vol *v, uint64_t ino, xfs_inode *ip)
{
    uint8_t c[IN_CORE];
    uint64_t at;
    uint8_t forkoff;
    int rc;

    rc = ino_byte(v, ino, &at);
    if (rc != XFS_OK)
        return rc;
    rc = rd(v, at, IN_CORE, c);
    if (rc != XFS_OK)
        return rc;
    if (be16(c) != IN_MAGIC || c[4] != 3u || be64(c + 152) != ino ||
        memcmp(c + 160, v->uuid, 16) != 0)
        return XFS_ERR_CORRUPT;
    rc = crc_check(v, at, v->inodesize, IN_CRC);
    if (rc != XFS_OK)
        return rc;
    ip->ino = ino;
    ip->mode = be16(c + 2);
    ip->format = c[5];
    ip->size = be64(c + 56);
    ip->nextents = (be64(c + 120) & DIFLAG2_NREXT64) != 0 ? be64(c + 24)
                                                         : be32(c + 76);
    forkoff = c[82];
    ip->fork_len = forkoff != 0 ? (uint16_t)(forkoff * 8u)
                                : (uint16_t)(v->inodesize - IN_CORE);
    if (ip->fork_len > v->inodesize - IN_CORE || ip->fork_len > XFS_FORK_MAX)
        return XFS_ERR_CORRUPT;
    return rd(v, at + IN_CORE, ip->fork_len, ip->fork);
}

/* ---- the block map ---- */

/* Where logical block `lblk` of a fork is: `at` its byte, or ~0 for a
 * hole; `blocks` how many blocks on the same holds (a hole's count is how
 * far the next mapping is, ~0 when there is none). */
typedef struct {
    uint64_t at;
    uint64_t blocks;
    uint8_t  unwritten;
} xrun;

typedef struct {
    uint8_t  unwritten;
    uint64_t startoff;
    uint64_t startblock;
    uint32_t count;
} xrec;

static void rec_get(const uint8_t *p, xrec *r)
{
    uint64_t l0 = be64(p);
    uint64_t l1 = be64(p + 8);

    r->unwritten = (uint8_t)(l0 >> 63);
    r->startoff = (l0 >> 9) & ((((uint64_t)1) << 54) - 1u);
    r->startblock = ((l0 & 0x1FFu) << 43) | (l1 >> 21);
    r->count = (uint32_t)(l1 & 0x1FFFFFu);
}

/* One record against lblk: 1 when it holds it, 0 when not, with *next cut
 * to its start when it is the nearest after. */
static int rec_try(const xfs_vol *v, const xrec *r, uint64_t lblk, xrun *out,
                   uint64_t *next)
{
    uint64_t at;

    if (r->count == 0)
        return XFS_ERR_CORRUPT;
    if (lblk >= r->startoff && lblk - r->startoff < r->count) {
        if (fsb_byte(v, r->startblock + (lblk - r->startoff), &at) != XFS_OK)
            return XFS_ERR_CORRUPT;
        out->at = at;
        out->blocks = r->count - (lblk - r->startoff);
        out->unwritten = r->unwritten;
        return 1;
    }
    if (r->startoff > lblk && r->startoff < *next)
        *next = r->startoff;
    return 0;
}

static void hole(xrun *out, uint64_t lblk, uint64_t next)
{
    out->at = ~(uint64_t)0;
    out->blocks = next == ~(uint64_t)0 ? ~(uint64_t)0 : next - lblk;
    out->unwritten = 0;
}

static int bmap_extents(const xfs_vol *v, const xfs_inode *ip, uint64_t lblk,
                        xrun *out)
{
    uint64_t next = ~(uint64_t)0;
    uint64_t i;

    if (ip->nextents > ip->fork_len / 16u)
        return XFS_ERR_CORRUPT;
    for (i = 0; i < ip->nextents; i++) {
        xrec r;
        int rc;

        rec_get(ip->fork + (size_t)i * 16u, &r);
        rc = rec_try(v, &r, lblk, out, &next);
        if (rc != 0)
            return rc < 0 ? rc : XFS_OK;
    }
    hole(out, lblk, next);
    return XFS_OK;
}

/* A bmbt block below the inode's root: its header checked, its checksum
 * too, its record count returned. */
static int bmbt_open(xfs_vol *v, const xfs_inode *ip, uint64_t fsb,
                     uint16_t level, uint64_t *at, uint16_t *nr)
{
    uint8_t h[BMBT_HDR];
    int rc = fsb_byte(v, fsb, at);

    if (rc != XFS_OK)
        return rc;
    rc = rd(v, *at, BMBT_HDR, h);
    if (rc != XFS_OK)
        return rc;
    *nr = be16(h + 6);
    if (be32(h) != BMA3_MAGIC || be16(h + 4) != level ||
        be64(h + 24) != *at / BB || be64(h + 56) != ip->ino ||
        memcmp(h + 40, v->uuid, 16) != 0 ||
        *nr > (v->blocksize - BMBT_HDR) / 16u)
        return XFS_ERR_CORRUPT;
    return crc_check(v, *at, v->blocksize, BMBT_CRC);
}

static int bmap_btree(xfs_vol *v, const xfs_inode *ip, uint64_t lblk,
                      xrun *out)
{
    uint16_t level;
    uint16_t nr;
    uint32_t maxrecs;
    uint32_t i;
    uint64_t fsb;
    uint64_t at;
    uint64_t next = ~(uint64_t)0;
    int rc;

    if (ip->fork_len < 4u)
        return XFS_ERR_CORRUPT;
    level = be16(ip->fork);
    nr = be16(ip->fork + 2);
    maxrecs = (ip->fork_len - 4u) / 16u;
    if (level == 0 || level > BTREE_DEPTH || nr == 0 || nr > maxrecs)
        return XFS_ERR_CORRUPT;
    /* The root, in the inode: the last key not above lblk, or the first. */
    for (i = 1; i < nr && be64(ip->fork + 4u + (size_t)i * 8u) <= lblk; i++)
        ;
    fsb = be64(ip->fork + 4u + (size_t)maxrecs * 8u + (size_t)(i - 1u) * 8u);

    while (level-- > 0) {
        rc = bmbt_open(v, ip, fsb, level, &at, &nr);
        if (rc != XFS_OK)
            return rc;
        if (level > 0) {
            uint8_t k[8];

            if (nr == 0)
                return XFS_ERR_CORRUPT;
            maxrecs = (v->blocksize - BMBT_HDR) / 16u;
            for (i = 1; i < nr; i++) {
                rc = rd(v, at + BMBT_HDR + (uint64_t)i * 8u, 8, k);
                if (rc != XFS_OK)
                    return rc;
                if (be64(k) > lblk)
                    break;
            }
            rc = rd(v, at + BMBT_HDR + (uint64_t)maxrecs * 8u +
                        (uint64_t)(i - 1u) * 8u, 8, k);
            if (rc != XFS_OK)
                return rc;
            fsb = be64(k);
            continue;
        }
        for (i = 0; i < nr; i++) {
            uint8_t b[16];
            xrec r;

            rc = rd(v, at + BMBT_HDR + (uint64_t)i * 16u, 16, b);
            if (rc != XFS_OK)
                return rc;
            rec_get(b, &r);
            rc = rec_try(v, &r, lblk, out, &next);
            if (rc != 0)
                return rc < 0 ? rc : XFS_OK;
        }
    }
    /* Past this leaf's last record, the next one is in its sibling; a
     * block at a time is as right and costs only the lookup. */
    hole(out, lblk, next == ~(uint64_t)0 ? lblk + 1u : next);
    return XFS_OK;
}

static int bmap(xfs_vol *v, const xfs_inode *ip, uint64_t lblk, xrun *out)
{
    if (ip->format == FMT_EXTENTS)
        return bmap_extents(v, ip, lblk, out);
    if (ip->format == FMT_BTREE)
        return bmap_btree(v, ip, lblk, out);
    return XFS_ERR_CORRUPT;
}

/* ---- the log ---- */

static int log_cycle(xfs_vol *v, uint64_t bb, uint32_t *cycle)
{
    uint8_t w[8];
    int rc = rd(v, v->log_off + bb * BB, 8, w);

    if (rc != XFS_OK)
        return rc;
    /* A record header's first word is the magic; its cycle is the next. */
    *cycle = be32(w) == LOG_MAGIC ? be32(w + 4) : be32(w);
    return XFS_OK;
}

/* The block the log's next record would go at: the first one written in
 * the cycle before the last block's, or past the end when every block is
 * in one cycle; then any block in the flight of iclogs before it still on
 * that older cycle, which is a write that did not land. */
static int log_head(xfs_vol *v, uint64_t *head)
{
    uint64_t n = v->log_bbs;
    uint32_t first;
    uint32_t last;
    uint32_t c;
    uint64_t lo;
    uint64_t hi;
    uint64_t i;
    int rc;

    rc = log_cycle(v, 0, &first);
    if (rc == XFS_OK)
        rc = log_cycle(v, n - 1u, &last);
    if (rc != XFS_OK)
        return rc;
    if (first == last) {
        *head = n;
        return XFS_OK;
    }
    lo = 0;
    hi = n - 1u;
    while (hi - lo > 1u) {
        uint64_t mid = lo + (hi - lo) / 2u;

        rc = log_cycle(v, mid, &c);
        if (rc != XFS_OK)
            return rc;
        if (c == last)
            hi = mid;
        else
            lo = mid;
    }
    *head = hi;
    for (i = *head > LOG_TORN ? *head - LOG_TORN : 0; i < *head; i++) {
        rc = log_cycle(v, i, &c);
        if (rc != XFS_OK)
            return rc;
        if (c == last) {
            *head = i;
            break;
        }
    }
    return XFS_OK;
}

/* Clean when the last whole record before the head is the unmount record
 * alone. */
static int log_clean(xfs_vol *v)
{
    uint64_t n = v->log_bbs;
    uint64_t head;
    unsigned tries;
    int rc = log_head(v, &head);

    if (rc != XFS_OK)
        return rc;
    for (tries = 0; tries < 4u; tries++) {
        uint8_t h[324];
        uint64_t rec = ~(uint64_t)0;
        uint64_t k;
        uint32_t hdr_bbs;
        uint64_t len_bbs;

        for (k = 1; k <= LOG_SCAN && k <= n; k++) {
            uint64_t bb = (head + n - k) % n;
            uint8_t w[4];

            rc = rd(v, v->log_off + bb * BB, 4, w);
            if (rc != XFS_OK)
                return rc;
            if (be32(w) == LOG_MAGIC) {
                rec = bb;
                break;
            }
        }
        if (rec == ~(uint64_t)0)
            return XFS_ERR_NEEDS_RECOVERY;
        rc = rd(v, v->log_off + rec * BB, sizeof h, h);
        if (rc != XFS_OK)
            return rc;
        hdr_bbs = 1;
        if ((be32(h + 8) & 2u) != 0 && be32(h + 320) > 32768u)
            hdr_bbs = (be32(h + 320) + 32767u) / 32768u;
        len_bbs = ((uint64_t)be32(h + 12) + BB - 1u) / BB;
        /* A record that runs past the head did not finish: the one before
         * it is the last. */
        if (((head + n - rec) % n == 0 ? n : (head + n - rec) % n) <
            hdr_bbs + len_bbs) {
            head = rec;
            continue;
        }
        if (be32(h + 40) != 1u)
            return XFS_ERR_NEEDS_RECOVERY;
        {
            uint8_t op[12];

            rc = rd(v, v->log_off + ((rec + hdr_bbs) % n) * BB, sizeof op, op);
            if (rc != XFS_OK)
                return rc;
            return (op[9] & OP_UNMOUNT) != 0 ? XFS_OK
                                             : XFS_ERR_NEEDS_RECOVERY;
        }
    }
    return XFS_ERR_NEEDS_RECOVERY;
}

/* ---- mount ---- */

int xfs_probe(xfs_read_fn read, void *ctx)
{
    uint8_t m[4];

    if (read == (xfs_read_fn)0)
        return XFS_ERR_BAD_ARG;
    if (read(ctx, 0, sizeof m, m) != 0)
        return XFS_ERR_IO;
    return be32(m) == SB_MAGIC ? XFS_OK : XFS_ERR_NOT_XFS;
}

int xfs_mount(xfs_vol *v, xfs_read_fn read, void *ctx)
{
    uint8_t sb[264];
    uint32_t incompat;
    uint64_t at;
    int rc;

    if (v == (xfs_vol *)0 || read == (xfs_read_fn)0)
        return XFS_ERR_BAD_ARG;
    memset(v, 0, sizeof *v);
    v->read = read;
    v->ctx = ctx;
    v->win_off = ~(uint64_t)0;

    rc = rd(v, 0, sizeof sb, sb);
    if (rc != XFS_OK)
        return rc;
    if (be32(sb) != SB_MAGIC)
        return XFS_ERR_NOT_XFS;
    if ((be16(sb + 100) & 0xFu) != 5u)
        return XFS_ERR_UNSUPPORTED;
    v->blocksize = be32(sb + 4);
    v->sectsize = be16(sb + 102);
    v->inodesize = be16(sb + 104);
    v->blocklog = sb[120];
    v->inopblog = sb[123];
    v->agblklog = sb[124];
    v->dblocks = be64(sb + 8);
    v->agblocks = be32(sb + 84);
    v->agcount = be32(sb + 88);
    if (!pow2_in(v->blocksize, 1024u, 65536u) ||
        log2u(v->blocksize) != v->blocklog ||
        !pow2_in(v->sectsize, 512u, v->blocksize) ||
        !pow2_in(v->inodesize, 256u, 2048u) ||
        v->inodesize > v->blocksize ||
        v->inopblog != v->blocklog - log2u(v->inodesize) ||
        v->agblocks == 0 || v->agcount == 0 || v->agblklog > 31u ||
        v->agblocks > ((uint32_t)1 << v->agblklog))
        return XFS_ERR_CORRUPT;
    rc = crc_check(v, 0, v->sectsize, SB_CRC);
    if (rc != XFS_OK)
        return rc;
    incompat = be32(sb + 216);
    if ((incompat & ~INCOMPAT_OK) != 0 || (incompat & INCOMPAT_FTYPE) == 0 ||
        sb[192] != 0 || be64(sb + 16) != 0 || be64(sb + 48) == 0)
        return XFS_ERR_UNSUPPORTED;   /* directory blocks, realtime, log */
    v->ftype = 1;
    memcpy(v->uuid, sb + ((incompat & INCOMPAT_META_UUID) != 0 ? 248 : 32),
           16);
    v->rootino = be64(sb + 56);

    rc = fsb_byte(v, be64(sb + 48), &at);
    if (rc != XFS_OK)
        return rc;
    v->log_off = at;
    v->log_bbs = ((uint64_t)be32(sb + 96) << v->blocklog) / BB;
    if (v->log_bbs < 2u)
        return XFS_ERR_CORRUPT;
    return log_clean(v);
}

/* ---- directories ---- */

static int dir_at(xfs_vol *v, xfs_dir *d, uint64_t ino)
{
    int rc = inode_read(v, ino, &d->inode);

    if (rc != XFS_OK)
        return rc;
    if ((d->inode.mode & S_IFMT_) != S_IFDIR_)
        return XFS_ERR_BAD_ARG;
    d->vol = v;
    d->index = 0;
    d->blk_end = 0;
    d->blk_phys = ~(uint64_t)0;
    if (d->inode.format == FMT_LOCAL) {
        if (d->inode.fork_len < 6u)
            return XFS_ERR_CORRUPT;
        d->pos = 2u + (d->inode.fork[1] != 0 ? 8u : 4u);
    } else if (d->inode.format == FMT_EXTENTS ||
               d->inode.format == FMT_BTREE) {
        d->pos = 0;
    } else {
        return XFS_ERR_CORRUPT;
    }
    return XFS_OK;
}

int xfs_dir_open_root(xfs_vol *v, xfs_dir *d)
{
    if (v == (xfs_vol *)0 || d == (xfs_dir *)0)
        return XFS_ERR_BAD_ARG;
    return dir_at(v, d, v->rootino);
}

int xfs_dir_open(xfs_vol *v, xfs_dir *d, const xfs_dirent *e)
{
    if (v == (xfs_vol *)0 || d == (xfs_dir *)0 ||
        e == (const xfs_dirent *)0 || !e->is_dir)
        return XFS_ERR_BAD_ARG;
    return dir_at(v, d, e->ino);
}

static void fill(xfs_dirent *out, const uint8_t *name, uint8_t len,
                 uint8_t ftype, uint64_t ino)
{
    memcpy(out->name, name, len);
    out->name[len] = 0;
    out->name_len = len;
    out->is_dir = (uint8_t)(ftype == FT_DIR);
    out->is_regular = (uint8_t)(ftype == FT_REG);
    out->ino = ino;
}

/* A short-form directory: the entries in the inode. */
static int dir_next_local(xfs_dir *d, xfs_dirent *out)
{
    const uint8_t *f = d->inode.fork;
    uint32_t isz = f[1] != 0 ? 8u : 4u;
    uint32_t len;
    uint64_t ino;

    if (d->index >= f[0])
        return 0;
    if (d->pos + 3u > d->inode.fork_len)
        return XFS_ERR_CORRUPT;
    len = 3u + f[d->pos] + 1u + isz;
    if (f[d->pos] == 0 || d->pos + len > d->inode.fork_len)
        return XFS_ERR_CORRUPT;
    ino = isz == 8u ? be64(f + d->pos + 4u + f[d->pos])
                    : be32(f + d->pos + 4u + f[d->pos]);
    fill(out, f + d->pos + 3u, f[d->pos], f[d->pos + 3u + f[d->pos]], ino);
    d->pos += len;
    d->index++;
    return 1;
}

/* The data block lblk of a directory: its header checked and the end of
 * its entries found. 0 with d->pos moved past a hole, 1 when it is d's. */
static int dir_block(xfs_dir *d, uint64_t lblk)
{
    xfs_vol *v = d->vol;
    uint8_t h[DIR_HDR];
    xrun r;
    uint32_t magic;
    int rc = bmap(v, &d->inode, lblk, &r);

    if (rc != XFS_OK)
        return rc;
    if (r.at == ~(uint64_t)0) {
        d->pos = r.blocks == ~(uint64_t)0 || r.blocks > (DIR_LEAF_OFF >> v->blocklog)
                     ? DIR_LEAF_OFF
                     : (lblk + r.blocks) << v->blocklog;
        return 0;
    }
    if (r.unwritten)
        return XFS_ERR_CORRUPT;
    rc = rd(v, r.at, DIR_HDR, h);
    if (rc != XFS_OK)
        return rc;
    magic = be32(h);
    if ((magic != XDB3_MAGIC && magic != XDD3_MAGIC) ||
        be64(h + 8) != r.at / BB || be64(h + 40) != d->inode.ino ||
        memcmp(h + 24, v->uuid, 16) != 0)
        return XFS_ERR_CORRUPT;
    rc = crc_check(v, r.at, v->blocksize, DIR_CRC);
    if (rc != XFS_OK)
        return rc;
    d->blk_end = v->blocksize;
    if (magic == XDB3_MAGIC) {
        uint8_t t[4];
        uint32_t count;

        rc = rd(v, r.at + v->blocksize - 8u, 4, t);
        if (rc != XFS_OK)
            return rc;
        count = be32(t);
        if (count > (v->blocksize - DIR_HDR - 8u) / 8u)
            return XFS_ERR_CORRUPT;
        d->blk_end = v->blocksize - 8u - count * 8u;
    }
    d->blk_phys = r.at;
    d->pos += DIR_HDR;
    return 1;
}

int xfs_dir_next(xfs_dir *d, xfs_dirent *out)
{
    if (d == (xfs_dir *)0 || out == (xfs_dirent *)0)
        return XFS_ERR_BAD_ARG;
    if (d->inode.format == FMT_LOCAL)
        return dir_next_local(d, out);
    for (;;) {
        xfs_vol *v = d->vol;
        uint32_t in_blk = (uint32_t)(d->pos & (v->blocksize - 1u));
        uint8_t e[9];
        uint32_t size;
        uint8_t name[XFS_NAME_MAX + 1u];
        int rc;

        if (d->pos >= DIR_LEAF_OFF)
            return 0;
        if (in_blk == 0) {
            rc = dir_block(d, d->pos >> v->blocklog);
            if (rc < 0)
                return rc;
            continue;
        }
        if (in_blk >= d->blk_end) {
            d->pos += v->blocksize - in_blk;
            continue;
        }
        if (d->blk_end - in_blk < 8u)
            return XFS_ERR_CORRUPT;
        rc = rd(v, d->blk_phys + in_blk, 4, e);
        if (rc != XFS_OK)
            return rc;
        if (be16(e) == 0xFFFFu) {
            size = be16(e + 2);
            if (size < 8u || (size & 7u) != 0 || size > d->blk_end - in_blk)
                return XFS_ERR_CORRUPT;
            d->pos += size;
            continue;
        }
        rc = rd(v, d->blk_phys + in_blk, 9, e);
        if (rc != XFS_OK)
            return rc;
        size = (8u + 1u + e[8] + 1u + 2u + 7u) & ~7u;
        if (e[8] == 0 || size > d->blk_end - in_blk)
            return XFS_ERR_CORRUPT;
        rc = rd(v, d->blk_phys + in_blk + 9u, e[8] + 1u, name);
        if (rc != XFS_OK)
            return rc;
        d->pos += size;
        if ((e[8] == 1u && name[0] == '.') ||
            (e[8] == 2u && name[0] == '.' && name[1] == '.'))
            continue;
        fill(out, name, e[8], name[e[8]], be64(e));
        return 1;
    }
}

/* ---- files ---- */

int xfs_file_open(xfs_vol *v, xfs_file *f, const xfs_dirent *e)
{
    int rc;

    if (v == (xfs_vol *)0 || f == (xfs_file *)0 ||
        e == (const xfs_dirent *)0)
        return XFS_ERR_BAD_ARG;
    rc = inode_read(v, e->ino, &f->inode);
    if (rc != XFS_OK)
        return rc;
    if ((f->inode.mode & S_IFMT_) != S_IFREG_ ||
        (f->inode.format != FMT_EXTENTS && f->inode.format != FMT_BTREE))
        return XFS_ERR_UNSUPPORTED;
    f->vol = v;
    return XFS_OK;
}

int xfs_file_read(xfs_file *f, uint64_t off, uint64_t len, void *buf)
{
    uint8_t *out = buf;

    if (f == (xfs_file *)0 || (buf == (void *)0 && len > 0))
        return XFS_ERR_BAD_ARG;
    if (off > f->inode.size || len > f->inode.size - off)
        return XFS_ERR_RANGE;
    while (len > 0) {
        xfs_vol *v = f->vol;
        uint64_t lblk = off >> v->blocklog;
        uint64_t in_blk = off & (v->blocksize - 1u);
        uint64_t n;
        xrun r;
        int rc = bmap(v, &f->inode, lblk, &r);

        if (rc != XFS_OK)
            return rc;
        /* The run, or as much of it as is asked for. */
        if (r.blocks == ~(uint64_t)0 ||
            r.blocks >= (in_blk + len + v->blocksize - 1u) >> v->blocklog)
            n = len;
        else
            n = (r.blocks << v->blocklog) - in_blk;
        if (n == 0)
            return XFS_ERR_CORRUPT;
        if (r.at == ~(uint64_t)0 || r.unwritten)
            memset(out, 0, (size_t)n);
        else if (v->read(v->ctx, r.at + in_blk, n, out) != 0)
            return XFS_ERR_IO;
        off += n;
        out += n;
        len -= n;
    }
    return XFS_OK;
}
