/* Read-only FAT12/16/32. See falcon-core/src/fs/fat.h for what this driver
 * supports, what it declines, and why the type rule's constants are exact. */

#include <fs/fat.h>

#include <lib/string.h>

/* ---- BPB offsets (Microsoft FAT specification, "Boot Sector and BPB") ---- */
#define BPB_JMP           0u
#define BPB_BYTES_PER_SEC 11u
#define BPB_SEC_PER_CLUS  13u
#define BPB_RSVD_SEC_CNT  14u
#define BPB_NUM_FATS      16u
#define BPB_ROOT_ENT_CNT  17u
#define BPB_TOT_SEC_16    19u
#define BPB_FAT_SZ_16     22u
#define BPB_TOT_SEC_32    32u
/* FAT32 extended BPB */
#define BPB32_FAT_SZ_32   36u
#define BPB32_ROOT_CLUS   44u
#define BPB32_BOOT_SIG    66u
#define BPB32_VOL_LAB     71u
/* FAT12/16 extended BPB */
#define BPB16_BOOT_SIG    38u
#define BPB16_VOL_LAB     43u

#define EXT_BOOT_SIG      0x29u

/* ---- directory entry offsets ---- */
#define DIR_ENTRY_SIZE    32u
#define DE_NAME           0u     /* 11 bytes, 8.3, space padded */
#define DE_ATTR           11u
#define DE_NT_RES         12u    /* the lower-case flags Windows NT writes */
#define DE_FST_CLUS_HI    20u
#define DE_FST_CLUS_LO    26u
#define DE_FILE_SIZE      28u

#define ATTR_READ_ONLY    0x01u
#define ATTR_HIDDEN       0x02u
#define ATTR_SYSTEM       0x04u
#define ATTR_VOLUME_ID    0x08u
#define ATTR_DIRECTORY    0x10u
#define ATTR_LONG_NAME    0x0Fu  /* RO|HIDDEN|SYSTEM|VOLUME_ID together */

#define NT_RES_LOWER_BASE 0x08u
#define NT_RES_LOWER_EXT  0x10u

#define DE_FREE           0xE5u  /* deleted */
#define DE_END            0x00u  /* this and every following entry are free */
#define DE_KANJI_E5       0x05u  /* a real 0xE5 first byte, escaped */

/* ---- long name entry offsets ---- */
#define LDIR_ORD          0u
#define LDIR_ATTR         11u
#define LDIR_TYPE         12u
#define LDIR_CHKSUM       13u
#define LDIR_LAST         0x40u  /* set on the entry read FIRST */
#define LDIR_SEQ_MASK     0x1Fu
#define LDIR_MAX_SEQ      20u    /* 20 * 13 = 260, capped to FAT_NAME_MAX */
#define LDIR_CHARS        13u

/* Where an LFN record keeps its thirteen UCS-2 units. Three runs, because the
 * layout was fitted around the fields of the 8.3 entry it impersonates. */
static const uint8_t lfn_runs[3][2] = { { 1u, 5u }, { 14u, 6u }, { 28u, 2u } };

/* ---- end-of-chain and bad-cluster marks, per width ---- */
#define EOC_12            0x0FF8u
#define EOC_16            0xFFF8u
#define EOC_32            0x0FFFFFF8u
#define CLUSTER_MIN       2u

static int is_pow2(uint32_t v)
{
    return v != 0 && (v & (v - 1u)) == 0;
}

/* Every read goes through here, so the sector-alignment contract of fat.h is
 * stated once and cannot be forgotten at a call site. */
static int read_sector(fat_vol *v, uint64_t sector, void *buf)
{
    return v->read(v->read_ctx, sector * v->sector_size, v->sector_size,
                   buf) == 0 ? FAT_OK : FAT_ERR_IO;
}

/* One byte of the first FAT, through the sector cache.
 *
 * A byte at a time rather than a word, because a FAT12 entry is twelve bits
 * and the pair of bytes holding it can straddle a sector boundary. Reading
 * the two bytes independently is what makes that case ordinary instead of a
 * special one, and the cache means the straddle costs one extra lookup and
 * not an extra read. */
