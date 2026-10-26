/* falcon-boot: a read-only btrfs reader. See btrfs.h for the set it reads
 * and why nothing is replayed.
 *
 * The layout is the kernel's (include/uapi/linux/btrfs_tree.h): a tree
 * block is a 101-byte header, then 33-byte key pointers in a node or 25-byte
 * item headers in a leaf, a leaf's item data packed from its end. Keys sort
 * by (objectid, type, offset), and a node's key for a child is that child's
 * first key, so a descent that takes the last key not above the target
 * lands in the leaf that holds it.
 */

#include <fs/btrfs.h>
#include <fs/crc32c.h>

#include <lib/string.h>

/* ---- the format ---- */

#define SB_CSUM_TYPE    0xc4u
#define SB_MAGIC        0x40u
#define SB_ROOT         0x50u
#define SB_CHUNK_ROOT   0x58u
#define SB_LOG_ROOT     0x60u
#define SB_TOTAL_BYTES  0x70u
#define SB_NUM_DEVICES  0x88u
#define SB_SECTORSIZE   0x90u
#define SB_NODESIZE     0x94u
#define SB_SYS_ARRAY_SZ 0xa0u
#define SB_INCOMPAT     0xbcu
#define SB_ROOT_LEVEL   0xc6u
#define SB_CHUNK_LEVEL  0xc7u
#define SB_FSID         0x20u
#define SB_METADATA_UUID 0x23bu
#define SB_SYS_ARRAY    0x32bu
#define SB_SYS_ARRAY_MAX 2048u
#define SB_CSUM_FROM    0x20u
#define SB_SIZE         4096u

#define HDR_SIZE     101u
#define HDR_FSID     32u
#define HDR_BYTENR   48u
#define HDR_NRITEMS  96u
#define HDR_LEVEL    100u
#define KEY_SIZE     17u
#define PTR_SIZE     33u   /* key, blockptr, generation */
#define ITEM_SIZE    25u   /* key, data offset, data size */
#define MAX_LEVEL    8u

#define T_INODE_ITEM  1u
#define T_DIR_ITEM    84u
#define T_DIR_INDEX   96u
#define T_EXTENT_DATA 108u
#define T_ROOT_ITEM   132u
#define T_CHUNK_ITEM  228u

#define OID_FS_TREE          5u
#define OID_ROOT_TREE_DIR    6u
#define OID_FIRST_CHUNK_TREE 256u

/* Chunk profiles that spread one logical byte over more than one place,
 * which a single device cannot hold and this reader does not map. DUP's two
 * copies are both on the device, and the first one is read. */
#define BG_STRIPED (0x8u | 0x10u | 0x40u | 0x80u | 0x100u | 0x200u | 0x400u)

/* The incompat bits a single-device volume mkfs.btrfs and the kernel make
 * today can carry, every one of them about how things are written, none
 * about where: mixed backrefs, the default subvolume, mixed block groups,
 * the lzo and zstd markers (a compressed extent is refused where it is
 * met), big metadata, extended irefs, raid1c34 (its chunks are refused by
 * profile), skinny metadata, no holes, the metadata UUID and simple
 * quotas. Raid56, zoned, extent-tree-v2 and the raid stripe tree change
 * where things are, and anything newer is unknown. */
#define INCOMPAT_OK (0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x40u | \
                     0x100u | 0x200u | 0x400u | 0x800u | 0x10000u)
#define INCOMPAT_METADATA_UUID 0x400u

#define FE_INLINE   0u
#define FE_REG      1u
#define FE_PREALLOC 2u
#define FE_HEAD     21u    /* before an inline extent's data */
#define FE_SIZE     53u

#define DT_REG 1u
#define DT_DIR 2u
#define S_IFMT_  0xF000u
#define S_IFREG_ 0x8000u
#define S_IFDIR_ 0x4000u

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static void key_get(const uint8_t *p, btrfs_key *k)
{
    k->objectid = le64(p);
    k->type = p[8];
    k->offset = le64(p + 9);
}

static int key_cmp(const btrfs_key *a, const btrfs_key *b)
{
    if (a->objectid != b->objectid)
        return a->objectid < b->objectid ? -1 : 1;
    if (a->type != b->type)
        return a->type < b->type ? -1 : 1;
    if (a->offset != b->offset)
        return a->offset < b->offset ? -1 : 1;
    return 0;
}

