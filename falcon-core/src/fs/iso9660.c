/* Read-only ISO9660 + Rock Ridge + Joliet. See falcon-core/src/fs/iso9660.h
 * for scope and for which name space is served.
 *
 * Structure offsets are ECMA-119 (4th edition, 2019) Section 9 for directory
 * records and 8.4/8.5 for the volume descriptors; Rock Ridge is IEEE P1282
 * (RRIP 1.12) over P1281 (SUSP 1.12); Joliet is Microsoft's 1995 spec.
 *
 * Every read the driver issues is whole 2048-byte blocks at block-aligned
 * offsets. The loader reaches an ISO through the exFAT driver, which would
 * take anything, but the device under that is Block I/O, and keeping this
 * layer aligned means a future caller that hands it a raw device needs no
 * second look.
 */

#include <fs/iso9660.h>

#include <lib/string.h>

#define BLK       ISO9660_BLOCK
#define NO_CACHE  (~(uint64_t)0)

/* ECMA-119 9.1: the fixed part of a directory record is 33 bytes, and the
 * name is at least one byte, so 34 is the smallest record that can exist. */
#define REC_MIN   34u

/* Directory record flags (ECMA-119 9.1.6). */
#define F_HIDDEN      0x01u
#define F_DIR         0x02u
#define F_ASSOCIATED  0x04u
#define F_MULTI       0x80u

/* Continuation areas a single record may chain through. Real media use one at
 * most; the bound is what keeps a crafted CE cycle from spinning forever. */
#define CE_HOPS_MAX   16u

/* POSIX file type bits as Rock Ridge PX carries them. */
#define PX_IFMT   0170000u
#define PX_IFDIR  0040000u
#define PX_IFREG  0100000u
#define PX_IFLNK  0120000u

static int read_block(iso9660_vol *v, uint64_t lba, uint8_t *buf,
                      uint64_t *cached)
{
    uint64_t off = lba * BLK;

    if (cached != (uint64_t *)0 && *cached == off)
        return ISO9660_OK;
    if (v->read(v->read_ctx, off, BLK, buf) != 0) {
        if (cached != (uint64_t *)0)
            *cached = NO_CACHE;
        return ISO9660_ERR_IO;
    }
    if (cached != (uint64_t *)0)
        *cached = off;
    return ISO9660_OK;
}

int iso9660_probe(iso9660_read_fn read, void *read_ctx)
{
    uint8_t b[BLK];

    if (read == (iso9660_read_fn)0)
        return ISO9660_ERR_BAD_ARG;
    if (read(read_ctx, (uint64_t)16u * BLK, BLK, b) != 0)
        return ISO9660_ERR_IO;
    if (memcmp(b + 1, "CD001", 5) != 0)
        return ISO9660_ERR_BAD_SUPER;
    return ISO9660_OK;
}

/* ---- names ---- */

/* ISO9660 identifiers carry a ";version" suffix, and a file with no extension
 * keeps the '.' that separated it from one (ECMA-119 7.5.1). Neither is part
 * of the name anyone typed, and both are stripped from the plain and Joliet
 * forms. A name that is nothing but those characters is left alone rather
 * than reduced to nothing. */
static uint16_t strip_version(uint16_t *name, uint16_t n)
{
    uint16_t i;

    for (i = 0; i < n; i++) {
        if (name[i] == ';') {
            if (i > 0)
                n = i;
            break;
        }
    }
    if (n > 1 && name[n - 1] == '.')
        n--;
    name[n] = 0;
    return n;
}

static uint16_t name_plain(const uint8_t *id, uint8_t len, uint16_t *out)
{
    uint16_t i;

    for (i = 0; i < len && i < ISO9660_NAME_MAX; i++)
        out[i] = id[i];
    return strip_version(out, i);
}

/* Joliet identifiers are UCS-2 big-endian, so an odd length is a record
 * someone built wrong. */
