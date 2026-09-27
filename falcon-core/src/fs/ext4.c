/* Read-only ext4. Structure layouts follow the kernel's on-disk format as
 * documented in Documentation/filesystems/ext4/; field offsets are spelled out
 * as constants below and were checked byte for byte against an image produced
 * by mke2fs 1.47.4. The supported feature set, the checksum policy and the
 * deliberate omissions are argued in ext4.h; read that first.
 */

#include <fs/ext4.h>
#include <lib/string.h>

#define OFF_INVALID (~(uint64_t)0)
#define IDX_INVALID (~(uint32_t)0)

/* Superblock, relative to its byte offset 1024. */
#define SB_OFF                  1024u
#define SB_SIZE                 1024u
#define SB_INODES_COUNT         0x00u
#define SB_BLOCKS_COUNT_LO      0x04u
#define SB_FIRST_DATA_BLOCK     0x14u
#define SB_LOG_BLOCK_SIZE       0x18u
#define SB_BLOCKS_PER_GROUP     0x20u
#define SB_INODES_PER_GROUP     0x28u
#define SB_MAGIC                0x38u
#define SB_STATE                0x3Au
#define SB_REV_LEVEL            0x4Cu
#define SB_FIRST_INO            0x54u
#define SB_INODE_SIZE           0x58u
#define SB_FEATURE_COMPAT       0x5Cu
#define SB_FEATURE_INCOMPAT     0x60u
#define SB_FEATURE_RO_COMPAT    0x64u
#define SB_UUID                 0x68u
#define SB_DESC_SIZE            0xFEu
#define SB_BLOCKS_COUNT_HI      0x150u
#define SB_CHECKSUM_TYPE        0x175u
#define SB_CHECKSUM_SEED        0x270u
#define SB_CHECKSUM             0x3FCu

#define EXT4_MAGIC              0xEF53u
#define EXT4_VALID_FS           0x0001u /* cleanly unmounted */
#define EXT4_ERROR_FS           0x0002u /* errors detected */
#define EXT4_ORPHAN_FS          0x0004u /* orphan inodes being recovered */
#define EXT4_CHECKSUM_CRC32C    1u

/* Feature bits. The whitelists are exactly what a stock mkfs.ext4 sets on a
 * 1 GiB partition (ext4.h has the dumpe2fs evidence); anything outside them is
 * refused rather than assumed harmless. */
#define INCOMPAT_FILETYPE       0x0002u
#define INCOMPAT_RECOVER        0x0004u
#define INCOMPAT_EXTENTS        0x0040u
#define INCOMPAT_64BIT          0x0080u
#define INCOMPAT_FLEX_BG        0x0200u
#define INCOMPAT_CSUM_SEED      0x2000u
#define INCOMPAT_SUPPORTED (INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | \
                            INCOMPAT_64BIT | INCOMPAT_FLEX_BG | \
                            INCOMPAT_CSUM_SEED)

#define RO_COMPAT_SPARSE_SUPER  0x0001u
#define RO_COMPAT_LARGE_FILE    0x0002u
#define RO_COMPAT_BTREE_DIR     0x0004u
#define RO_COMPAT_HUGE_FILE     0x0008u
#define RO_COMPAT_DIR_NLINK     0x0020u
#define RO_COMPAT_EXTRA_ISIZE   0x0040u
#define RO_COMPAT_METADATA_CSUM 0x0400u
/* Set while the orphan file still lists inodes a mount must finish deleting.
 * It is not a format this driver cannot read, it is a filesystem nobody
 * finished writing, so it is answered as dirty rather than as unsupported. */
#define RO_COMPAT_ORPHAN_PRESENT 0x00010000u
#define RO_COMPAT_SUPPORTED (RO_COMPAT_SPARSE_SUPER | RO_COMPAT_LARGE_FILE | \
                             RO_COMPAT_BTREE_DIR | RO_COMPAT_HUGE_FILE | \
                             RO_COMPAT_DIR_NLINK | RO_COMPAT_EXTRA_ISIZE | \
                             RO_COMPAT_METADATA_CSUM)

/* Group descriptor. */
#define GD_INODE_TABLE_LO       0x08u
#define GD_CHECKSUM             0x1Eu
#define GD_INODE_TABLE_HI       0x28u

/* Inode. */
#define INO_MODE                0x00u
#define INO_SIZE_LO             0x04u
#define INO_FLAGS               0x20u
#define INO_BLOCK               0x28u
#define INO_BLOCK_SIZE          60u
#define INO_GENERATION          0x64u
#define INO_SIZE_HIGH           0x6Cu
#define INO_CHECKSUM_LO         0x7Cu
#define INO_GOOD_OLD_SIZE       128u
#define INO_EXTRA_ISIZE         0x80u
#define INO_CHECKSUM_HI         0x82u

#define S_IFMT                  0xF000u
#define S_IFREG                 0x8000u
#define S_IFDIR                 0x4000u
#define S_IFLNK                 0xA000u

#define INODE_FL_EXTENTS        0x00080000u
#define INODE_FL_INLINE_DATA    0x10000000u