static int pow2_in(uint32_t x, uint32_t lo, uint32_t hi)
{
    return x >= lo && x <= hi && (x & (x - 1u)) == 0;
}

/* ---- the window ---- */

/* The window over the physical byte at `phys`, and how many bytes of it
 * follow that byte. */
static const uint8_t *win_at(btrfs_vol *v, uint64_t phys, uint32_t *avail)
{
    uint64_t base = phys & ~(uint64_t)(BTRFS_WIN - 1u);

    if (v->win_off != base) {
        v->win_off = ~(uint64_t)0;
        if (v->read(v->ctx, base, BTRFS_WIN, v->win) != 0)
            return (const uint8_t *)0;
        v->win_off = base;
    }
    *avail = BTRFS_WIN - (uint32_t)(phys - base);
    return v->win + (phys - base);
}

/* len bytes at physical `phys`, through the window. */
static int rd(btrfs_vol *v, uint64_t phys, uint32_t len, void *out)
{
    uint8_t *o = out;

    while (len > 0) {
        uint32_t avail;
        const uint8_t *p = win_at(v, phys, &avail);
        uint32_t n;

        if (p == (const uint8_t *)0)
            return BTRFS_ERR_IO;
        n = len < avail ? len : avail;
        memcpy(o, p, n);
        o += n;
        phys += n;
        len -= n;
    }
    return BTRFS_OK;
}

/* ---- logical to physical ---- */

static const btrfs_chunk *chunk_of(const btrfs_vol *v, uint64_t logical)
{
    uint32_t i;

    for (i = 0; i < v->nchunks; i++) {
        const btrfs_chunk *c = &v->chunks[i];

        if (logical >= c->logical && logical - c->logical < c->length)
            return c;
    }
    return (const btrfs_chunk *)0;
}

/* The physical start of `len` logical bytes that one chunk holds whole. */
static int map(const btrfs_vol *v, uint64_t logical, uint64_t len,
               uint64_t *phys)
{
    const btrfs_chunk *c = chunk_of(v, logical);

    if (c == (const btrfs_chunk *)0 || len > c->length ||
        logical - c->logical > c->length - len)
        return BTRFS_ERR_CORRUPT;
    *phys = c->physical + (logical - c->logical);
    return BTRFS_OK;
}

static int add_chunk(btrfs_vol *v, uint64_t logical, const uint8_t *chunk,
                     uint32_t avail)
{
    uint64_t length;
    uint64_t type;
    uint16_t stripes;
    uint32_t i;

    if (avail < 48u + 32u)
        return BTRFS_ERR_CORRUPT;
    length = le64(chunk);
    type = le64(chunk + 24);
    stripes = le16(chunk + 44);
    if (length == 0 || stripes == 0 ||
        avail < 48u + 32u * (uint32_t)stripes)
        return BTRFS_ERR_CORRUPT;
    if ((type & BG_STRIPED) != 0)
        return BTRFS_ERR_UNSUPPORTED;
    for (i = 0; i < v->nchunks; i++) {
        if (v->chunks[i].logical == logical)
            return BTRFS_OK;
    }
    if (v->nchunks == BTRFS_CHUNKS_MAX)
        return BTRFS_ERR_UNSUPPORTED;
    v->chunks[v->nchunks].logical = logical;
    v->chunks[v->nchunks].length = length;
    v->chunks[v->nchunks].physical = le64(chunk + 48 + 8);
    v->nchunks++;
    return BTRFS_OK;
}

/* ---- tree blocks ---- */

/* The node at `logical`: its physical start and item count, after its
 * header is checked and, once a mount, its checksum. `level` is what the
 * parent says it is. */