static int fat_byte(fat_vol *v, uint64_t off, uint8_t *out)
{
    uint64_t sector = v->fat_off_sectors + off / v->sector_size;
    int rc;

    if (off / v->sector_size >= v->fat_sectors)
        return FAT_ERR_CORRUPT;
    if (v->fat_cache_off != sector) {
        rc = read_sector(v, sector, v->fat_cache);
        if (rc != FAT_OK) {
            v->fat_cache_off = ~(uint64_t)0;
            return rc;
        }
        v->fat_cache_off = sector;
    }
    *out = v->fat_cache[off % v->sector_size];
    return FAT_OK;
}

/* n consecutive FAT bytes, each through the cache.
 *
 * The buffer is cleared first, which is not defensive padding: gcc
 * -fanalyzer cannot follow the loop well enough to see that a successful
 * return has filled every byte, and it reports b[1] as uninitialized at the
 * caller's get_le32. Clearing here is cheaper than baselining a warning on
 * the one function in this driver that decides which cluster comes next, and
 * a partially filled buffer is never read anyway: every caller checks the
 * status first. */
static int fat_bytes(fat_vol *v, uint64_t off, unsigned n, uint8_t b[4])
{
    unsigned i;

    b[0] = 0;
    b[1] = 0;
    b[2] = 0;
    b[3] = 0;
    for (i = 0; i < n; i++) {
        int rc = fat_byte(v, off + i, &b[i]);

        if (rc != FAT_OK)
            return rc;
    }
    return FAT_OK;
}

/* The FAT entry for a cluster, widened to 32 bits. */
static int fat_entry(fat_vol *v, uint32_t cluster, uint32_t *out)
{
    uint8_t b[4];
    int rc;

    if (cluster < CLUSTER_MIN || cluster >= v->clusters + CLUSTER_MIN)
        return FAT_ERR_CORRUPT;

    switch (v->type) {
    case FAT_TYPE_12:
        /* One and a half bytes per entry, so the pair holding this one
         * starts at cluster + cluster/2. */
        rc = fat_bytes(v, (uint64_t)cluster + (cluster / 2u), 2u, b);
        if (rc != FAT_OK)
            return rc;
        /* An even cluster takes the low twelve bits of the pair, an odd one
         * the high twelve: two entries share the middle byte. */
        if ((cluster & 1u) != 0)
            *out = ((uint32_t)b[1] << 4) | ((uint32_t)b[0] >> 4);
        else
            *out = ((uint32_t)(b[1] & 0x0Fu) << 8) | (uint32_t)b[0];
        return FAT_OK;
    case FAT_TYPE_16:
        rc = fat_bytes(v, (uint64_t)cluster * 2u, 2u, b);
        if (rc != FAT_OK)
            return rc;
        *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8);
        return FAT_OK;
    default:
        rc = fat_bytes(v, (uint64_t)cluster * 4u, 4u, b);
        if (rc != FAT_OK)
            return rc;
        /* The top four bits are reserved and are not part of the number. */
        *out = get_le32(b) & 0x0FFFFFFFu;
        return FAT_OK;
    }
}

static int is_eoc(const fat_vol *v, uint32_t entry)
{
    switch (v->type) {
    case FAT_TYPE_12: return entry >= EOC_12;
    case FAT_TYPE_16: return entry >= EOC_16;
    default:          return entry >= EOC_32;
    }
}

/* The cluster `index` steps along the chain from `first`.
 *
 * A chain is a linked list, so this is O(index). Callers that walk a file or
 * a directory in order keep the last answer and ask for the next one, which
 * makes the common case O(1); the seek fields of fat_file exist for that. */