/* Extent tree.
 *
 * A node is a 12-byte ext4_extent_header followed by eh_max slots of 12 bytes
 * each. Which struct a slot holds depends on eh_depth and nothing else: above
 * zero they are ext4_extent_idx and point at child nodes, at zero they are
 * ext4_extent and point at file data. All three structs are exactly 12 bytes,
 * which is why one EXT_ENT_SIZE indexes both kinds; the EI_ and EE_ offsets
 * below describe the same twelve bytes two different ways and must never be
 * mixed.
 *
 * The root lives in the inode's 60-byte i_block, so it holds a header plus
 * four slots. Every other node is a whole filesystem block.
 *
 * Depth is bounded because the kernel bounds it (EXT4_MAX_EXTENT_DEPTH). It
 * is a generous bound rather than a tight one: with 4 KiB blocks a node holds
 * 340 slots, so a depth-2 tree already addresses 4 * 340 * 340 extents of up
 * to 32768 blocks each, far past the 2^32 logical blocks an extent can name.
 * Anything deeper is corruption, and capping it is also what stops a cyclic
 * tree from spinning the descent loop forever. */
#define EXT_MAGIC               0xF30Au
#define EXT_HDR_SIZE            12u
#define EXT_ENT_SIZE            12u
#define EXT_MAX_DEPTH           5u
#define EXT_INIT_MAX_LEN        32768u /* longer means "uninitialized" */

#define EH_MAGIC                0x00u
#define EH_ENTRIES              0x02u
#define EH_MAX                  0x04u
#define EH_DEPTH                0x06u

#define EI_BLOCK                0x00u
#define EI_LEAF_LO              0x04u
#define EI_LEAF_HI              0x08u

#define EE_BLOCK                0x00u
#define EE_LEN                  0x04u
#define EE_START_HI             0x06u
#define EE_START_LO             0x08u

/* Directory entry. */
#define DE_INODE                0x00u
#define DE_REC_LEN              0x04u
#define DE_NAME_LEN             0x06u
#define DE_FILE_TYPE            0x07u
#define DE_NAME                 8u
#define DE_TAIL_SIZE            12u
#define DE_TAIL_MAGIC_FT        0xDEu
#define DE_TAIL_CSUM            8u

#define FT_REG                  1u
#define FT_DIR                  2u

/* crc32c (Castagnoli, reflected), nibble table: 64 bytes of rodata and two
 * lookups per byte. Nothing here is on a hot path (ext4 checksums metadata
 * only, never file data), so the 1 KiB byte-wide table would buy nothing.
 * Entry i is the reflected polynomial 0x82F63B78 folded four times over i. */
static const uint32_t crc32c_tab[16] = {
    0x00000000U, 0x105EC76FU, 0x20BD8EDEU, 0x30E349B1U,
    0x417B1DBCU, 0x5125DAD3U, 0x61C69362U, 0x7198540DU,
    0x82F63B78U, 0x92A8FC17U, 0xA24BB5A6U, 0xB21572C9U,
    0xC38D26C4U, 0xD3D3E1ABU, 0xE330A81AU, 0xF36E6F75U,
};

/* Running update, matching the kernel's ext4_chksum(): no initial or final
 * inversion, so callers chain pieces of a structure together. */
static uint32_t crc32c(uint32_t crc, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ crc32c_tab[crc & 0xFu];
        crc = (crc >> 4) ^ crc32c_tab[crc & 0xFu];
    }
    return crc;
}

/* Feed a little-endian u32 (inode number, group number, generation) the way
 * the kernel does, which is by hashing the on-disk representation. */
static uint32_t crc32c_le32(uint32_t crc, uint32_t val)
{
    uint8_t b[4];

    b[0] = (uint8_t)(val & 0xFFu);
    b[1] = (uint8_t)((val >> 8) & 0xFFu);
    b[2] = (uint8_t)((val >> 16) & 0xFFu);
    b[3] = (uint8_t)((val >> 24) & 0xFFu);
    return crc32c(crc, b, sizeof(b));
}

static uint32_t crc32c_zeros(uint32_t crc, size_t n)
{
    static const uint8_t zeros[4] = { 0, 0, 0, 0 };

    return crc32c(crc, zeros, n);
}

static uint64_t block_off(const ext4_vol *v, uint64_t blk)
{
    return blk * v->block_size;
}

/* Read one filesystem block of metadata through the volume's block cache. The
 * cache is keyed by byte offset, so interleaved use by the inode reader and
 * the extent walker is correct by construction; only locality is at stake. */
static int meta_block(ext4_vol *v, uint64_t blk, const uint8_t **out)
{
    uint64_t off;

    if (blk == 0 || blk >= v->blocks_count)
        return EXT4_ERR_CORRUPT;

    off = block_off(v, blk);
    if (v->blk_cache_off != off) {
        if (v->read(v->read_ctx, off, v->block_size, v->blk_cache) != 0) {
            v->blk_cache_off = OFF_INVALID;
            return EXT4_ERR_IO;
        }
        v->blk_cache_off = off;
    }
    *out = v->blk_cache;
    return EXT4_OK;
}