static int node_open(btrfs_vol *v, uint64_t logical, uint8_t level,
                     uint64_t *phys, uint32_t *nr)
{
    uint8_t h[HDR_SIZE];
    uint32_t per;
    uint32_t i;
    int rc;

    rc = map(v, logical, v->nodesize, phys);
    if (rc != BTRFS_OK)
        return rc;
    rc = rd(v, *phys, HDR_SIZE, h);
    if (rc != BTRFS_OK)
        return rc;
    per = level > 0 ? PTR_SIZE : ITEM_SIZE;
    *nr = le32(h + HDR_NRITEMS);
    if (le64(h + HDR_BYTENR) != logical || h[HDR_LEVEL] != level ||
        memcmp(h + HDR_FSID, v->fsid, 16) != 0 ||
        *nr > (v->nodesize - HDR_SIZE) / per)
        return BTRFS_ERR_CORRUPT;

    for (i = 0; i < BTRFS_MEMO; i++) {
        if (v->memo[i] == logical)
            return BTRFS_OK;
    }
    {
        uint32_t crc = 0xFFFFFFFFu;
        uint64_t at = *phys + SB_CSUM_FROM;
        uint32_t left = v->nodesize - SB_CSUM_FROM;

        while (left > 0) {
            uint32_t avail;
            const uint8_t *p = win_at(v, at, &avail);
            uint32_t n;

            if (p == (const uint8_t *)0)
                return BTRFS_ERR_IO;
            n = left < avail ? left : avail;
            crc = crc32c(crc, p, n);
            at += n;
            left -= n;
        }
        if ((crc ^ 0xFFFFFFFFu) != le32(h))
            return BTRFS_ERR_CSUM;
    }
    v->memo[v->memo_next] = logical;
    v->memo_next = (v->memo_next + 1u) % BTRFS_MEMO;
    return BTRFS_OK;
}

/* A descent, kept so the walk can go on to the next leaf. Level 0 is the
 * leaf. */
typedef struct {
    uint64_t node[MAX_LEVEL];   /* logical */
    uint64_t phys[MAX_LEVEL];
    uint32_t nr[MAX_LEVEL];
    uint32_t slot[MAX_LEVEL];
    uint8_t  top;               /* the root's level */
} bpath;

static int ptr_at(btrfs_vol *v, uint64_t phys, uint32_t i, btrfs_key *k,
                  uint64_t *child)
{
    uint8_t b[PTR_SIZE];
    int rc = rd(v, phys + HDR_SIZE + (uint64_t)i * PTR_SIZE, PTR_SIZE, b);

    if (rc != BTRFS_OK)
        return rc;
    key_get(b, k);
    *child = le64(b + KEY_SIZE);
    return BTRFS_OK;
}

typedef struct {
    btrfs_key key;
    uint64_t  data;             /* physical */
    uint32_t  size;
} bitem;

static int item_at(btrfs_vol *v, uint64_t phys, uint32_t i, bitem *it)
{
    uint8_t b[ITEM_SIZE];
    uint32_t off;
    int rc = rd(v, phys + HDR_SIZE + (uint64_t)i * ITEM_SIZE, ITEM_SIZE, b);

    if (rc != BTRFS_OK)
        return rc;
    key_get(b, &it->key);
    off = le32(b + KEY_SIZE);
    it->size = le32(b + KEY_SIZE + 4);
    if (off > v->nodesize - HDR_SIZE || it->size > v->nodesize - HDR_SIZE - off)
        return BTRFS_ERR_CORRUPT;
    it->data = phys + HDR_SIZE + off;
    return BTRFS_OK;
}

/* Down from `root` to the leaf that would hold `want`; the leaf's slot is
 * the first item not below it, which may be one past the last. */
static int descend(btrfs_vol *v, uint64_t root, uint8_t level,
                   const btrfs_key *want, bpath *p)
{
    uint64_t node = root;
    int rc;

    if (level >= MAX_LEVEL)
        return BTRFS_ERR_CORRUPT;
    p->top = level;
    for (;;) {
        uint32_t lo = 0;
        uint32_t hi;
        btrfs_key k;

        rc = node_open(v, node, level, &p->phys[level], &p->nr[level]);
        if (rc != BTRFS_OK)
            return rc;
        p->node[level] = node;
        hi = p->nr[level];
        if (level == 0) {
            bitem it = { { 0, 0, 0 }, 0, 0 };

            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2u;

                rc = item_at(v, p->phys[0], mid, &it);
                if (rc != BTRFS_OK)
                    return rc;
                if (key_cmp(&it.key, want) < 0)
                    lo = mid + 1u;
                else
                    hi = mid;
            }
            p->slot[0] = lo;
            return BTRFS_OK;
        }
        if (hi == 0)
            return BTRFS_ERR_CORRUPT;
        /* The last key pointer not above `want`, or the first. */
        while (hi - lo > 1u) {
            uint32_t mid = lo + (hi - lo) / 2u;
            uint64_t child;

            rc = ptr_at(v, p->phys[level], mid, &k, &child);
            if (rc != BTRFS_OK)
                return rc;
            if (key_cmp(&k, want) <= 0)
                lo = mid;
            else
                hi = mid;
        }
        p->slot[level] = lo;
        rc = ptr_at(v, p->phys[level], lo, &k, &node);
        if (rc != BTRFS_OK)
            return rc;
        level--;
    }
}