static int name_joliet(const uint8_t *id, uint8_t len, uint16_t *out,
                       uint16_t *n)
{
    uint16_t i;

    if ((len & 1u) != 0)
        return ISO9660_ERR_CORRUPT;
    for (i = 0; i < len / 2u && i < ISO9660_NAME_MAX; i++)
        out[i] = (uint16_t)(((uint16_t)id[(size_t)2u * i] << 8) |
                            id[(size_t)2u * i + 1u]);
    *n = strip_version(out, i);
    return ISO9660_OK;
}

/* ---- Rock Ridge ---- */

typedef struct {
    uint8_t  nm[ISO9660_NAME_MAX];
    uint16_t nm_len;
    uint8_t  have_nm;
    uint8_t  nm_open;       /* an NM with CONTINUE was the last one seen */
    uint8_t  have_px;
    uint32_t mode;
    uint8_t  relocated;     /* RE: this record is the moved directory itself */
    uint8_t  have_cl;       /* CL: this record stands in for a moved one */
    uint32_t cl_lba;
    uint8_t  zf;
    uint8_t  have_ce;
    uint32_t ce_lba, ce_off, ce_len;
} rr_info;

/* Walks one System Use area. Returns 1 when an ST entry ended the whole
 * chain, 0 when the area simply ran out, negative on a malformed entry. */
static int rr_area(const uint8_t *p, uint32_t len, rr_info *ri)
{
    uint32_t i = 0;

    while (i + 4u <= len) {
        const uint8_t *e = p + i;
        uint32_t el = e[2];

        /* SUSP 5.1: an area may be padded with a zero byte; anything shorter
         * than a header is the end of the entries rather than one. */
        if (e[0] == 0)
            break;
        if (el < 4u || i + el > len)
            return ISO9660_ERR_CORRUPT;

        if (e[0] == 'S' && e[1] == 'T') {
            return 1;
        } else if (e[0] == 'C' && e[1] == 'E') {
            if (el < 28u)
                return ISO9660_ERR_CORRUPT;
            ri->have_ce = 1;
            ri->ce_lba = get_le32(e + 4);
            ri->ce_off = get_le32(e + 12);
            ri->ce_len = get_le32(e + 20);
        } else if (e[0] == 'N' && e[1] == 'M') {
            uint8_t flags;
            uint32_t n, j;

            if (el < 5u)
                return ISO9660_ERR_CORRUPT;
            flags = e[4];
            /* CURRENT and PARENT name "." and ".."; the walk never produces
             * either, so an entry claiming to be one is not a name. */
            if ((flags & 0x06u) != 0) {
                ri->nm_open = 0;
            } else {
                if (ri->have_nm && !ri->nm_open)
                    ri->nm_len = 0;     /* a second, unrelated NM wins */
                n = el - 5u;
                if ((uint32_t)ri->nm_len + n > ISO9660_NAME_MAX)
                    return ISO9660_ERR_CORRUPT;
                for (j = 0; j < n; j++)
                    ri->nm[ri->nm_len + j] = e[5u + j];
                ri->nm_len = (uint16_t)(ri->nm_len + n);
                ri->have_nm = 1;
                ri->nm_open = (uint8_t)((flags & 0x01u) != 0);
            }
        } else if (e[0] == 'P' && e[1] == 'X') {
            if (el < 36u)
                return ISO9660_ERR_CORRUPT;
            ri->have_px = 1;
            ri->mode = get_le32(e + 4);
        } else if (e[0] == 'C' && e[1] == 'L') {
            if (el < 12u)
                return ISO9660_ERR_CORRUPT;
            ri->have_cl = 1;
            ri->cl_lba = get_le32(e + 4);
        } else if (e[0] == 'R' && e[1] == 'E') {
            ri->relocated = 1;
        } else if (e[0] == 'Z' && e[1] == 'F') {
            ri->zf = 1;
        }
        i += el;
    }
    return 0;
}