static int group_desc(ext4_vol *v, uint32_t group, const uint8_t **out)
{
    uint64_t off;
    uint8_t buf[64];
    uint32_t want;
    uint32_t got;

    if (group >= v->group_count)
        return EXT4_ERR_CORRUPT;

    if (v->gd_cache_group != group) {
        off = v->gdt_off + (uint64_t)group * v->desc_size;
        /* Descriptors are 32 or 64 bytes, so several share a 512-byte
         * sector, and that sector is what is read: on a 512-byte disk it is
         * a whole device sector and the callback's re-blocking layer (DESIGN
         * Section 3.4) passes it straight through.
         *
         * One sector always suffices: gdt_off is block-aligned, desc_size is
         * a power of two no larger than 64 (checked at mount), and both 32
         * and 64 divide 512, so a descriptor cannot straddle a sector. */
        {
            uint64_t sec = off & ~(uint64_t)511;
            uint8_t sector[512];

            if (v->read(v->read_ctx, sec, sizeof(sector), sector) != 0)
                return EXT4_ERR_IO;
            memcpy(buf, sector + (off - sec), v->desc_size);
        }

        if (v->has_csum) {
            got = get_le16(buf + GD_CHECKSUM);
            want = crc32c_le32(v->csum_seed, group);
            want = crc32c(want, buf, GD_CHECKSUM);
            want = crc32c_zeros(want, 2);
            if (v->desc_size > GD_CHECKSUM + 2u)
                want = crc32c(want, buf + GD_CHECKSUM + 2u,
                              v->desc_size - (GD_CHECKSUM + 2u));
            if ((want & 0xFFFFu) != got)
                return EXT4_ERR_CSUM;
        }

        memcpy(v->gd_cache, buf, sizeof(v->gd_cache));
        v->gd_cache_group = group;
    }
    *out = v->gd_cache;
    return EXT4_OK;
}

int ext4_mount(ext4_vol *v, ext4_read_fn read, void *read_ctx)
{
    uint8_t sb[SB_SIZE];
    uint32_t incompat;
    uint32_t ro_compat;
    uint32_t log_bs;
    uint32_t state;
    uint64_t groups;

    if (v == (void *)0 || read == (void *)0)
        return EXT4_ERR_BAD_ARG;

    memset(v, 0, sizeof(*v));
    v->read = read;
    v->read_ctx = read_ctx;
    v->blk_cache_off = OFF_INVALID;
    v->gd_cache_group = IDX_INVALID;

    if (read(read_ctx, SB_OFF, SB_SIZE, sb) != 0)
        return EXT4_ERR_IO;

    if (get_le16(sb + SB_MAGIC) != EXT4_MAGIC)
        return EXT4_ERR_BAD_SUPER;
    if (get_le32(sb + SB_REV_LEVEL) < 1)
        return EXT4_ERR_UNSUPPORTED; /* rev 0 has no feature flags at all */

    /* Compat bits (has_journal, ext_attr, resize_inode, dir_index,
     * orphan_file) never change how bytes are read, so they are not even
     * fetched; SB_FEATURE_COMPAT exists above only to document the layout. */
    incompat = get_le32(sb + SB_FEATURE_INCOMPAT);
    ro_compat = get_le32(sb + SB_FEATURE_RO_COMPAT);

    /* Checksum first: every later field is only worth trusting once the
     * superblock is known to be intact. */
    if ((ro_compat & RO_COMPAT_METADATA_CSUM) != 0) {
        if (sb[SB_CHECKSUM_TYPE] != EXT4_CHECKSUM_CRC32C)
            return EXT4_ERR_UNSUPPORTED;
        if (crc32c(0xFFFFFFFFu, sb, SB_CHECKSUM) != get_le32(sb + SB_CHECKSUM))
            return EXT4_ERR_CSUM;
        v->has_csum = 1;
    }

    /* RECOVER is an incompat bit, but it means "the journal holds committed
     * data the filesystem does not", which deserves its own answer rather than
     * the generic unsupported-feature one. */
    if ((incompat & INCOMPAT_RECOVER) != 0)
        return EXT4_ERR_NEEDS_RECOVERY;
    if ((ro_compat & RO_COMPAT_ORPHAN_PRESENT) != 0)
        return EXT4_ERR_DIRTY;
    if ((incompat & ~INCOMPAT_SUPPORTED) != 0)
        return EXT4_ERR_UNSUPPORTED;
    if ((ro_compat & ~RO_COMPAT_SUPPORTED) != 0)
        return EXT4_ERR_UNSUPPORTED;
    if ((incompat & INCOMPAT_EXTENTS) == 0 ||
        (incompat & INCOMPAT_FILETYPE) == 0)
        return EXT4_ERR_UNSUPPORTED;

    /* s_state alone is not enough and is checked last: a filesystem snapshot
     * taken mid-write still reads "clean" here while the incompat RECOVER bit
     * above is what actually says the journal holds the truth. */
    state = get_le16(sb + SB_STATE);
    if ((state & EXT4_VALID_FS) == 0 ||
        (state & (EXT4_ERROR_FS | EXT4_ORPHAN_FS)) != 0)
        return EXT4_ERR_DIRTY;

    log_bs = get_le32(sb + SB_LOG_BLOCK_SIZE);
    if (log_bs > 2u) /* 1024 << 2 == EXT4_BLOCK_MAX */
        return EXT4_ERR_UNSUPPORTED;
    v->block_size = 1024u << log_bs;

    v->inode_size = get_le16(sb + SB_INODE_SIZE);
    if (v->inode_size < INO_GOOD_OLD_SIZE || v->inode_size > v->block_size ||
        (v->inode_size & (v->inode_size - 1u)) != 0)
        return EXT4_ERR_BAD_SUPER;

    v->inodes_per_group = get_le32(sb + SB_INODES_PER_GROUP);
    v->blocks_per_group = get_le32(sb + SB_BLOCKS_PER_GROUP);
    v->first_data_block = get_le32(sb + SB_FIRST_DATA_BLOCK);
    v->inodes_count = get_le32(sb + SB_INODES_COUNT);
    v->blocks_count = (uint64_t)get_le32(sb + SB_BLOCKS_COUNT_LO) |
                      ((uint64_t)get_le32(sb + SB_BLOCKS_COUNT_HI) << 32);

    if (v->inodes_per_group == 0 || v->blocks_per_group == 0 ||
        v->inodes_count == 0 || v->blocks_count == 0)
        return EXT4_ERR_BAD_SUPER;
    if (v->first_data_block != (v->block_size == 1024u ? 1u : 0u))
        return EXT4_ERR_BAD_SUPER;

    v->desc_size = ((incompat & INCOMPAT_64BIT) != 0)
                       ? get_le16(sb + SB_DESC_SIZE) : 32u;
    if (v->desc_size < 32u || v->desc_size > sizeof(v->gd_cache) ||
        (v->desc_size & (v->desc_size - 1u)) != 0)
        return EXT4_ERR_BAD_SUPER;

    groups = (v->blocks_count - v->first_data_block + v->blocks_per_group - 1) /
             v->blocks_per_group;
    if (groups == 0 || groups > 0xFFFFFFFFu)
        return EXT4_ERR_BAD_SUPER;
    v->group_count = (uint32_t)groups;

    /* The descriptor table sits in the block after the superblock, which is
     * block 1 for every block size except 1 KiB (where the superblock is
     * block 1 and the table is block 2). META_BG, which moves it, is refused
     * above. */
    v->gdt_off = block_off(v, (uint64_t)v->first_data_block + 1u);

    if (v->has_csum) {
        v->csum_seed = ((incompat & INCOMPAT_CSUM_SEED) != 0)
                           ? get_le32(sb + SB_CHECKSUM_SEED)
                           : crc32c(0xFFFFFFFFu, sb + SB_UUID, 16);
    }
    return EXT4_OK;
}