/* The leaf after the path's, its slot 0. 0 when the path's leaf is the
 * last. */
static int next_leaf(btrfs_vol *v, bpath *p)
{
    uint8_t level;
    int rc;

    for (level = 1; level <= p->top; level++) {
        if (p->slot[level] + 1u < p->nr[level])
            break;
    }
    if (level > p->top)
        return 0;
    p->slot[level]++;
    for (;;) {
        btrfs_key k;
        uint64_t child;

        rc = ptr_at(v, p->phys[level], p->slot[level], &k, &child);
        if (rc != BTRFS_OK)
            return rc;
        level--;
        rc = node_open(v, child, level, &p->phys[level], &p->nr[level]);
        if (rc != BTRFS_OK)
            return rc;
        p->node[level] = child;
        p->slot[level] = 0;
        if (level == 0)
            return 1;
    }
}

/* The item at the path's slot, moving to the next leaf when the slot is
 * past this one's last. 1 with *it, 0 at the end of the tree. */
static int path_item(btrfs_vol *v, bpath *p, bitem *it)
{
    while (p->slot[0] >= p->nr[0]) {
        int rc = next_leaf(v, p);

        if (rc <= 0)
            return rc;
    }
    {
        int rc = item_at(v, p->phys[0], p->slot[0], it);

        return rc == BTRFS_OK ? 1 : rc;
    }
}

/* The first item not below `want`: 1 with *it, 0 when there is none. */
static int search_ge(btrfs_vol *v, uint64_t root, uint8_t level,
                     const btrfs_key *want, bpath *p, bitem *it)
{
    int rc = descend(v, root, level, want, p);

    return rc != BTRFS_OK ? rc : path_item(v, p, it);
}

/* The last item not above `want`: 1 with *it, 0 when every item is above
 * it. A node's key is its child's first, so the leaf the descent ends in
 * holds that item unless its first item is already above `want`, which
 * happens only on the leftmost path. */
static int search_le(btrfs_vol *v, uint64_t root, uint8_t level,
                     const btrfs_key *want, bitem *it)
{
    bpath p;
    int rc = descend(v, root, level, want, &p);

    if (rc != BTRFS_OK)
        return rc;
    if (p.slot[0] < p.nr[0]) {
        rc = item_at(v, p.phys[0], p.slot[0], it);
        if (rc != BTRFS_OK)
            return rc;
        if (key_cmp(&it->key, want) == 0)
            return 1;
    }
    if (p.slot[0] == 0)
        return 0;
    rc = item_at(v, p.phys[0], p.slot[0] - 1u, it);
    return rc == BTRFS_OK ? 1 : rc;
}

/* ---- mount ---- */

int btrfs_probe(btrfs_read_fn read, void *ctx)
{
    uint8_t m[8];

    if (read == (btrfs_read_fn)0)
        return BTRFS_ERR_BAD_ARG;
    if (read(ctx, BTRFS_SUPER_OFF + SB_MAGIC, sizeof m, m) != 0)
        return BTRFS_ERR_IO;
    return memcmp(m, "_BHRfS_M", 8) == 0 ? BTRFS_OK : BTRFS_ERR_NOT_BTRFS;
}