/* The whole System Use chain of one record: the area inside it, then every
 * continuation area it points at. The continuation blocks are read through the
 * volume's cache, never the caller's directory buffer, which still holds the
 * record being parsed. */
static int rr_parse(iso9660_vol *v, const uint8_t *rec, uint32_t rec_len,
                    rr_info *ri)
{
    uint32_t name_len = rec[32];
    uint32_t start = 33u + name_len + ((name_len & 1u) ? 0u : 1u) + v->su_skip;
    unsigned hops = 0;
    int rc;

    memset(ri, 0, sizeof(*ri));
    if (start >= rec_len)
        return ISO9660_OK;      /* no System Use area at all */
    rc = rr_area(rec + start, rec_len - start, ri);
    while (rc == 0 && ri->have_ce) {
        uint32_t lba = ri->ce_lba, off = ri->ce_off, len = ri->ce_len;

        if (++hops > CE_HOPS_MAX)
            return ISO9660_ERR_CORRUPT;
        if (off >= BLK || len > BLK - off)
            return ISO9660_ERR_CORRUPT;
        ri->have_ce = 0;
        rc = read_block(v, lba, v->cache, &v->cache_off);
        if (rc != ISO9660_OK)
            return rc;
        rc = rr_area(v->cache + off, len, ri);
    }
    return rc < 0 ? rc : ISO9660_OK;
}

/* A CL record is a zero-length placeholder; the directory it stands for keeps
 * its own "." record, whose extent and length are the directory's. */
static int cl_resolve(iso9660_vol *v, uint32_t lba, uint32_t *len)
{
    int rc = read_block(v, lba, v->cache, &v->cache_off);
    const uint8_t *r = v->cache;

    if (rc != ISO9660_OK)
        return rc;
    if (r[0] < REC_MIN || r[32] != 1u || r[33] != 0u || get_le32(r + 2) != lba)
        return ISO9660_ERR_CORRUPT;
    *len = get_le32(r + 10);
    return ISO9660_OK;
}

/* ---- mount ---- */

/* primary_only forces the ISO 9660 primary tree with plain names, ignoring any
 * Joliet or Rock Ridge tree that would otherwise win. Windows' bootmgr reads a
 * boot CD through the primary descriptor, so a loader that relocates
 * \sources\boot.wim for it must edit the record in this tree, not the Joliet
 * one iso9660_mount prefers. */