/* Fill out from the on-disk inode, verifying its checksum. */
static int inode_read(ext4_vol *v, uint32_t ino, ext4_inode *out)
{
    const uint8_t *gd;
    const uint8_t *raw;
    uint64_t itable;
    uint64_t blk;
    uint32_t group;
    uint32_t index;
    uint32_t in_block;
    int rc;

    if (ino == 0 || ino > v->inodes_count)
        return EXT4_ERR_BAD_ARG;

    group = (ino - 1u) / v->inodes_per_group;
    index = (ino - 1u) % v->inodes_per_group;

    rc = group_desc(v, group, &gd);
    if (rc != EXT4_OK)
        return rc;

    itable = get_le32(gd + GD_INODE_TABLE_LO);
    if (v->desc_size > GD_INODE_TABLE_HI)
        itable |= (uint64_t)get_le32(gd + GD_INODE_TABLE_HI) << 32;

    /* inode_size is a power of two no larger than the block size, so an inode
     * never straddles a block boundary and one block read always suffices. */
    blk = itable + (uint64_t)index / (v->block_size / v->inode_size);
    in_block = (index % (v->block_size / v->inode_size)) * v->inode_size;

    rc = meta_block(v, blk, &raw);
    if (rc != EXT4_OK)
        return rc;
    raw += in_block;

    memset(out, 0, sizeof(*out));
    out->ino = ino;
    out->mode = get_le16(raw + INO_MODE);
    out->flags = get_le32(raw + INO_FLAGS);
    out->generation = get_le32(raw + INO_GENERATION);
    out->size = (uint64_t)get_le32(raw + INO_SIZE_LO);
    if ((out->mode & S_IFMT) != S_IFDIR)
        out->size |= (uint64_t)get_le32(raw + INO_SIZE_HIGH) << 32;
    memcpy(out->root, raw + INO_BLOCK, INO_BLOCK_SIZE);

    if (v->has_csum) {
        uint32_t extra = (v->inode_size > INO_GOOD_OLD_SIZE)
                             ? get_le16(raw + INO_EXTRA_ISIZE) : 0u;
        uint32_t expect = get_le16(raw + INO_CHECKSUM_LO);
        uint32_t have;
        uint32_t off;
        int wide = 0;

        out->csum_seed = crc32c_le32(v->csum_seed, ino);
        out->csum_seed = crc32c_le32(out->csum_seed, out->generation);

        have = crc32c(out->csum_seed, raw, INO_CHECKSUM_LO);
        have = crc32c_zeros(have, 2);
        off = INO_CHECKSUM_LO + 2u;
        have = crc32c(have, raw + off, INO_GOOD_OLD_SIZE - off);
        if (v->inode_size > INO_GOOD_OLD_SIZE) {
            have = crc32c(have, raw + INO_GOOD_OLD_SIZE,
                          INO_CHECKSUM_HI - INO_GOOD_OLD_SIZE);
            off = INO_CHECKSUM_HI;
            /* The high half only exists if i_extra_isize actually reaches it,
             * which is the kernel's EXT4_FITS_IN_INODE test. */
            if (INO_GOOD_OLD_SIZE + extra >= INO_CHECKSUM_HI + 2u) {
                expect |= (uint32_t)get_le16(raw + INO_CHECKSUM_HI) << 16;
                have = crc32c_zeros(have, 2);
                off += 2u;
                wide = 1;
            }
            have = crc32c(have, raw + off, v->inode_size - off);
        }
        if (!wide)
            have &= 0xFFFFu;
        if (have != expect)
            return EXT4_ERR_CSUM;
    } else {
        out->csum_seed = 0;
    }

    if ((out->flags & INODE_FL_INLINE_DATA) != 0)
        return EXT4_ERR_INLINE;
    if ((out->mode & S_IFMT) == S_IFLNK)
        return EXT4_ERR_SYMLINK;
    if ((out->flags & INODE_FL_EXTENTS) == 0)
        return EXT4_ERR_UNSUPPORTED; /* old indirect block map */
    return EXT4_OK;
}