/* The chunks the superblock carries, enough to read the chunk tree by. */
static int sys_chunks(btrfs_vol *v, const uint8_t *arr, uint32_t size)
{
    uint32_t at = 0;

    while (at < size) {
        btrfs_key k;
        uint16_t stripes;
        uint32_t len;
        int rc;

        if (size - at < KEY_SIZE + 48u)
            return BTRFS_ERR_CORRUPT;
        key_get(arr + at, &k);
        if (k.type != T_CHUNK_ITEM)
            return BTRFS_ERR_CORRUPT;
        stripes = le16(arr + at + KEY_SIZE + 44);
        len = KEY_SIZE + 48u + 32u * (uint32_t)stripes;
        if (stripes == 0 || len > size - at)
            return BTRFS_ERR_CORRUPT;
        rc = add_chunk(v, k.offset, arr + at + KEY_SIZE, len - KEY_SIZE);
        if (rc != BTRFS_OK)
            return rc;
        at += len;
    }
    return BTRFS_OK;
}

/* Every chunk the chunk tree names. */
static int chunk_tree(btrfs_vol *v, uint64_t root, uint8_t level)
{
    static const btrfs_key first = { OID_FIRST_CHUNK_TREE, T_CHUNK_ITEM, 0 };
    bpath p;
    bitem it = { { 0, 0, 0 }, 0, 0 };
    int rc = search_ge(v, root, level, &first, &p, &it);

    while (rc == 1 && it.key.type == T_CHUNK_ITEM) {
        /* The chunk and its first stripe; add_chunk checks the stripe
         * count against the item's whole size. */
        uint8_t c[48u + 32u];

        if (it.size < sizeof c)
            return BTRFS_ERR_CORRUPT;
        rc = rd(v, it.data, sizeof c, c);
        if (rc != BTRFS_OK)
            return rc;
        rc = add_chunk(v, it.key.offset, c, it.size);
        if (rc != BTRFS_OK)
            return rc;
        p.slot[0]++;
        rc = path_item(v, &p, &it);
    }
    return rc < 0 ? rc : BTRFS_OK;
}

/* The subvolume the root directory's "default" entry names; 5, the top
 * one, when there is none. */
static int default_subvol(btrfs_vol *v, uint64_t *id)
{
    static const btrfs_key first = { OID_ROOT_TREE_DIR, T_DIR_ITEM, 0 };
    bpath p;
    bitem it = { { 0, 0, 0 }, 0, 0 };
    int rc = search_ge(v, v->root_tree, v->root_level, &first, &p, &it);

    *id = OID_FS_TREE;
    while (rc == 1 && it.key.objectid == OID_ROOT_TREE_DIR &&
           it.key.type == T_DIR_ITEM) {
        uint32_t at = 0;

        /* A DIR_ITEM holds every name whose hash collided, one after the
         * other. */
        while (at + 30u <= it.size) {
            uint8_t d[30u + 7u];
            uint16_t data_len;
            uint16_t name_len;

            rc = rd(v, it.data + at, 30u, d);
            if (rc != BTRFS_OK)
                return rc;
            data_len = le16(d + 25);
            name_len = le16(d + 27);
            if ((uint32_t)name_len + data_len > it.size - at - 30u)
                return BTRFS_ERR_CORRUPT;
            if (name_len == 7u) {
                rc = rd(v, it.data + at + 30u, 7u, d + 30);
                if (rc != BTRFS_OK)
                    return rc;
                if (memcmp(d + 30, "default", 7) == 0) {
                    *id = le64(d);
                    return BTRFS_OK;
                }
            }
            at += 30u + name_len + data_len;
        }
        p.slot[0]++;
        rc = path_item(v, &p, &it);
    }
    return rc < 0 ? rc : BTRFS_OK;
}