static int chain_seek(fat_vol *v, uint32_t first, uint32_t index,
                      uint32_t *out)
{
    uint32_t cluster = first;
    uint32_t entry;
    uint32_t i;
    int rc;

    if (cluster < CLUSTER_MIN)
        return FAT_ERR_CORRUPT;
    for (i = 0; i < index; i++) {
        rc = fat_entry(v, cluster, &entry);
        if (rc != FAT_OK)
            return rc;
        if (is_eoc(v, entry))
            return FAT_ERR_RANGE;
        if (entry < CLUSTER_MIN || entry >= v->clusters + CLUSTER_MIN)
            return FAT_ERR_CORRUPT;
        cluster = entry;
    }
    *out = cluster;
    return FAT_OK;
}

static uint64_t cluster_sector(const fat_vol *v, uint32_t cluster)
{
    uint32_t per = v->cluster_size / v->sector_size;

    return (uint64_t)v->data_off_sectors +
           (uint64_t)(cluster - CLUSTER_MIN) * per;
}

/* ---- mount ---- */

/* The things that make a boot sector a BPB, without trusting any of it yet.
 * Shared by probe and mount so the two cannot disagree about what a FAT
 * volume looks like. */
static int bpb_plausible(const uint8_t *s)
{
    uint32_t bps = get_le16(s + BPB_BYTES_PER_SEC);
    uint8_t spc = s[BPB_SEC_PER_CLUS];

    /* A jump instruction, because byte 0 of a FAT volume is code. */
    if (!(s[BPB_JMP] == 0xEBu || s[BPB_JMP] == 0xE9u))
        return 0;
    if (bps < 512u || bps > FAT_MAX_SECTOR || !is_pow2(bps))
        return 0;
    if (!is_pow2(spc))
        return 0;
    if (s[BPB_NUM_FATS] == 0 || get_le16(s + BPB_RSVD_SEC_CNT) == 0)
        return 0;
    return 1;
}

int fat_probe(fat_read_fn read, void *read_ctx)
{
    uint8_t s[512];

    if (read == (fat_read_fn)0)
        return FAT_ERR_BAD_ARG;
    if (read(read_ctx, 0, sizeof s, s) != 0)
        return FAT_ERR_IO;
    return bpb_plausible(s) ? FAT_OK : FAT_ERR_NOT_FAT;
}

/* An 11-byte space-padded label into a C string, trailing spaces removed. */
static void label_copy(char *out, const uint8_t *in)
{
    size_t n = 11u;
    size_t i;

    while (n > 0 && (in[n - 1u] == ' ' || in[n - 1u] == '\0'))
        n--;
    for (i = 0; i < n; i++)
        out[i] = (char)in[i];
    out[n] = '\0';
}

/* Defined below, next to the directory iterator whose machinery it borrows.
 * mount needs it and sits above that, and a forward declaration is cheaper
 * than moving either. */
static void label_from_root(fat_vol *v);