static int mount_common(iso9660_vol *v, iso9660_read_fn read, void *read_ctx,
                        int primary_only)
{
    uint8_t b[BLK];
    uint32_t pvd_root_lba = 0, pvd_root_len = 0;
    uint32_t jol_root_lba = 0, jol_root_len = 0;
    int have_pvd = 0, have_joliet = 0;
    unsigned i;
    int rc;

    if (v == (iso9660_vol *)0 || read == (iso9660_read_fn)0)
        return ISO9660_ERR_BAD_ARG;
    memset(v, 0, sizeof(*v));
    v->read = read;
    v->read_ctx = read_ctx;
    v->cache_off = NO_CACHE;

    /* The descriptor set starts at block 16 and ends at a terminator. 64 is
     * far more descriptors than any image carries and bounds a set with no
     * terminator at all. */
    for (i = 16; i < 16u + 64u; i++) {
        if (read(read_ctx, (uint64_t)i * BLK, BLK, b) != 0)
            return ISO9660_ERR_IO;
        if (memcmp(b + 1, "CD001", 5) != 0) {
            if (i == 16)
                return ISO9660_ERR_BAD_SUPER;
            break;
        }
        if (b[0] == 255)
            break;
        if (b[0] == 1 && !have_pvd) {
            int n;

            if (get_le16(b + 128) != BLK)
                return ISO9660_ERR_UNSUPPORTED;
            if (b[156] < REC_MIN)
                return ISO9660_ERR_CORRUPT;
            pvd_root_lba = get_le32(b + 156 + 2);
            pvd_root_len = get_le32(b + 156 + 10);
            for (n = 0; n < 32; n++)
                v->label[n] = (char)b[40 + n];
            v->label[32] = '\0';
            for (n = 31; n >= 0 && (v->label[n] == ' ' || v->label[n] == '\0');
                 n--)
                v->label[n] = '\0';
            have_pvd = 1;
        } else if (b[0] == 2 && !have_joliet && b[88] == '%' && b[89] == '/' &&
                   (b[90] == '@' || b[90] == 'C' || b[90] == 'E')) {
            if (get_le16(b + 128) != BLK)
                continue;       /* an odd SVD is ignored, not fatal */
            jol_root_lba = get_le32(b + 156 + 2);
            jol_root_len = get_le32(b + 156 + 10);
            have_joliet = (b[156] >= REC_MIN && jol_root_len != 0);
        }
    }
    if (!have_pvd)
        return ISO9660_ERR_BAD_SUPER;
    if (pvd_root_len == 0)
        return ISO9660_ERR_CORRUPT;

    /* Rock Ridge announces itself in the root's "." record: its System Use
     * area opens with SUSP's SP entry (SUSP 5.3), check bytes BE EF, and
     * SP's LEN_SKP applies to every System Use area after it. Not consulted
     * for a primary-only mount, which serves plain names by definition. */
    if (!primary_only) {
        rc = read_block(v, pvd_root_lba, v->cache, &v->cache_off);
        if (rc != ISO9660_OK)
            return rc;
        {
            const uint8_t *r = v->cache;
            uint32_t rl = r[0];

            if (rl >= REC_MIN + 7u && r[32] == 1u &&
                r[34] == 'S' && r[35] == 'P' && r[36] >= 7u &&
                r[38] == 0xBEu && r[39] == 0xEFu) {
                v->names = ISO9660_NAMES_RRIP;
                v->su_skip = r[40];
            }
        }
    }

    if (!primary_only && v->names == ISO9660_NAMES_RRIP) {
        v->root_lba = pvd_root_lba;
        v->root_len = pvd_root_len;
    } else if (!primary_only && have_joliet) {
        v->names = ISO9660_NAMES_JOLIET;
        v->root_lba = jol_root_lba;
        v->root_len = jol_root_len;
    } else {
        v->names = ISO9660_NAMES_PLAIN;
        v->root_lba = pvd_root_lba;
        v->root_len = pvd_root_len;
    }
    return ISO9660_OK;
}

int iso9660_mount(iso9660_vol *v, iso9660_read_fn read, void *read_ctx)
{
    return mount_common(v, read, read_ctx, 0);
}

int iso9660_mount_primary(iso9660_vol *v, iso9660_read_fn read, void *read_ctx)
{
    return mount_common(v, read, read_ctx, 1);
}

/* ---- directories ---- */

static void dir_init(iso9660_vol *v, iso9660_dir *d, uint32_t lba,
                     uint32_t len)
{
    d->vol = v;
    d->lba = lba;
    d->len = len;
    d->pos = 0;
    d->buf_off = NO_CACHE;
}

int iso9660_dir_open_root(iso9660_vol *v, iso9660_dir *d)
{
    if (v == (iso9660_vol *)0 || d == (iso9660_dir *)0)
        return ISO9660_ERR_BAD_ARG;
    dir_init(v, d, v->root_lba, v->root_len);
    return ISO9660_OK;
}

int iso9660_dir_open(iso9660_vol *v, iso9660_dir *d, const iso9660_dirent *e)
{
    if (v == (iso9660_vol *)0 || d == (iso9660_dir *)0 ||
        e == (const iso9660_dirent *)0 || !e->is_dir)
        return ISO9660_ERR_BAD_ARG;
    dir_init(v, d, e->lba, (uint32_t)e->size);
    return ISO9660_OK;
}

/* The next raw record of the directory, skipping the zero padding that ends a
 * block early (records never straddle blocks, ECMA-119 6.8.1.1). 1 with *rec
 * pointing into d->buf, 0 at the end, negative on error. */