int btrfs_mount(btrfs_vol *v, btrfs_read_fn read, void *ctx)
{
    const uint8_t *sb;
    uint32_t avail;
    uint64_t chunk_root;
    uint8_t chunk_level;
    uint64_t subvol;
    uint32_t sys_size;
    uint64_t incompat;
    bitem it = { { 0, 0, 0 }, 0, 0 };
    bpath p;
    btrfs_key want;
    uint32_t i;
    int rc;

    if (v == (btrfs_vol *)0 || read == (btrfs_read_fn)0)
        return BTRFS_ERR_BAD_ARG;
    memset(v, 0, sizeof *v);
    v->read = read;
    v->ctx = ctx;
    v->win_off = ~(uint64_t)0;
    for (i = 0; i < BTRFS_MEMO; i++)
        v->memo[i] = ~(uint64_t)0;

    sb = win_at(v, BTRFS_SUPER_OFF, &avail);
    if (sb == (const uint8_t *)0)
        return BTRFS_ERR_IO;
    if (memcmp(sb + SB_MAGIC, "_BHRfS_M", 8) != 0)
        return BTRFS_ERR_NOT_BTRFS;
    if (le16(sb + SB_CSUM_TYPE) != 0)
        return BTRFS_ERR_UNSUPPORTED;
    if ((crc32c(0xFFFFFFFFu, sb + SB_CSUM_FROM, SB_SIZE - SB_CSUM_FROM) ^
         0xFFFFFFFFu) != le32(sb))
        return BTRFS_ERR_CSUM;

    v->sectorsize = le32(sb + SB_SECTORSIZE);
    v->nodesize = le32(sb + SB_NODESIZE);
    v->total_bytes = le64(sb + SB_TOTAL_BYTES);
    incompat = le64(sb + SB_INCOMPAT);
    if (!pow2_in(v->sectorsize, 4096u, 65536u) ||
        !pow2_in(v->nodesize, v->sectorsize, 65536u))
        return BTRFS_ERR_CORRUPT;
    if (le64(sb + SB_NUM_DEVICES) != 1u || (incompat & ~(uint64_t)INCOMPAT_OK))
        return BTRFS_ERR_UNSUPPORTED;
    memcpy(v->fsid, sb + ((incompat & INCOMPAT_METADATA_UUID) != 0
                              ? SB_METADATA_UUID : SB_FSID), 16);
    v->root_tree = le64(sb + SB_ROOT);
    v->root_level = sb[SB_ROOT_LEVEL];
    chunk_root = le64(sb + SB_CHUNK_ROOT);
    chunk_level = sb[SB_CHUNK_LEVEL];
    if (le64(sb + SB_LOG_ROOT) != 0)
        v->flags |= BTRFS_LOG_SKIPPED;
    sys_size = le32(sb + SB_SYS_ARRAY_SZ);
    if (sys_size > SB_SYS_ARRAY_MAX)
        return BTRFS_ERR_CORRUPT;
    /* The array is parsed in the window, before anything else is read. */
    rc = sys_chunks(v, sb + SB_SYS_ARRAY, sys_size);
    if (rc != BTRFS_OK)
        return rc;

    rc = chunk_tree(v, chunk_root, chunk_level);
    if (rc != BTRFS_OK)
        return rc;
    rc = default_subvol(v, &subvol);
    if (rc != BTRFS_OK)
        return rc;

    want.objectid = subvol;
    want.type = T_ROOT_ITEM;
    want.offset = 0;
    rc = search_ge(v, v->root_tree, v->root_level, &want, &p, &it);
    if (rc < 0)
        return rc;
    if (rc == 0 || it.key.objectid != subvol || it.key.type != T_ROOT_ITEM ||
        it.size < 239u)
        return BTRFS_ERR_CORRUPT;
    {
        uint8_t r[239];

        rc = rd(v, it.data, sizeof r, r);
        if (rc != BTRFS_OK)
            return rc;
        v->root_dir = le64(r + 168);
        v->fs_tree = le64(r + 176);
        v->fs_level = r[238];
    }
    return BTRFS_OK;
}

/* ---- inodes and directories ---- */

static int inode_get(btrfs_vol *v, uint64_t ino, uint32_t *mode,
                     uint64_t *size)
{
    btrfs_key want;
    bpath p;
    bitem it = { { 0, 0, 0 }, 0, 0 };
    uint8_t b[60];
    int rc;

    want.objectid = ino;
    want.type = T_INODE_ITEM;
    want.offset = 0;
    rc = search_ge(v, v->fs_tree, v->fs_level, &want, &p, &it);
    if (rc < 0)
        return rc;
    if (rc == 0 || it.key.objectid != ino || it.key.type != T_INODE_ITEM ||
        it.size < 160u)
        return BTRFS_ERR_NOT_FOUND;
    rc = rd(v, it.data, sizeof b, b);
    if (rc != BTRFS_OK)
        return rc;
    *size = le64(b + 16);
    *mode = le32(b + 52);
    return BTRFS_OK;
}