int fat_mount(fat_vol *v, fat_read_fn read, void *read_ctx)
{
    uint8_t s[512];
    uint32_t bps, spc, rsvd, nfats, root_ents, fat_sz, tot_sec;
    uint32_t root_sectors, data_sectors, clusters, meta_sectors;

    if (v == (fat_vol *)0 || read == (fat_read_fn)0)
        return FAT_ERR_BAD_ARG;
    memset(v, 0, sizeof(*v));
    if (read(read_ctx, 0, sizeof s, s) != 0)
        return FAT_ERR_IO;
    if (!bpb_plausible(s))
        return FAT_ERR_NOT_FAT;

    bps = get_le16(s + BPB_BYTES_PER_SEC);
    spc = s[BPB_SEC_PER_CLUS];
    rsvd = get_le16(s + BPB_RSVD_SEC_CNT);
    nfats = s[BPB_NUM_FATS];
    root_ents = get_le16(s + BPB_ROOT_ENT_CNT);
    fat_sz = get_le16(s + BPB_FAT_SZ_16);
    if (fat_sz == 0)
        fat_sz = get_le32(s + BPB32_FAT_SZ_32);
    tot_sec = get_le16(s + BPB_TOT_SEC_16);
    if (tot_sec == 0)
        tot_sec = get_le32(s + BPB_TOT_SEC_32);
    if (fat_sz == 0 || tot_sec == 0)
        return FAT_ERR_CORRUPT;

    /* The root directory of a FAT12/16 volume occupies whole sectors even
     * when its entry count does not fill the last one. */
    root_sectors = ((root_ents * DIR_ENTRY_SIZE) + (bps - 1u)) / bps;
    meta_sectors = rsvd + nfats * fat_sz + root_sectors;
    if (meta_sectors >= tot_sec)
        return FAT_ERR_CORRUPT;
    data_sectors = tot_sec - meta_sectors;
    clusters = data_sectors / spc;
    if (clusters == 0)
        return FAT_ERR_CORRUPT;

    /* THE type rule. Exact, and the only place it is decided. */
    if (clusters < FAT_MAX_CLUSTERS_12)
        v->type = FAT_TYPE_12;
    else if (clusters < FAT_MAX_CLUSTERS_16)
        v->type = FAT_TYPE_16;
    else
        v->type = FAT_TYPE_32;

    /* A BPB that contradicts the cluster count is refused rather than read
     * with the width its fields imply: the count is the spec's answer, and a
     * volume where the two disagree is one we would mis-parse either way. */
    if (v->type == FAT_TYPE_32) {
        if (root_ents != 0 || get_le16(s + BPB_FAT_SZ_16) != 0)
            return FAT_ERR_UNSUPPORTED;
    } else if (root_ents == 0) {
        return FAT_ERR_UNSUPPORTED;
    }

    v->read = read;
    v->read_ctx = read_ctx;
    v->sector_size = bps;
    v->cluster_size = bps * spc;
    v->fat_off_sectors = rsvd;
    v->fat_sectors = fat_sz;
    v->data_off_sectors = meta_sectors;
    v->clusters = clusters;
    v->root_off_sectors = rsvd + nfats * fat_sz;
    v->root_entries = root_ents;
    v->fat_cache_off = ~(uint64_t)0;

    if (v->type == FAT_TYPE_32) {
        v->root_cluster = get_le32(s + BPB32_ROOT_CLUS);
        if (v->root_cluster < CLUSTER_MIN ||
            v->root_cluster >= clusters + CLUSTER_MIN)
            return FAT_ERR_CORRUPT;
        if (s[BPB32_BOOT_SIG] == EXT_BOOT_SIG)
            label_copy(v->label, s + BPB32_VOL_LAB);
    } else if (s[BPB16_BOOT_SIG] == EXT_BOOT_SIG) {
        label_copy(v->label, s + BPB16_VOL_LAB);
    }
    label_from_root(v);
    return FAT_OK;
}

/* ---- directories ---- */

int fat_dir_open_root(fat_vol *v, fat_dir *d)
{
    if (v == (fat_vol *)0 || d == (fat_dir *)0)
        return FAT_ERR_BAD_ARG;
    memset(d, 0, sizeof(*d));
    d->vol = v;
    d->buf_off = ~(uint64_t)0;
    if (v->type == FAT_TYPE_32) {
        d->cluster = v->root_cluster;
    } else {
        d->fixed = 1;
        d->limit = v->root_entries * DIR_ENTRY_SIZE;
    }
    return FAT_OK;
}

int fat_dir_open(fat_vol *v, fat_dir *d, const fat_dirent *e)
{
    if (v == (fat_vol *)0 || d == (fat_dir *)0 || e == (const fat_dirent *)0)
        return FAT_ERR_BAD_ARG;
    if (!e->is_dir)
        return FAT_ERR_BAD_ARG;
    /* ".." pointing at the root of a FAT12/16 volume is recorded as cluster
     * zero, which is the one place a directory legitimately has none. The
     * iterator never produces "..", so reaching here with it means a caller
     * built a dirent by hand; answer with the root it names. */
    if (e->cluster == 0)
        return fat_dir_open_root(v, d);
    memset(d, 0, sizeof(*d));
    d->vol = v;
    d->cluster = e->cluster;
    d->buf_off = ~(uint64_t)0;
    return FAT_OK;
}

/* The sector holding d->pos, read into d->buf. Advances the cached chain
 * position by one cluster at a time, which is what keeps a sequential walk
 * from re-walking the chain for every sector. */