/* Validate an extent tree node in place. in_inode selects the 60-byte root,
 * which has no checksum tail of its own (the inode's checksum covers it).
 *
 * avail is how many bytes the node is allowed to occupy: 60 for the root, one
 * filesystem block otherwise. Bounding eh_max against it is what stops a
 * forged header from walking the entry loops off the end of the inode or the
 * block, so this runs before any caller reads a single slot. A block node
 * must also leave four bytes past its last slot for the checksum tail. */
static int extent_node_check(const ext4_vol *v, const ext4_inode *in,
                             const uint8_t *node, uint32_t avail, int in_inode)
{
    uint32_t entries;
    uint32_t max;

    if (get_le16(node + EH_MAGIC) != EXT_MAGIC)
        return EXT4_ERR_CORRUPT;
    entries = get_le16(node + EH_ENTRIES);
    max = get_le16(node + EH_MAX);
    if (entries > max)
        return EXT4_ERR_CORRUPT;
    if (EXT_HDR_SIZE + EXT_ENT_SIZE * max > avail)
        return EXT4_ERR_CORRUPT;

    if (!in_inode && v->has_csum) {
        uint32_t tail = EXT_HDR_SIZE + EXT_ENT_SIZE * max;

        /* Unreachable at every block size ext4 permits, and kept anyway.
         *
         * Reaching it needs a node that passed the bound above (tail <=
         * avail) and still has no room for the four-byte sum (tail + 4 >
         * avail), so avail - tail < 4. tail is 12 * (max + 1) and avail is
         * the block size, and 1024, 2048 and 4096 leave remainders of 4, 8
         * and 4 against 12: the largest tail that fits always leaves at
         * least four bytes spare. No value of eh_max can land in the gap.
         *
         * It stays because the arithmetic that makes it dead is the block
         * size's, not this function's, and a reader arriving here should
         * find the reason rather than spend an afternoon building a node
         * that triggers it. */
        if (tail + 4u > avail)
            return EXT4_ERR_CORRUPT;
        if (crc32c(in->csum_seed, node, tail) != get_le32(node + tail))
            return EXT4_ERR_CSUM;
    }
    return EXT4_OK;
}

/* Map a logical block to its physical block, plus how many blocks follow it
 * contiguously. The resolved extent is cached in the inode, so a sequential
 * walk through a 90 MiB UKI costs one tree descent per extent, not per block.
 *
 * Shape of the descent: start at the root in i_block, read eh_depth once, and
 * take that many steps down, checking each node as it is read. Both loops are
 * linear scans rather than binary searches. A node holds at most 340 slots and
 * the cursor above makes the whole descent rare, so the branch-free scan is
 * the cheaper thing to get right.
 *
 * Every exit that is not a real mapping is EXT4_ERR_HOLE, which the caller
 * turns into a refusal: falling below the first indexed block, landing in the
 * gap between two extents, and a preallocated extent whose contents are
 * undefined. A read-only driver has nothing useful to return for any of them,
 * and returning zeroes would be a lie about what is on disk. */