static int dir_at(btrfs_vol *v, btrfs_dir *d, uint64_t ino)
{
    uint32_t mode;
    uint64_t size;
    int rc = inode_get(v, ino, &mode, &size);

    if (rc != BTRFS_OK)
        return rc;
    if ((mode & S_IFMT_) != S_IFDIR_)
        return BTRFS_ERR_BAD_ARG;
    d->vol = v;
    d->ino = ino;
    d->next_index = 0;
    return BTRFS_OK;
}

int btrfs_dir_open_root(btrfs_vol *v, btrfs_dir *d)
{
    if (v == (btrfs_vol *)0 || d == (btrfs_dir *)0)
        return BTRFS_ERR_BAD_ARG;
    return dir_at(v, d, v->root_dir);
}

int btrfs_dir_open(btrfs_vol *v, btrfs_dir *d, const btrfs_dirent *e)
{
    if (v == (btrfs_vol *)0 || d == (btrfs_dir *)0 ||
        e == (const btrfs_dirent *)0 || !e->is_dir)
        return BTRFS_ERR_BAD_ARG;
    return dir_at(v, d, e->ino);
}

int btrfs_dir_next(btrfs_dir *d, btrfs_dirent *out)
{
    if (d == (btrfs_dir *)0 || out == (btrfs_dirent *)0)
        return BTRFS_ERR_BAD_ARG;
    for (;;) {
        btrfs_vol *v = d->vol;
        btrfs_key want;
        bpath p;
        bitem it = { { 0, 0, 0 }, 0, 0 };
        uint8_t h[30];
        uint16_t name_len;
        int rc;

        want.objectid = d->ino;
        want.type = T_DIR_INDEX;
        want.offset = d->next_index;
        rc = search_ge(v, v->fs_tree, v->fs_level, &want, &p, &it);
        if (rc <= 0)
            return rc;
        if (it.key.objectid != d->ino || it.key.type != T_DIR_INDEX)
            return 0;
        if (it.key.offset == ~(uint64_t)0)
            return BTRFS_ERR_CORRUPT;
        d->next_index = it.key.offset + 1u;
        if (it.size < 30u)
            return BTRFS_ERR_CORRUPT;
        rc = rd(v, it.data, 30u, h);
        if (rc != BTRFS_OK)
            return rc;
        name_len = le16(h + 27);
        if (name_len == 0 || name_len > BTRFS_NAME_MAX ||
            name_len > it.size - 30u)
            return BTRFS_ERR_CORRUPT;
        /* A subvolume's entry points into another tree. */
        if (h[8] == T_ROOT_ITEM)
            continue;
        rc = rd(v, it.data + 30u, name_len, out->name);
        if (rc != BTRFS_OK)
            return rc;
        out->name[name_len] = 0;
        out->name_len = (uint8_t)name_len;
        out->is_dir = (uint8_t)(h[29] == DT_DIR);
        out->is_regular = (uint8_t)(h[29] == DT_REG);
        out->ino = le64(h);
        out->size = 0;
        return 1;
    }
}

/* ---- files ---- */

int btrfs_file_open(btrfs_vol *v, btrfs_file *f, const btrfs_dirent *e)
{
    uint32_t mode;
    int rc;

    if (v == (btrfs_vol *)0 || f == (btrfs_file *)0 ||
        e == (const btrfs_dirent *)0)
        return BTRFS_ERR_BAD_ARG;
    rc = inode_get(v, e->ino, &mode, &f->size);
    if (rc != BTRFS_OK)
        return rc;
    if ((mode & S_IFMT_) != S_IFREG_)
        return BTRFS_ERR_UNSUPPORTED;
    f->vol = v;
    f->ino = e->ino;
    return BTRFS_OK;
}