static int dir_sector(fat_dir *d, uint32_t *in_sector)
{
    fat_vol *v = d->vol;
    uint64_t sector;
    int rc;

    if (d->fixed) {
        if (d->pos >= d->limit)
            return 0;
        sector = (uint64_t)v->root_off_sectors + d->pos / v->sector_size;
    } else {
        uint32_t index;

        if (d->pos >= FAT_DIR_MAX)
            return FAT_ERR_CORRUPT;
        index = d->pos / v->cluster_size;

        if (index != d->chain_index) {
            uint32_t cluster;

            /* Sequential by construction: dir_next only ever moves forward. */
            if (index == d->chain_index + 1u) {
                uint32_t entry;

                rc = fat_entry(v, d->cluster, &entry);
                if (rc != FAT_OK)
                    return rc;
                if (is_eoc(v, entry))
                    return 0;
                if (entry < CLUSTER_MIN || entry >= v->clusters + CLUSTER_MIN)
                    return FAT_ERR_CORRUPT;
                cluster = entry;
            } else {
                rc = chain_seek(v, d->cluster, index - d->chain_index,
                                &cluster);
                if (rc == FAT_ERR_RANGE)
                    return 0;
                if (rc != FAT_OK)
                    return rc;
            }
            d->cluster = cluster;
            d->chain_index = index;
        }
        sector = cluster_sector(v, d->cluster) +
                 (d->pos % v->cluster_size) / v->sector_size;
    }
    if (d->buf_off != sector) {
        rc = read_sector(v, sector, d->buf);
        if (rc != FAT_OK) {
            d->buf_off = ~(uint64_t)0;
            return rc;
        }
        d->buf_off = sector;
    }
    *in_sector = d->pos % v->sector_size;
    return 1;
}

/* The volume label as the root directory records it, which is what every
 * other tool shows and what the BPB copy is only a stale echo of.
 *
 * It walks raw entries rather than calling fat_dir_next, because the entry it
 * is looking for is exactly the one the iterator hides. Absence is not an
 * error: whatever the BPB gave stays. */
static void label_from_root(fat_vol *v)
{
    fat_dir d;

    if (fat_dir_open_root(v, &d) != FAT_OK)
        return;
    for (;;) {
        uint32_t in_sector = 0;
        const uint8_t *e;

        if (dir_sector(&d, &in_sector) != 1)
            return;
        e = d.buf + in_sector;
        d.pos += DIR_ENTRY_SIZE;
        if (e[DE_NAME] == DE_END)
            return;
        if (e[DE_NAME] == DE_FREE ||
            (e[DE_ATTR] & ATTR_LONG_NAME) == ATTR_LONG_NAME)
            continue;
        if ((e[DE_ATTR] & ATTR_VOLUME_ID) != 0) {
            label_copy(v->label, e + DE_NAME);
            return;
        }
    }
}

/* The checksum an LFN run claims over the 8.3 entry it belongs to. The
 * rotate is the spec's, and it is the only thing tying a long name to a short
 * one: without checking it, a stale LFN run left by a deleted file attaches
 * its name to whatever entry follows. */
static uint8_t short_name_checksum(const uint8_t *name)
{
    uint8_t sum = 0;
    unsigned i;

    for (i = 0; i < 11u; i++)
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + name[i]);
    return sum;
}

/* "README  TXT" -> "README.TXT", honouring the lower-case flags Windows NT
 * records for names that are pure 8.3 but not upper case. */