static int next_record(iso9660_dir *d, const uint8_t **rec, uint32_t *rec_lba,
                       uint16_t *rec_off)
{
    for (;;) {
        uint32_t blk, off, rl;
        int rc;

        if (d->pos >= d->len)
            return 0;
        blk = (uint32_t)(d->pos / BLK);
        off = (uint32_t)(d->pos % BLK);
        rc = read_block(d->vol, (uint64_t)d->lba + blk, d->buf, &d->buf_off);
        if (rc != ISO9660_OK)
            return rc;
        rl = d->buf[off];
        if (rl == 0) {
            d->pos = ((uint64_t)blk + 1u) * BLK;
            continue;
        }
        if (rl < REC_MIN || off + rl > BLK || 33u + d->buf[off + 32] > rl)
            return ISO9660_ERR_CORRUPT;
        *rec = d->buf + off;
        *rec_lba = d->lba + blk;
        *rec_off = (uint16_t)off;
        d->pos += rl;
        return 1;
    }
}

/* Fills out from one record. Returns 1 for an entry, 0 for a record the walk
 * must not produce, negative on error. */
static int parse_record(iso9660_vol *v, const uint8_t *rec, uint32_t rec_lba,
                        uint16_t rec_off, iso9660_dirent *out)
{
    uint32_t rl = rec[0];
    uint8_t flags = rec[25];
    uint8_t name_len = rec[32];
    const uint8_t *id = rec + 33;
    rr_info ri;
    int rc;

    if (name_len == 1 && (id[0] == 0 || id[0] == 1))
        return 0;               /* "." and ".." */
    if (flags & F_ASSOCIATED)
        return 0;               /* an associated file (a resource fork) */

    memset(out, 0, sizeof(*out));
    out->lba = get_le32(rec + 2);
    out->size = get_le32(rec + 10);
    out->is_dir = (uint8_t)((flags & F_DIR) != 0);
    out->is_regular = (uint8_t)!out->is_dir;
    out->parts = 1;
    out->rec_lba = rec_lba;
    out->rec_off = rec_off;
    /* An interleaved file's bytes are not contiguous in its extent. Nothing
     * written since the 1990s interleaves; it is refused at open. */
    if (rec[26] != 0 || rec[27] != 0)
        out->compressed = 1;

    if (v->names == ISO9660_NAMES_JOLIET) {
        rc = name_joliet(id, name_len, out->name, &out->name_len);
        if (rc != ISO9660_OK)
            return rc;
        return 1;
    }

    out->name_len = name_plain(id, name_len, out->name);
    if (v->names != ISO9660_NAMES_RRIP)
        return 1;

    rc = rr_parse(v, rec, rl, &ri);
    if (rc != ISO9660_OK)
        return rc;
    if (ri.relocated)
        return 0;               /* reached again through its CL record */
    if (ri.have_nm && ri.nm_len > 0) {
        uint16_t i;

        for (i = 0; i < ri.nm_len; i++)
            out->name[i] = ri.nm[i];
        out->name[ri.nm_len] = 0;
        out->name_len = ri.nm_len;
    }
    if (ri.have_px && !out->is_dir &&
        (ri.mode & PX_IFMT) != PX_IFREG)
        out->is_regular = 0;
    if (ri.zf)
        out->compressed = 1;
    if (ri.have_cl) {
        uint32_t len;

        rc = cl_resolve(v, ri.cl_lba, &len);
        if (rc != ISO9660_OK)
            return rc;
        out->is_dir = 1;
        out->is_regular = 0;
        out->lba = ri.cl_lba;
        out->size = len;
    }
    return 1;
}