/* len logical bytes, chunk by chunk, straight into the caller's buffer. */
static int read_logical(btrfs_vol *v, uint64_t logical, uint64_t len,
                        uint8_t *out)
{
    while (len > 0) {
        const btrfs_chunk *c = chunk_of(v, logical);
        uint64_t n;

        if (c == (const btrfs_chunk *)0)
            return BTRFS_ERR_CORRUPT;
        n = c->length - (logical - c->logical);
        if (n > len)
            n = len;
        if (v->read(v->ctx, c->physical + (logical - c->logical), n, out) != 0)
            return BTRFS_ERR_IO;
        logical += n;
        out += n;
        len -= n;
    }
    return BTRFS_OK;
}

/* The bytes of [off, off + *n) that the extent `it` holds, at most *n of
 * them; *n is cut to what it covers. 0 when it covers none of off. */
static int from_extent(btrfs_vol *v, const bitem *it, uint64_t off,
                       uint64_t *n, uint8_t *out)
{
    uint8_t e[FE_SIZE];
    uint64_t start = it->key.offset;
    uint64_t len;
    int rc;

    if (it->size < FE_HEAD)
        return BTRFS_ERR_CORRUPT;
    rc = rd(v, it->data, it->size < FE_SIZE ? FE_HEAD : FE_SIZE, e);
    if (rc != BTRFS_OK)
        return rc;
    if (e[17] != 0 || le16(e + 18) != 0)
        return BTRFS_ERR_UNSUPPORTED;            /* encryption, encoding */
    if (e[20] == FE_INLINE) {
        if (e[16] != 0)
            return BTRFS_ERR_UNSUPPORTED;
        len = it->size - FE_HEAD;
    } else if (e[20] == FE_REG || e[20] == FE_PREALLOC) {
        if (it->size < FE_SIZE)
            return BTRFS_ERR_CORRUPT;
        len = le64(e + 45);
    } else {
        return BTRFS_ERR_CORRUPT;
    }
    if (off < start || off - start >= len)
        return 0;
    if (*n > len - (off - start))
        *n = len - (off - start);
    if (e[20] == FE_INLINE)
        return rd(v, it->data + FE_HEAD + (off - start), (uint32_t)*n, out) ==
               BTRFS_OK ? 1 : BTRFS_ERR_IO;
    if (e[20] == FE_PREALLOC || le64(e + 21) == 0) {
        memset(out, 0, (size_t)*n);
        return 1;
    }
    if (e[16] != 0)
        return BTRFS_ERR_UNSUPPORTED;            /* compressed */
    rc = read_logical(v, le64(e + 21) + le64(e + 37) + (off - start), *n, out);
    return rc == BTRFS_OK ? 1 : rc;
}

int btrfs_file_read(btrfs_file *f, uint64_t off, uint64_t len, void *buf)
{
    uint8_t *out = buf;

    if (f == (btrfs_file *)0 || (buf == (void *)0 && len > 0))
        return BTRFS_ERR_BAD_ARG;
    if (off > f->size || len > f->size - off)
        return BTRFS_ERR_RANGE;
    while (len > 0) {
        btrfs_vol *v = f->vol;
        btrfs_key want;
        bitem it = { { 0, 0, 0 }, 0, 0 };
        uint64_t n = len;
        int rc;

        want.objectid = f->ino;
        want.type = T_EXTENT_DATA;
        want.offset = off;
        rc = search_le(v, v->fs_tree, v->fs_level, &want, &it);
        if (rc < 0)
            return rc;
        if (rc == 1 && it.key.objectid == f->ino &&
            it.key.type == T_EXTENT_DATA)
            rc = from_extent(v, &it, off, &n, out);
        else
            rc = 0;
        if (rc < 0)
            return rc;
        if (rc == 0) {
            /* A hole, as far as the next extent or the end. */
            bpath p;

            rc = search_ge(v, v->fs_tree, v->fs_level, &want, &p, &it);
            if (rc < 0)
                return rc;
            if (rc == 1 && it.key.objectid == f->ino &&
                it.key.type == T_EXTENT_DATA) {
                if (it.key.offset <= off)
                    return BTRFS_ERR_CORRUPT;
                if (n > it.key.offset - off)
                    n = it.key.offset - off;
            }
            memset(out, 0, (size_t)n);
        }
        if (n == 0)
            return BTRFS_ERR_CORRUPT;
        off += n;
        out += n;
        len -= n;
    }
    return BTRFS_OK;
}