static void short_name(const uint8_t *e, uint16_t *out, uint16_t *out_len)
{
    uint8_t nt = e[DE_NT_RES];
    unsigned n = 0;
    unsigned i;
    unsigned end;

    for (end = 8u; end > 0 && e[DE_NAME + end - 1u] == ' '; end--)
        ;
    for (i = 0; i < end; i++) {
        uint8_t c = e[DE_NAME + i];

        if (i == 0 && c == DE_KANJI_E5)
            c = DE_FREE;
        if ((nt & NT_RES_LOWER_BASE) != 0 && c >= 'A' && c <= 'Z')
            c = (uint8_t)(c + ('a' - 'A'));
        out[n++] = c;
    }
    for (end = 3u; end > 0 && e[DE_NAME + 8u + end - 1u] == ' '; end--)
        ;
    if (end > 0) {
        out[n++] = '.';
        for (i = 0; i < end; i++) {
            uint8_t c = e[DE_NAME + 8u + i];

            if ((nt & NT_RES_LOWER_EXT) != 0 && c >= 'A' && c <= 'Z')
                c = (uint8_t)(c + ('a' - 'A'));
            out[n++] = c;
        }
    }
    out[n] = 0;
    *out_len = (uint16_t)n;
}

/* Copies one LFN record's thirteen units into the accumulator at the slot its
 * sequence number claims. Returns 0 when the record is not usable. */
static int lfn_chars(const uint8_t *e, uint16_t *acc, uint16_t *acc_len)
{
    unsigned seq = e[LDIR_ORD] & LDIR_SEQ_MASK;
    unsigned base;
    unsigned n = 0;
    unsigned r;

    if (seq == 0 || seq > LDIR_MAX_SEQ)
        return 0;
    base = (seq - 1u) * LDIR_CHARS;
    if (base + LDIR_CHARS > FAT_NAME_MAX)
        return 0;
    for (r = 0; r < 3u; r++) {
        unsigned off = lfn_runs[r][0];
        unsigned count = lfn_runs[r][1];
        unsigned i;

        for (i = 0; i < count; i++, n++) {
            /* The index arithmetic is done in size_t rather than unsigned.
             * Nothing here can overflow (count is at most six and the runs
             * are fixed), but this file parses attacker-supplied media and
             * the check that flags the narrow form is kept on across the
             * repository for exactly that class of code. */
            uint16_t c = get_le16(e + off + (size_t)i * 2u);

            /* 0x0000 terminates the name, 0xFFFF pads after it. */
            if (c == 0 || c == 0xFFFFu) {
                if (base + n > *acc_len)
                    *acc_len = (uint16_t)(base + n);
                return 1;
            }
            acc[base + n] = c;
        }
    }
    if (base + n > *acc_len)
        *acc_len = (uint16_t)(base + n);
    return 1;
}