int iso9660_dir_next(iso9660_dir *d, iso9660_dirent *out)
{
    if (d == (iso9660_dir *)0 || out == (iso9660_dirent *)0 ||
        d->vol == (iso9660_vol *)0)
        return ISO9660_ERR_BAD_ARG;

    for (;;) {
        const uint8_t *rec;
        uint32_t rec_lba;
        uint16_t rec_off;
        uint8_t flags;
        int rc = next_record(d, &rec, &rec_lba, &rec_off);

        if (rc <= 0)
            return rc;
        flags = rec[25];
        rc = parse_record(d->vol, rec, rec_lba, rec_off, out);
        if (rc < 0)
            return rc;
        if (rc == 0) {
            /* A skipped multi-extent file still owns the records after it. */
            while (flags & F_MULTI) {
                rc = next_record(d, &rec, &rec_lba, &rec_off);
                if (rc < 0)
                    return rc;
                if (rc == 0)
                    return ISO9660_ERR_CORRUPT;
                flags = rec[25];
            }
            continue;
        }

        /* A multi-extent file (ECMA-119 6.5.1): every record but the last
         * carries the flag, and the parts follow each other in the
         * directory. Their lengths add up to the file. */
        while (flags & F_MULTI) {
            if (out->is_dir)
                return ISO9660_ERR_CORRUPT;
            rc = next_record(d, &rec, &rec_lba, &rec_off);
            if (rc < 0)
                return rc;
            if (rc == 0)
                return ISO9660_ERR_CORRUPT;
            if (out->parts >= ISO9660_MAX_PARTS)
                return ISO9660_ERR_UNSUPPORTED;
            out->parts++;
            out->size += get_le32(rec + 10);
            flags = rec[25];
        }
        return 1;
    }
}

/* ---- files ---- */

int iso9660_file_open(iso9660_vol *v, iso9660_file *f,
                      const iso9660_dirent *e)
{
    uint64_t total;
    uint32_t lba, off;
    unsigned blocks = 0;

    if (v == (iso9660_vol *)0 || f == (iso9660_file *)0 ||
        e == (const iso9660_dirent *)0 || e->is_dir)
        return ISO9660_ERR_BAD_ARG;
    if (!e->is_regular)
        return ISO9660_ERR_SYMLINK;
    if (e->compressed)
        return ISO9660_ERR_UNSUPPORTED;
    if (e->parts == 0 || e->parts > ISO9660_MAX_PARTS)
        return ISO9660_ERR_BAD_ARG;

    memset(f, 0, sizeof(*f));
    f->vol = v;
    f->size = e->size;
    f->buf_off = NO_CACHE;

    if (e->parts == 1) {
        f->nparts = 1;
        f->part[0].lba = e->lba;
        f->part[0].len = (uint32_t)e->size;
        return ISO9660_OK;
    }

    /* A multi-extent file's parts are re-read from the directory, starting at
     * the record the walk found first. The walk that produced the dirent has
     * already checked each record's shape; this checks it again, because a
     * dirent is data a caller handed back and the disc may be read twice. */
    lba = e->rec_lba;
    off = e->rec_off;
    total = 0;
    for (;;) {
        const uint8_t *r;
        uint32_t rl;
        int rc;

        if (off >= BLK) {
            lba++;
            off = 0;
        }
        if (off == 0 && ++blocks > 2u * ISO9660_MAX_PARTS)
            return ISO9660_ERR_CORRUPT;
        rc = read_block(v, lba, f->buf, &f->buf_off);
        if (rc != ISO9660_OK)
            return rc;
        rl = f->buf[off];
        if (rl == 0) {
            off = BLK;
            continue;
        }
        if (rl < REC_MIN || off + rl > BLK)
            return ISO9660_ERR_CORRUPT;
        r = f->buf + off;
        if (f->nparts >= ISO9660_MAX_PARTS)
            return ISO9660_ERR_CORRUPT;
        f->part[f->nparts].lba = get_le32(r + 2);
        f->part[f->nparts].len = get_le32(r + 10);
        total += f->part[f->nparts].len;
        f->nparts++;
        off += rl;
        if (!(r[25] & F_MULTI))
            break;
    }
    if (f->nparts != e->parts || total != e->size)
        return ISO9660_ERR_CORRUPT;
    f->buf_off = NO_CACHE;
    return ISO9660_OK;
}