static int extent_lookup(ext4_vol *v, ext4_inode *in, uint32_t lblk,
                         uint64_t *pblk, uint32_t *run)
{
    const uint8_t *node = in->root;
    uint32_t avail = INO_BLOCK_SIZE;
    uint32_t depth;
    uint32_t i;
    int in_inode = 1;
    int rc;

    if (in->ext_len != 0 && lblk >= in->ext_lblk &&
        lblk - in->ext_lblk < in->ext_len) {
        *pblk = in->ext_pblk + (lblk - in->ext_lblk);
        *run = in->ext_len - (lblk - in->ext_lblk);
        return EXT4_OK;
    }

    rc = extent_node_check(v, in, node, avail, in_inode);
    if (rc != EXT4_OK)
        return rc;
    depth = get_le16(node + EH_DEPTH);
    if (depth > EXT_MAX_DEPTH)
        return EXT4_ERR_CORRUPT;

    while (depth > 0) {
        uint32_t entries = get_le16(node + EH_ENTRIES);
        uint32_t pick = IDX_INVALID;
        uint64_t child;

        /* Index entries are sorted; the child to descend into is the last one
         * whose first logical block is at or below the target. */
        for (i = 0; i < entries; i++) {
            const uint8_t *e = node + EXT_HDR_SIZE + i * EXT_ENT_SIZE;

            if (get_le32(e + EI_BLOCK) > lblk)
                break;
            pick = i;
        }
        if (pick == IDX_INVALID)
            return EXT4_ERR_HOLE; /* below the first indexed block */

        {
            const uint8_t *e = node + EXT_HDR_SIZE + pick * EXT_ENT_SIZE;

            child = (uint64_t)get_le32(e + EI_LEAF_LO) |
                    ((uint64_t)get_le16(e + EI_LEAF_HI) << 32);
        }

        rc = meta_block(v, child, &node);
        if (rc != EXT4_OK)
            return rc;
        avail = v->block_size;
        in_inode = 0;

        rc = extent_node_check(v, in, node, avail, in_inode);
        if (rc != EXT4_OK)
            return rc;
        /* Each step down must drop the depth by exactly one. Without this a
         * node pointing at itself, or at any node of equal or greater depth,
         * would keep the loop descending until EXT_MAX_DEPTH saved it; the
         * check turns that into an immediate, accurate answer. */
        if (get_le16(node + EH_DEPTH) != depth - 1u)
            return EXT4_ERR_CORRUPT;
        depth--;
    }

    {
        uint32_t entries = get_le16(node + EH_ENTRIES);

        for (i = 0; i < entries; i++) {
            const uint8_t *e = node + EXT_HDR_SIZE + i * EXT_ENT_SIZE;
            uint32_t first = get_le32(e + EE_BLOCK);
            uint32_t len = get_le16(e + EE_LEN);

            if (first > lblk)
                break;
            /* A length above the initialized limit marks a preallocated,
             * never-written extent. Its blocks are allocated but their
             * contents are undefined, so reading them is refused rather than
             * silently served. */
            if (len > EXT_INIT_MAX_LEN)
                return EXT4_ERR_HOLE;
            if (len == 0 || lblk - first >= len)
                continue;

            in->ext_lblk = first;
            in->ext_len = len;
            in->ext_pblk = (uint64_t)get_le32(e + EE_START_LO) |
                           ((uint64_t)get_le16(e + EE_START_HI) << 32);
            if (in->ext_pblk == 0 ||
                in->ext_pblk + len > v->blocks_count)
                return EXT4_ERR_CORRUPT;
            *pblk = in->ext_pblk + (lblk - first);
            *run = len - (lblk - first);
            return EXT4_OK;
        }
    }
    return EXT4_ERR_HOLE;
}

static int dir_open_inode(ext4_vol *v, ext4_dir *d, uint32_t ino)
{
    int rc;

    memset(d, 0, sizeof(*d));
    d->vol = v;
    d->buf_lblk = IDX_INVALID;

    rc = inode_read(v, ino, &d->inode);
    if (rc != EXT4_OK)
        return rc;
    if ((d->inode.mode & S_IFMT) != S_IFDIR)
        return EXT4_ERR_BAD_ARG;
    /* Directory blocks are allocated whole, so i_size is always a multiple of
     * the block size. The iterator relies on it: it bounds itself on i_size
     * alone, so a ragged tail would let it walk a partial block and read
     * rec_len out of whatever followed. */
    if (d->inode.size % v->block_size != 0)
        return EXT4_ERR_CORRUPT;
    return EXT4_OK;
}

int ext4_dir_open_root(ext4_vol *v, ext4_dir *d)
{
    if (v == (void *)0 || d == (void *)0)
        return EXT4_ERR_BAD_ARG;
    return dir_open_inode(v, d, EXT4_ROOT_INO);
}

int ext4_dir_open_ino(ext4_vol *v, ext4_dir *d, uint32_t ino)
{
    if (v == (void *)0 || d == (void *)0)
        return EXT4_ERR_BAD_ARG;
    return dir_open_inode(v, d, ino);
}