int fat_dir_next(fat_dir *d, fat_dirent *out)
{
    uint16_t lfn[FAT_NAME_MAX + 1];
    uint16_t lfn_len = 0;
    uint8_t lfn_sum = 0;
    int lfn_valid = 0;

    if (d == (fat_dir *)0 || out == (fat_dirent *)0 || d->vol == (fat_vol *)0)
        return FAT_ERR_BAD_ARG;

    for (;;) {
        uint8_t e[DIR_ENTRY_SIZE];
        uint32_t in_sector = 0;
        uint8_t attr;
        int rc = dir_sector(d, &in_sector);

        if (rc <= 0)
            return rc;   /* 0 = end of directory, negative = error */
        memcpy(e, d->buf + in_sector, DIR_ENTRY_SIZE);
        d->pos += DIR_ENTRY_SIZE;

        if (e[DE_NAME] == DE_END)
            return 0;
        if (e[DE_NAME] == DE_FREE) {
            lfn_valid = 0;
            lfn_len = 0;
            continue;
        }
        attr = e[DE_ATTR];
        if ((attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) {
            /* A run starts at the record carrying LDIR_LAST and every record
             * in it must claim the same checksum. Anything else and the run
             * is abandoned, so the 8.3 name is used instead of a name
             * assembled from somebody else's records. */
            if ((e[LDIR_ORD] & LDIR_LAST) != 0) {
                memset(lfn, 0, sizeof lfn);
                lfn_len = 0;
                lfn_sum = e[LDIR_CHKSUM];
                lfn_valid = 1;
            } else if (!lfn_valid || e[LDIR_CHKSUM] != lfn_sum) {
                lfn_valid = 0;
            }
            if (lfn_valid && e[LDIR_TYPE] == 0) {
                if (!lfn_chars(e, lfn, &lfn_len))
                    lfn_valid = 0;
            } else {
                lfn_valid = 0;
            }
            continue;
        }

        /* The volume label entry is the volume's name, not a file; mount has
         * already taken it and the caller reads it as vol->label. */
        if ((attr & ATTR_VOLUME_ID) != 0) {
            lfn_valid = 0;
            lfn_len = 0;
            continue;
        }
        /* "." and ".." are this directory and its parent, and every consumer
         * in the tree would have to skip them. */
        if (e[DE_NAME] == '.') {
            lfn_valid = 0;
            lfn_len = 0;
            continue;
        }

        memset(out, 0, sizeof(*out));
        if (lfn_valid && lfn_len > 0 &&
            lfn_sum == short_name_checksum(e + DE_NAME)) {
            memcpy(out->name, lfn, (size_t)lfn_len * sizeof(uint16_t));
            out->name[lfn_len] = 0;
            out->name_len = lfn_len;
        } else {
            short_name(e, out->name, &out->name_len);
        }
        out->is_dir = (uint8_t)((attr & ATTR_DIRECTORY) != 0);
        out->is_regular = (uint8_t)!out->is_dir;
        out->cluster = ((uint32_t)get_le16(e + DE_FST_CLUS_HI) << 16) |
                       get_le16(e + DE_FST_CLUS_LO);
        out->size = get_le32(e + DE_FILE_SIZE);
        /* A directory's size field is meaningless on FAT; every writer sets
         * it to zero and a reader that believed it would read nothing. */
        if (out->is_dir)
            out->size = 0;
        return 1;
    }
}

/* ---- files ---- */

int fat_file_open(fat_vol *v, fat_file *f, const fat_dirent *e)
{
    if (v == (fat_vol *)0 || f == (fat_file *)0 || e == (const fat_dirent *)0)
        return FAT_ERR_BAD_ARG;
    if (e->is_dir)
        return FAT_ERR_BAD_ARG;
    memset(f, 0, sizeof(*f));
    f->vol = v;
    f->first_cluster = e->cluster;
    f->size = e->size;
    /* An empty file has no cluster at all, which is legal and is why nothing
     * here refuses cluster 0; reads of a zero-length file are all refused by
     * the range check instead. */
    return FAT_OK;
}

/* The cluster holding file offset `index`, using and updating the cached
 * position so that reading a file front to back does not re-walk the chain. */
static int file_cluster(fat_file *f, uint32_t index, uint32_t *out)
{
    int rc;

    if (f->seek_cluster >= CLUSTER_MIN && index >= f->seek_index) {
        uint32_t cluster;

        rc = chain_seek(f->vol, f->seek_cluster, index - f->seek_index,
                        &cluster);
        if (rc != FAT_OK)
            return rc;
        f->seek_index = index;
        f->seek_cluster = cluster;
        *out = cluster;
        return FAT_OK;
    }
    rc = chain_seek(f->vol, f->first_cluster, index, out);
    if (rc != FAT_OK)
        return rc;
    f->seek_index = index;
    f->seek_cluster = *out;
    return FAT_OK;
}

int fat_file_read(fat_file *f, uint64_t off, uint64_t len, void *buf)
{
    fat_vol *v;
    uint8_t *dst = buf;

    if (f == (fat_file *)0 || buf == (void *)0 || f->vol == (fat_vol *)0)
        return FAT_ERR_BAD_ARG;
    v = f->vol;
    /* Wholly inside the file or nothing, like every driver in the tree. The
     * addition cannot wrap: size is 32 bits and both operands are 64. */
    if (off > f->size || len > (uint64_t)f->size - off)
        return FAT_ERR_RANGE;
    if (len == 0)
        return FAT_OK;
    if (f->first_cluster < CLUSTER_MIN)
        return FAT_ERR_CORRUPT;

    /* Whole sectors go to the device as one read per run of clusters that
     * sit next to each other on disk, not one read per sector. The
     * difference is the whole cost on a real machine: firmware Block I/O over
     * USB has no cache and a millisecond or more per request, and hashing a
     * medium's 1 MiB BOOTX64.EFI out of its El Torito image a sector at a
     * time was 2048 of them per entry, which is most of what the menu's
     * startup probe spent (measured 2026-09-20 on the archusb stick: about
     * 9,700 of 10,250 reads across seven images). Only the partial sectors
     * at either end still go through the bounce buffer. */
    while (len > 0) {
        uint32_t index = (uint32_t)(off / v->cluster_size);
        uint32_t in_cluster = (uint32_t)(off % v->cluster_size);
        uint32_t cluster;
        uint64_t sector;
        uint32_t in_sector = in_cluster % v->sector_size;
        uint64_t take;
        uint64_t whole;
        int rc = file_cluster(f, index, &cluster);

        if (rc != FAT_OK)
            return rc;
        sector = cluster_sector(v, cluster) + in_cluster / v->sector_size;

        if (in_sector != 0 || len < v->sector_size) {
            take = v->sector_size - in_sector;
            if (take > len)
                take = len;
            rc = read_sector(v, sector, f->buf);
            if (rc != FAT_OK)
                return rc;
            memcpy(dst, f->buf + in_sector, (size_t)take);
            dst += take;
            off += take;
            len -= take;
            continue;
        }

        /* Sector-aligned from here: the rest of this cluster, then each
         * following cluster for as long as the chain's next is the next one
         * on disk. Stepping the cursor one cluster at a time is what the loop
         * would do anyway, so a run that ends at a gap costs nothing extra;
         * the next pass starts from the cursor it left. */
        whole = len - len % v->sector_size;
        take = v->cluster_size - in_cluster;
        if (take > whole)
            take = whole;
        while (take < whole) {
            uint32_t next;

            rc = file_cluster(f, index + 1u, &next);
            if (rc != FAT_OK)
                return rc;
            if (next != cluster + 1u)
                break;
            cluster = next;
            index++;
            take += (whole - take < v->cluster_size) ? whole - take
                                                     : v->cluster_size;
        }
        if (v->read(v->read_ctx, sector * v->sector_size, take, dst) != 0)
            return FAT_ERR_IO;
        dst += take;
        off += take;
        len -= take;
    }
    return FAT_OK;
}

int fat_file_extents(fat_vol *v, const fat_dirent *e, fat_extent *out,
                     size_t cap, size_t *count)
{
    uint64_t remaining;
    uint32_t cluster;
    size_t n = 0;
    int rc;

    if (v == (fat_vol *)0 || e == (const fat_dirent *)0 ||
        count == (size_t *)0)
        return FAT_ERR_BAD_ARG;
    *count = 0;
    if (e->size == 0)
        return FAT_OK;
    if (e->cluster < CLUSTER_MIN)
        return FAT_ERR_CORRUPT;

    remaining = e->size;
    cluster = e->cluster;
    while (remaining > 0) {
        uint64_t start = cluster_sector(v, cluster) * v->sector_size;
        uint64_t run = v->cluster_size;
        uint32_t entry;

        /* Coalesce while the chain stays physically contiguous, which on a
         * freshly written volume is the whole file in one extent. */
        for (;;) {
            rc = fat_entry(v, cluster, &entry);
            if (rc != FAT_OK)
                return rc;
            if (is_eoc(v, entry))
                break;
            if (entry < CLUSTER_MIN || entry >= v->clusters + CLUSTER_MIN)
                return FAT_ERR_CORRUPT;
            if (entry != cluster + 1u)
                break;
            cluster = entry;
            run += v->cluster_size;
            if (run >= remaining)
                break;
        }
        if (run > remaining)
            run = remaining;
        if (n < cap) {
            out[n].vol_off = start;
            out[n].len = run;
        }
        n++;
        remaining -= run;
        if (remaining == 0)
            break;
        if (is_eoc(v, entry))
            return FAT_ERR_CORRUPT;   /* chain ends before the size does */
        cluster = entry;
    }
    *count = n;
    return n > cap ? FAT_ERR_NOSPACE : FAT_OK;
}