/* Reads [off, off+len) of one part, which is contiguous on disc. Whole blocks
 * go straight into the caller's buffer; a ragged head or tail goes through the
 * file's one-block cache so the callback only ever sees aligned blocks. */
static int read_part(iso9660_file *f, const iso9660_part *p, uint64_t off,
                     uint64_t len, uint8_t *dst)
{
    uint64_t base = (uint64_t)p->lba * BLK;

    while (len > 0) {
        uint64_t inblk = off % BLK;
        uint64_t n;

        if (inblk == 0 && len >= BLK) {
            n = len - (len % BLK);
            if (f->vol->read(f->vol->read_ctx, base + off, n, dst) != 0)
                return ISO9660_ERR_IO;
        } else {
            int rc = read_block(f->vol, (base + off - inblk) / BLK, f->buf,
                                &f->buf_off);

            if (rc != ISO9660_OK)
                return rc;
            n = BLK - inblk;
            if (n > len)
                n = len;
            memcpy(dst, f->buf + inblk, (size_t)n);
        }
        off += n;
        len -= n;
        dst += n;
    }
    return ISO9660_OK;
}

int iso9660_file_read(iso9660_file *f, uint64_t off, uint64_t len, void *buf)
{
    uint8_t *dst = (uint8_t *)buf;
    uint64_t start = 0;
    unsigned i;

    if (f == (iso9660_file *)0 || f->vol == (iso9660_vol *)0 ||
        (buf == (void *)0 && len != 0))
        return ISO9660_ERR_BAD_ARG;
    if (off > f->size || len > f->size - off)
        return ISO9660_ERR_RANGE;

    for (i = 0; i < f->nparts && len > 0; i++) {
        uint64_t plen = f->part[i].len;

        if (off < start + plen) {
            uint64_t in = off - start;
            uint64_t n = plen - in;
            int rc;

            if (n > len)
                n = len;
            rc = read_part(f, &f->part[i], in, n, dst);
            if (rc != ISO9660_OK)
                return rc;
            dst += n;
            off += n;
            len -= n;
        }
        start += plen;
    }
    return len == 0 ? ISO9660_OK : ISO9660_ERR_CORRUPT;
}

int iso9660_file_extents(iso9660_vol *v, const iso9660_dirent *e,
                         iso9660_extent *out, size_t cap, size_t *count)
{
    static iso9660_file f;      /* ~2 KB: keep off the loader's stack */
    size_t n = 0;
    unsigned i;
    int rc;

    if (count == (size_t *)0)
        return ISO9660_ERR_BAD_ARG;
    *count = 0;
    if (cap != 0 && out == (iso9660_extent *)0)
        return ISO9660_ERR_BAD_ARG;
    rc = iso9660_file_open(v, &f, e);
    if (rc != ISO9660_OK)
        return rc;

    /* The run being built is tracked apart from the caller's array, so the
     * count a too-small cap reports is the count a big enough one gets. */
    {
        uint64_t run_off = 0, run_len = 0;

        for (i = 0; i < f.nparts; i++) {
            uint64_t off = (uint64_t)f.part[i].lba * BLK;
            uint64_t len = f.part[i].len;

            if (len == 0)
                continue;
            if (run_len != 0 && run_off + run_len == off) {
                run_len += len;
            } else {
                if (run_len != 0) {
                    if (n < cap) {
                        out[n].vol_off = run_off;
                        out[n].len = run_len;
                    }
                    n++;
                }
                run_off = off;
                run_len = len;
            }
        }
        if (run_len != 0) {
            if (n < cap) {
                out[n].vol_off = run_off;
                out[n].len = run_len;
            }
            n++;
        }
    }
    *count = n;
    return n > cap ? ISO9660_ERR_NOSPACE : ISO9660_OK;
}