int ext4_dir_open(ext4_vol *v, ext4_dir *d, const ext4_dirent *e)
{
    if (v == (void *)0 || d == (void *)0 || e == (void *)0)
        return EXT4_ERR_BAD_ARG;
    if (!e->is_dir)
        return EXT4_ERR_BAD_ARG;
    return dir_open_inode(v, d, e->ino);
}

/* Does this block's record chain end on an ext4_dir_entry_tail, the 12-byte
 * record shaped like an unused entry with the impossible file type 0xDE?
 *
 * The tail's four-byte signature is NOT sufficient on its own: e2fsprogs
 * writes an htree node's dx_tail reserved word as the same 0C 00 00 DE, so the
 * last twelve bytes of an index block are byte-identical to a checksum tail
 * while the checksum before them is computed over the index entries by a
 * different rule entirely. What actually distinguishes the two is the record
 * chain, which a leaf ends exactly on the tail and an index block runs past
 * (an htree root's ".." spans the index area, and a dx_node is a single fake
 * record covering the whole block). Walking it also validates every rec_len
 * before the iterator trusts one. */
static int dir_block_has_tail(const ext4_vol *v, const uint8_t *buf)
{
    uint32_t bs = v->block_size;
    uint32_t off = 0;

    while (off < bs - DE_TAIL_SIZE) {
        uint32_t rec_len = get_le16(buf + off + DE_REC_LEN);

        if (rec_len < DE_TAIL_SIZE || (rec_len & 3u) != 0 || off + rec_len > bs)
            return 0;
        off += rec_len;
    }
    if (off != bs - DE_TAIL_SIZE)
        return 0;

    return get_le32(buf + off + DE_INODE) == 0 &&
           get_le16(buf + off + DE_REC_LEN) == DE_TAIL_SIZE &&
           buf[off + DE_NAME_LEN] == 0 &&
           buf[off + DE_FILE_TYPE] == DE_TAIL_MAGIC_FT;
}

/* Verify a directory block's checksum when it carries one. htree interior
 * blocks carry a dx_tail instead, which hashes the index entries this driver
 * deliberately never reads, so they are left unverified (ext4.h states the
 * policy). */
static int dir_block_check(const ext4_vol *v, const ext4_inode *in,
                           const uint8_t *buf)
{
    if (!v->has_csum || !dir_block_has_tail(v, buf))
        return EXT4_OK;

    if (crc32c(in->csum_seed, buf, v->block_size - DE_TAIL_SIZE) !=
        get_le32(buf + v->block_size - DE_TAIL_SIZE + DE_TAIL_CSUM))
        return EXT4_ERR_CSUM;
    return EXT4_OK;
}

int ext4_dir_next(ext4_dir *d, ext4_dirent *out)
{
    ext4_vol *v;
    uint32_t bs;

    if (d == (void *)0 || d->vol == (void *)0 || out == (void *)0)
        return EXT4_ERR_BAD_ARG;

    v = d->vol;
    bs = v->block_size;

    for (;;) {
        uint32_t lblk;
        uint32_t in_block;
        const uint8_t *de;
        uint32_t rec_len;
        uint32_t ino;
        uint8_t name_len;
        uint8_t type;

        if (d->pos + DE_NAME > d->inode.size)
            return 0;
        if (d->pos >= EXT4_DIR_MAX)
            return EXT4_ERR_UNSUPPORTED;

        lblk = (uint32_t)(d->pos / bs);
        in_block = (uint32_t)(d->pos % bs);

        if (d->buf_lblk != lblk) {
            uint64_t pblk;
            uint32_t run;
            int rc = extent_lookup(v, &d->inode, lblk, &pblk, &run);

            if (rc != EXT4_OK)
                return rc;
            if (v->read(v->read_ctx, block_off(v, pblk), bs, d->buf) != 0) {
                d->buf_lblk = IDX_INVALID;
                return EXT4_ERR_IO;
            }
            rc = dir_block_check(v, &d->inode, d->buf);
            if (rc != EXT4_OK) {
                d->buf_lblk = IDX_INVALID;
                return rc;
            }
            d->buf_lblk = lblk;
        }

        de = d->buf + in_block;
        rec_len = get_le16(de + DE_REC_LEN);
        ino = get_le32(de + DE_INODE);
        name_len = de[DE_NAME_LEN];
        type = de[DE_FILE_TYPE];

        if (rec_len < DE_NAME + 4u || (rec_len & 3u) != 0 ||
            in_block + rec_len > bs)
            return EXT4_ERR_CORRUPT;
        d->pos += rec_len;

        /* Unused padding, the checksum tail, the fake entry that fills an
         * htree interior block, and the root block's ".." entry that spans the
         * index area all present as entries with no inode. Skipping them is
         * what makes a linear walk of an indexed directory correct. */
        if (ino == 0)
            continue;
        if (name_len == 0 || DE_NAME + name_len > rec_len)
            return EXT4_ERR_CORRUPT;
        if (ino > v->inodes_count)
            return EXT4_ERR_CORRUPT;

        if ((name_len == 1 && de[DE_NAME] == '.') ||
            (name_len == 2 && de[DE_NAME] == '.' && de[DE_NAME + 1] == '.'))
            continue;

        memset(out, 0, sizeof(*out));
        memcpy(out->name, de + DE_NAME, name_len);
        out->name[name_len] = 0;
        out->name_len = name_len;
        out->ino = ino;
        out->file_type = type;
        out->is_dir = (uint8_t)(type == FT_DIR);
        out->is_regular = (uint8_t)(type == FT_REG);
        return 1;
    }
}

static int file_open_inode(ext4_vol *v, ext4_file *f, uint32_t ino)
{
    int rc;

    memset(f, 0, sizeof(*f));
    f->vol = v;
    f->buf_off = OFF_INVALID;

    rc = inode_read(v, ino, &f->inode);
    if (rc != EXT4_OK)
        return rc;
    if ((f->inode.mode & S_IFMT) != S_IFREG)
        return EXT4_ERR_BAD_ARG;
    return EXT4_OK;
}

int ext4_file_open_ino(ext4_vol *v, ext4_file *f, uint32_t ino)
{
    if (v == (void *)0 || f == (void *)0)
        return EXT4_ERR_BAD_ARG;
    return file_open_inode(v, f, ino);
}

int ext4_file_open(ext4_vol *v, ext4_file *f, const ext4_dirent *e)
{
    if (v == (void *)0 || f == (void *)0 || e == (void *)0)
        return EXT4_ERR_BAD_ARG;
    if (e->is_dir)
        return EXT4_ERR_BAD_ARG;
    /* The directory entry already says what this is; refusing here keeps the
     * distinct status instead of letting the inode read produce it later. */
    if (!e->is_regular && e->file_type != 0)
        return (e->file_type == 7u) ? EXT4_ERR_SYMLINK : EXT4_ERR_BAD_ARG;
    return file_open_inode(v, f, e->ino);
}

uint64_t ext4_file_size(const ext4_file *f)
{
    return (f == (void *)0) ? 0 : f->inode.size;
}

/* Move run bytes from volume offset abs into dst. Whole blocks go straight to
 * the caller's buffer; only the ragged ends of an unaligned request pay for
 * the bounce buffer.
 *
 * The split between this and ext4_file_read is one of coordinates. That one
 * works in logical blocks and calls the extent tree, so it is where holes and
 * tree descents happen; this one has a physical byte range already known to be
 * contiguous, so it only has to read whole filesystem blocks where it can.
 * Keeping them apart is why a 90 MiB read copies through f->buf at
 * most twice rather than once per extent. */
static int file_read_span(ext4_file *f, uint64_t abs, uint64_t run,
                          uint8_t *dst)
{
    ext4_vol *v = f->vol;
    uint32_t bs = v->block_size;

    while (run > 0) {
        uint64_t blk_off = abs - (abs % bs);
        uint32_t in_blk = (uint32_t)(abs - blk_off);

        if (in_blk == 0 && run >= bs) {
            uint64_t whole = (run / bs) * bs;

            if (v->read(v->read_ctx, abs, whole, dst) != 0)
                return EXT4_ERR_IO;
            dst += whole;
            abs += whole;
            run -= whole;
        } else {
            uint32_t take = bs - in_blk;

            if ((uint64_t)take > run)
                take = (uint32_t)run;
            if (f->buf_off != blk_off) {
                if (v->read(v->read_ctx, blk_off, bs, f->buf) != 0) {
                    f->buf_off = OFF_INVALID;
                    return EXT4_ERR_IO;
                }
                f->buf_off = blk_off;
            }
            memcpy(dst, f->buf + in_blk, take);
            dst += take;
            abs += take;
            run -= take;
        }
    }
    return EXT4_OK;
}

int ext4_file_read(ext4_file *f, uint64_t off, uint64_t len, void *buf)
{
    ext4_vol *v;
    uint8_t *dst = buf;
    uint32_t bs;

    if (f == (void *)0 || f->vol == (void *)0)
        return EXT4_ERR_BAD_ARG;
    if (buf == (void *)0 && len != 0)
        return EXT4_ERR_BAD_ARG;
    if (off > f->inode.size || len > f->inode.size - off)
        return EXT4_ERR_RANGE;
    if (len == 0)
        return EXT4_OK;

    v = f->vol;
    bs = v->block_size;

    while (len > 0) {
        uint64_t lblk = off / bs;
        uint32_t in_blk = (uint32_t)(off % bs);
        uint64_t pblk;
        uint32_t run;
        uint64_t avail;
        uint64_t take;
        int rc;

        if (lblk > 0xFFFFFFFFu)
            return EXT4_ERR_RANGE; /* extents address 32 bits of logical block */
        rc = extent_lookup(v, &f->inode, (uint32_t)lblk, &pblk, &run);
        if (rc != EXT4_OK)
            return rc;

        avail = (uint64_t)run * bs - in_blk;
        take = (avail < len) ? avail : len;

        rc = file_read_span(f, block_off(v, pblk) + in_blk, take, dst);
        if (rc != EXT4_OK)
            return rc;
        dst += take;
        off += take;
        len -= take;
    }
    return EXT4_OK;
}
