/* Read-only UDF. See falcon-core/src/fs/udf.h for scope.
 *
 * Offsets are ECMA-167 3rd edition (Part 3 for volume structures, Part 4 for
 * files) as profiled by OSTA UDF 2.60; comments name the section where the
 * layout is not obvious from the field.
 *
 * The walk from nothing to a file:
 *
 *   volume recognition  "BEA01" then "NSR02"/"NSR03" in the 2 KiB slots
 *                       from byte 32768 (ECMA-167 2/8)
 *   anchor              the AVDP at block 256 names the Volume Descriptor
 *                       Sequence (3/10.2)
 *   descriptor sequence the Partition Descriptor says where the partition
 *                       starts; the Logical Volume Descriptor says where the
 *                       File Set Descriptor is and how partitions are mapped
 *   file set            the FSD names the root directory's ICB (4/14.1)
 *   ICB                 a File Entry (4/14.9) or Extended File Entry
 *                       (4/14.17) holds a file's size and its allocation
 *                       descriptors, or the data itself
 *   directory           a byte stream of File Identifier Descriptors (4/14.4)
 *
 * All reads are whole blocks at block-aligned offsets, as in the ISO9660
 * driver and for the same reason.
 */

#include <fs/udf.h>

#include <lib/string.h>

#define BS        UDF_BLOCK
#define NO_CACHE  (~(uint64_t)0)

/* Descriptor tag identifiers (ECMA-167 3/7.2.1 and 4/7.2.1). */
#define TAG_PVD   1u
#define TAG_AVDP  2u
#define TAG_VDP   3u
#define TAG_PD    5u
#define TAG_LVD   6u
#define TAG_TD    8u
#define TAG_FSD   256u
#define TAG_FID   257u
#define TAG_AED   258u
#define TAG_FE    261u
#define TAG_EFE   266u

/* ICB file types (4/14.6.6). */
#define FT_DIR    4u
#define FT_FILE   5u
#define FT_SYMLINK 12u

/* FID file characteristics (4/14.4.3). */
#define FC_DIR     0x02u
#define FC_DELETED 0x04u
#define FC_PARENT  0x08u

/* Bounds on chains a crafted image could make cyclic. */
#define VDS_BLOCKS_MAX  64u
#define VDP_HOPS_MAX    4u
#define AED_HOPS_MAX    64u

static uint16_t crc_itu_update(uint16_t crc, const uint8_t *p, size_t n)
{
    size_t i;
    int b;

    for (i = 0; i < n; i++) {
        crc = (uint16_t)(crc ^ (uint16_t)((unsigned)p[i] << 8));
        for (b = 0; b < 8; b++) {
            unsigned c = crc;

            c = (c & 0x8000u) ? ((c << 1) ^ 0x1021u) : (c << 1);
            crc = (uint16_t)c;
        }
    }
    return crc;
}

static uint16_t crc_itu(const uint8_t *p, size_t n)
{
    return crc_itu_update(0, p, n);
}

/* A descriptor tag (ECMA-167 3/7.2): its checksum, the block it claims to be
 * recorded at (skipped when loc is ~0, for tags whose location is relative to
 * something the caller does not track), and the CRC over the body that
 * follows it within len bytes. Returns the tag identifier, or -1. */
static int tag_check(const uint8_t *d, size_t len, uint32_t loc)
{
    uint8_t sum = 0;
    unsigned i;
    uint16_t crc_len;

    if (len < 16u)
        return -1;
    for (i = 0; i < 16u; i++) {
        if (i != 4u)
            sum = (uint8_t)(sum + d[i]);
    }
    if (sum != d[4])
        return -1;
    if (loc != ~(uint32_t)0 && get_le32(d + 12) != loc)
        return -1;
    crc_len = get_le16(d + 10);
    if (crc_len > len - 16u)
        return -1;
    if (crc_itu(d + 16, crc_len) != get_le16(d + 8))
        return -1;
    return get_le16(d);
}

static int read_abs(udf_vol *v, uint64_t blk, uint8_t *buf, uint64_t *cached)
{
    uint64_t off = blk * BS;

    if (cached != (uint64_t *)0 && *cached == off)
        return UDF_OK;
    if (v->read(v->read_ctx, off, BS, buf) != 0) {
        if (cached != (uint64_t *)0)
            *cached = NO_CACHE;
        return UDF_ERR_IO;
    }
    if (cached != (uint64_t *)0)
        *cached = off;
    return UDF_OK;
}

/* Partition-relative block to volume block, refusing anything outside the
 * partition: every lbn on the medium is data off p2 until proven otherwise. */
static int read_part(udf_vol *v, uint32_t lbn, uint8_t *buf, uint64_t *cached)
{
    if (lbn >= v->part_len)
        return UDF_ERR_CORRUPT;
    return read_abs(v, (uint64_t)v->part_start + lbn, buf, cached);
}

int udf_probe(udf_read_fn read, void *read_ctx)
{
    uint8_t b[BS];
    unsigned i;
    int bea = 0;

    if (read == (udf_read_fn)0)
        return UDF_ERR_BAD_ARG;
    /* The recognition sequence (ECMA-167 2/8.3): 2 KiB slots from byte
     * 32768, an ISO9660 descriptor set possibly in front, BEA01 opening the
     * extended area and NSR02/NSR03 inside it. A plain ISO9660 image has
     * CD001 slots and no NSR, which is what keeps this from claiming it. */
    for (i = 0; i < 32u; i++) {
        if (read(read_ctx, 32768u + (uint64_t)i * 2048u, 2048u, b) != 0)
            return UDF_ERR_IO;
        if (memcmp(b + 1, "BEA01", 5) == 0) {
            bea = 1;
            continue;
        }
        if (bea && (memcmp(b + 1, "NSR02", 5) == 0 ||
                    memcmp(b + 1, "NSR03", 5) == 0))
            return UDF_OK;
        /* Anything but a descriptor that may precede the extended area
         * (TEA01 included) ends the sequence without an NSR. */
        if (memcmp(b + 1, "CD001", 5) != 0 &&
            memcmp(b + 1, "BOOT2", 5) != 0 &&
            memcmp(b + 1, "CDW02", 5) != 0)
            break;
    }
    return UDF_ERR_BAD_SUPER;
}

/* A dstring (ECMA-167 1/7.2.12): compression ID, characters, and the used
 * length in the last byte. 8 is one byte per character, 16 is UCS-2
 * big-endian; UDF 2.60 adds 254 and 255 for the same two widths. */
static void label_from_dstring(const uint8_t *d, unsigned field, char *out,
                               unsigned cap)
{
    unsigned used = d[field - 1u];
    unsigned n = 0, i;

    out[0] = '\0';
    if (used == 0 || used > field - 1u)
        return;
    if (d[0] == 8u || d[0] == 254u) {
        for (i = 1; i < used && n + 1u < cap; i++)
            out[n++] = (d[i] >= 0x20u && d[i] < 0x7Fu) ? (char)d[i] : '?';
    } else if (d[0] == 16u || d[0] == 255u) {
        for (i = 1; i + 1u < used && n + 1u < cap; i += 2) {
            uint16_t c = (uint16_t)(((uint16_t)d[i] << 8) | d[i + 1u]);

            out[n++] = (c >= 0x20u && c < 0x7Fu) ? (char)c : '?';
        }
    }
    out[n] = '\0';
    while (n > 0 && out[n - 1u] == ' ')
        out[--n] = '\0';
}

int udf_mount(udf_vol *v, udf_read_fn read, void *read_ctx)
{
    uint8_t *b;
    uint32_t vds_loc, vds_len;
    uint32_t fsd_lbn = 0;
    uint16_t pd_num = 0, map_part = 0;
    int have_pd = 0, have_lvd = 0, have_pvd = 0;
    unsigned hops = 0, i;
    int rc;

    if (v == (udf_vol *)0 || read == (udf_read_fn)0)
        return UDF_ERR_BAD_ARG;
    memset(v, 0, sizeof(*v));
    v->read = read;
    v->read_ctx = read_ctx;
    v->scratch_off = NO_CACHE;
    b = v->scratch;

    rc = udf_probe(read, read_ctx);
    if (rc != UDF_OK)
        return rc;

    /* The anchor is at block 256 whatever else is on the disc (3/8.4.2.1).
     * Its recorded location doubles as the block size check: a 2048-byte
     * read of block 256 of a 512-byte-block image lands elsewhere. */
    rc = read_abs(v, 256u, b, (uint64_t *)0);
    if (rc != UDF_OK)
        return rc;
    if (tag_check(b, BS, 256u) != (int)TAG_AVDP)
        return UDF_ERR_BAD_SUPER;
    vds_len = get_le32(b + 16);
    vds_loc = get_le32(b + 20);

    /* The sequence ends at a Terminating Descriptor or at the end of its
     * extent, and a Volume Descriptor Pointer moves it to another extent. */
    for (;;) {
        uint32_t n = vds_len / BS;
        int moved = 0, ended = 0;

        if (n > VDS_BLOCKS_MAX)
            n = VDS_BLOCKS_MAX;
        for (i = 0; i < n && !moved && !ended; i++) {
            int tag;

            rc = read_abs(v, (uint64_t)vds_loc + i, b, (uint64_t *)0);
            if (rc != UDF_OK)
                return rc;
            tag = tag_check(b, BS, vds_loc + i);
            if (tag < 0)
                return UDF_ERR_CORRUPT;
            if ((unsigned)tag == TAG_TD) {
                ended = 1;
            } else if ((unsigned)tag == TAG_VDP) {
                if (++hops > VDP_HOPS_MAX)
                    return UDF_ERR_CORRUPT;
                vds_len = get_le32(b + 20);
                vds_loc = get_le32(b + 24);
                moved = 1;
            } else if ((unsigned)tag == TAG_PVD && !have_pvd) {
                if (!have_lvd)
                    label_from_dstring(b + 24, 32u, v->label,
                                       sizeof v->label);
                have_pvd = 1;
            } else if ((unsigned)tag == TAG_PD && !have_pd) {
                pd_num = get_le16(b + 22);
                v->part_start = get_le32(b + 188);
                v->part_len = get_le32(b + 192);
                have_pd = 1;
            } else if ((unsigned)tag == TAG_LVD && !have_lvd) {
                uint32_t nmaps = get_le32(b + 268);
                uint32_t mt_len = get_le32(b + 264);
                const uint8_t *m = b + 440;

                if (get_le32(b + 212) != BS)
                    return UDF_ERR_UNSUPPORTED;
                if (nmaps != 1u || mt_len < 6u || mt_len > BS - 440u)
                    return UDF_ERR_UNSUPPORTED;
                if (m[0] != 1u || m[1] != 6u)
                    return UDF_ERR_UNSUPPORTED;   /* type 2: metadata etc. */
                map_part = get_le16(m + 4);
                /* The File Set Descriptor, as a long_ad in the logical
                 * volume contents use (3/10.6.13, UDF 2.2.4.4). */
                if (get_le16(b + 248 + 8) != 0)
                    return UDF_ERR_CORRUPT;
                fsd_lbn = get_le32(b + 248 + 4);
                /* The logical volume identifier is the volume's label in
                 * UDF's own terms (UDF 2.2.4.3), and what blkid reports; the
                 * PVD's identifier is the fallback for a sequence that
                 * leaves it empty. */
                {
                    char lv[sizeof v->label];

                    label_from_dstring(b + 84, 128u, lv, sizeof lv);
                    if (lv[0] != '\0')
                        memcpy(v->label, lv, sizeof lv);
                }
                have_lvd = 1;
            }
        }
        if (!moved)
            break;
    }
    if (!have_pd || !have_lvd)
        return UDF_ERR_CORRUPT;
    if (map_part != pd_num || v->part_len == 0)
        return UDF_ERR_CORRUPT;

    rc = read_part(v, fsd_lbn, b, (uint64_t *)0);
    if (rc != UDF_OK)
        return rc;
    if (tag_check(b, BS, fsd_lbn) != (int)TAG_FSD)
        return UDF_ERR_CORRUPT;
    if (get_le16(b + 400 + 8) != 0)
        return UDF_ERR_UNSUPPORTED;
    v->root_lbn = get_le32(b + 400 + 4);
    return UDF_OK;
}

/* ---- File Entries ---- */

/* Appends one allocation descriptor to f, following a continuation. Returns
 * 1 when the descriptor list ended (a zero length), 0 to keep reading, 2 for a
 * continuation whose block is in *next, negative on error. */
static int ad_take(udf_file *f, uint32_t raw_len, uint32_t lbn, uint32_t *next)
{
    uint32_t len = raw_len & 0x3FFFFFFFu;
    uint32_t type = raw_len >> 30;

    if (len == 0)
        return 1;
    if (type == 3u) {
        *next = lbn;
        return 2;
    }
    if (f->nextents >= UDF_MAX_EXTENTS)
        return UDF_ERR_UNSUPPORTED;
    f->ext[f->nextents].lbn = lbn;
    f->ext[f->nextents].len = len;
    f->ext[f->nextents].recorded = (uint8_t)(type == 0u);
    f->nextents++;
    return 0;
}

/* Walks one area of allocation descriptors of the given kind. */
static int ad_area(udf_file *f, const uint8_t *p, uint32_t len, unsigned kind,
                   uint32_t *next)
{
    uint32_t step = (kind == 0u) ? 8u : 16u;
    uint32_t i;

    for (i = 0; i + step <= len; i += step) {
        uint32_t raw = get_le32(p + i);
        uint32_t lbn = get_le32(p + i + 4);
        int rc;

        if (kind == 1u && get_le16(p + i + 8) != 0)
            return UDF_ERR_UNSUPPORTED;   /* a second partition */
        rc = ad_take(f, raw, lbn, next);
        if (rc != 0)
            return rc;
    }
    return 0;
}

/* Loads the File Entry at icb into f: size, type, and where the data is. */
static int fe_load(udf_vol *v, uint32_t icb, udf_file *f, uint8_t *ftype)
{
    uint8_t *b = f->buf;
    uint32_t l_ea, l_ad, base;
    uint16_t flags;
    unsigned kind, hops = 0;
    uint32_t next = 0;
    int tag, rc;

    memset(f, 0, sizeof(*f));
    f->vol = v;
    f->icb_lbn = icb;
    f->buf_off = NO_CACHE;

    rc = read_part(v, icb, b, (uint64_t *)0);
    if (rc != UDF_OK)
        return rc;
    tag = tag_check(b, BS, icb);
    if (tag == (int)TAG_FE) {
        base = 176u;
        l_ea = get_le32(b + 168);
        l_ad = get_le32(b + 172);
    } else if (tag == (int)TAG_EFE) {
        base = 216u;
        l_ea = get_le32(b + 208);
        l_ad = get_le32(b + 212);
    } else {
        return UDF_ERR_CORRUPT;
    }
    /* Strategy 4 is a single direct entry; 4096 chains indirect entries,
     * which only rewritable media use (4/14.6.2). */
    if (get_le16(b + 20) != 4u)
        return UDF_ERR_UNSUPPORTED;
    if (l_ea > BS || l_ad > BS || base + l_ea + l_ad > BS)
        return UDF_ERR_CORRUPT;

    *ftype = b[27];
    flags = get_le16(b + 34);
    f->size = get_le64(b + 56);
    kind = flags & 7u;

    if (kind == 3u) {
        /* Embedded: the data is the allocation descriptor area. */
        if (f->size > l_ad)
            return UDF_ERR_CORRUPT;
        f->embedded = 1;
        f->inline_off = (uint16_t)(base + l_ea);
        return UDF_OK;
    }
    if (kind > 1u)
        return UDF_ERR_UNSUPPORTED;     /* extended_ad */

    rc = ad_area(f, b + base + l_ea, l_ad, kind, &next);
    while (rc == 2) {
        uint32_t al;

        if (++hops > AED_HOPS_MAX)
            return UDF_ERR_CORRUPT;
        rc = read_part(v, next, b, (uint64_t *)0);
        if (rc != UDF_OK)
            return rc;
        if (tag_check(b, BS, next) != (int)TAG_AED)
            return UDF_ERR_CORRUPT;
        al = get_le32(b + 20);
        if (al > BS - 24u)
            return UDF_ERR_CORRUPT;
        rc = ad_area(f, b + 24, al, kind, &next);
    }
    if (rc < 0)
        return rc;

    /* The extents must cover the file; a shortfall would leave a tail the
     * reader has no bytes for. Extents may run past it (the last block is
     * whole), which is not an error. */
    {
        uint64_t sum = 0;
        uint16_t i;

        for (i = 0; i < f->nextents; i++)
            sum += f->ext[i].len;
        if (sum < f->size)
            return UDF_ERR_CORRUPT;
    }
    return UDF_OK;
}

int udf_file_open(udf_vol *v, udf_file *f, const udf_dirent *e)
{
    uint8_t ftype;
    int rc;

    if (v == (udf_vol *)0 || f == (udf_file *)0 || e == (const udf_dirent *)0 ||
        e->is_dir)
        return UDF_ERR_BAD_ARG;
    rc = fe_load(v, e->icb_lbn, f, &ftype);
    if (rc != UDF_OK)
        return rc;
    /* A symlink, a device node or a FIFO: none is a byte stream to read. */
    if (ftype != FT_FILE)
        return UDF_ERR_SYMLINK;
    f->buf_off = NO_CACHE;
    return UDF_OK;
}

int udf_file_read(udf_file *f, uint64_t off, uint64_t len, void *buf)
{
    uint8_t *dst = (uint8_t *)buf;
    uint64_t start = 0;
    uint16_t i;

    if (f == (udf_file *)0 || f->vol == (udf_vol *)0 ||
        (buf == (void *)0 && len != 0))
        return UDF_ERR_BAD_ARG;
    if (off > f->size || len > f->size - off)
        return UDF_ERR_RANGE;
    if (len == 0)
        return UDF_OK;

    if (f->embedded) {
        int rc = read_part(f->vol, f->icb_lbn, f->buf, &f->buf_off);

        if (rc != UDF_OK)
            return rc;
        memcpy(dst, f->buf + f->inline_off + off, (size_t)len);
        return UDF_OK;
    }

    for (i = 0; i < f->nextents && len > 0; i++) {
        const udf_extent_rec *x = &f->ext[i];
        uint64_t xlen = x->len;

        if (off < start + xlen) {
            uint64_t in = off - start;
            uint64_t n = xlen - in;

            if (n > len)
                n = len;
            if (!x->recorded) {
                memset(dst, 0, (size_t)n);
            } else {
                uint64_t done = 0;

                if ((uint64_t)x->lbn + (in + n + BS - 1u) / BS >
                    f->vol->part_len)
                    return UDF_ERR_CORRUPT;
                while (done < n) {
                    uint64_t pos = in + done;
                    uint64_t inblk = pos % BS;
                    uint64_t chunk;

                    if (inblk == 0 && n - done >= BS) {
                        uint64_t vblk = (uint64_t)f->vol->part_start +
                                        x->lbn + pos / BS;

                        chunk = (n - done) - ((n - done) % BS);
                        if (f->vol->read(f->vol->read_ctx, vblk * BS, chunk,
                                         dst + done) != 0)
                            return UDF_ERR_IO;
                    } else {
                        int rc = read_part(f->vol,
                                           (uint32_t)(x->lbn + pos / BS),
                                           f->buf, &f->buf_off);

                        if (rc != UDF_OK)
                            return rc;
                        chunk = BS - inblk;
                        if (chunk > n - done)
                            chunk = n - done;
                        memcpy(dst + done, f->buf + inblk, (size_t)chunk);
                    }
                    done += chunk;
                }
            }
            dst += n;
            off += n;
            len -= n;
        }
        start += xlen;
    }
    return len == 0 ? UDF_OK : UDF_ERR_CORRUPT;
}

int udf_file_extents(udf_vol *v, const udf_dirent *e, udf_extent *out,
                     size_t cap, size_t *count)
{
    static udf_file f;          /* ~3.6 KB: keep off the loader's stack */
    uint64_t run_off = 0, run_len = 0, left;
    size_t n = 0;
    uint16_t i;
    int rc;

    if (count == (size_t *)0)
        return UDF_ERR_BAD_ARG;
    *count = 0;
    if (cap != 0 && out == (udf_extent *)0)
        return UDF_ERR_BAD_ARG;
    rc = udf_file_open(v, &f, e);
    if (rc != UDF_OK)
        return rc;
    if (f.embedded)
        return UDF_ERR_UNSUPPORTED;

    left = f.size;
    for (i = 0; i < f.nextents && left > 0; i++) {
        uint64_t off = ((uint64_t)v->part_start + f.ext[i].lbn) * BS;
        uint64_t len = f.ext[i].len < left ? f.ext[i].len : left;

        if (!f.ext[i].recorded)
            return UDF_ERR_UNSUPPORTED;
        left -= len;
        if (run_len != 0 && run_off + run_len == off) {
            run_len += len;
            continue;
        }
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
    if (run_len != 0) {
        if (n < cap) {
            out[n].vol_off = run_off;
            out[n].len = run_len;
        }
        n++;
    }
    *count = n;
    return n > cap ? UDF_ERR_NOSPACE : UDF_OK;
}

/* ---- directories ---- */

static int dir_open_icb(udf_vol *v, udf_dir *d, uint32_t icb)
{
    uint8_t ftype;
    int rc = fe_load(v, icb, &d->dir, &ftype);

    if (rc != UDF_OK)
        return rc;
    if (ftype != FT_DIR)
        return UDF_ERR_CORRUPT;
    d->dir.is_dir = 1;
    d->dir.buf_off = NO_CACHE;
    d->pos = 0;
    return UDF_OK;
}

int udf_dir_open_root(udf_vol *v, udf_dir *d)
{
    if (v == (udf_vol *)0 || d == (udf_dir *)0)
        return UDF_ERR_BAD_ARG;
    return dir_open_icb(v, d, v->root_lbn);
}

int udf_dir_open(udf_vol *v, udf_dir *d, const udf_dirent *e)
{
    if (v == (udf_vol *)0 || d == (udf_dir *)0 || e == (const udf_dirent *)0 ||
        !e->is_dir)
        return UDF_ERR_BAD_ARG;
    return dir_open_icb(v, d, e->icb_lbn);
}

/* The File Identifier (4/14.4.8) is a dstring without the length byte: a
 * compression ID and then the characters. */
static int fid_name(const uint8_t *id, uint32_t len, udf_dirent *out)
{
    uint32_t i, n = 0;

    if (len == 0)
        return UDF_ERR_CORRUPT;
    if (id[0] == 8u || id[0] == 254u) {
        for (i = 1; i < len && n < UDF_NAME_MAX; i++)
            out->name[n++] = id[i];
    } else if (id[0] == 16u || id[0] == 255u) {
        if (((len - 1u) & 1u) != 0)
            return UDF_ERR_CORRUPT;
        for (i = 1; i + 1u < len && n < UDF_NAME_MAX; i += 2)
            out->name[n++] =
                (uint16_t)(((uint16_t)id[i] << 8) | id[i + 1u]);
    } else {
        return UDF_ERR_CORRUPT;
    }
    out->name[n] = 0;
    out->name_len = (uint16_t)n;
    return UDF_OK;
}

int udf_dir_next(udf_dir *d, udf_dirent *out)
{
    if (d == (udf_dir *)0 || out == (udf_dirent *)0 ||
        d->dir.vol == (udf_vol *)0)
        return UDF_ERR_BAD_ARG;

    for (;;) {
        uint8_t fid[38 + 255 + 3];
        uint8_t fc, l_fi;
        uint16_t l_iu;
        uint32_t total, icb;
        uint8_t ftype;
        udf_vol *v = d->dir.vol;
        int rc;

        if (d->pos >= d->dir.size)
            return 0;
        if (d->dir.size - d->pos < 38u)
            return UDF_ERR_CORRUPT;
        rc = udf_file_read(&d->dir, d->pos, 38u, fid);
        if (rc != UDF_OK)
            return rc;
        l_fi = fid[19];
        l_iu = get_le16(fid + 36);
        total = (38u + l_iu + l_fi + 3u) & ~3u;
        if (total > d->dir.size - d->pos)
            return UDF_ERR_CORRUPT;
        /* The implementation use area comes before the identifier and can be
         * as long as its 16-bit length says; only the identifier is kept. */
        if (l_fi > 0) {
            rc = udf_file_read(&d->dir, d->pos + 38u + l_iu, l_fi, fid + 38);
            if (rc != UDF_OK)
                return rc;
        }
        /* The tag (4/7.2): checksum, identifier, and a CRC over the rest of
         * the descriptor, streamed in chunks from the directory so the
         * implementation use area's 16-bit length never sizes a buffer.
         * ECMA-167 counts the padding as part of a FID, and oscdimg's CRC
         * covers it; other writers stop at the identifier. Either span is
         * accepted and the CRC is checked over the one the tag declares.
         *
         * The tag location is not checked: a FID sits at a byte offset in a
         * stream, and the block its tag names depends on how the stream was
         * laid out on the medium. */
        {
            uint32_t crc_len = get_le16(fid + 10);
            uint32_t raw = 38u + l_iu + l_fi;
            uint16_t crc = 0;
            uint8_t sum = 0;
            uint32_t k;

            for (k = 0; k < 16u; k++) {
                if (k != 4u)
                    sum = (uint8_t)(sum + fid[k]);
            }
            if (sum != fid[4] || get_le16(fid) != TAG_FID)
                return UDF_ERR_CORRUPT;
            if (crc_len != raw - 16u && crc_len != total - 16u)
                return UDF_ERR_CORRUPT;
            for (k = 16; k < 16u + crc_len;) {
                uint8_t chunk[64];
                uint32_t c = 16u + crc_len - k;

                if (c > sizeof chunk)
                    c = sizeof chunk;
                rc = udf_file_read(&d->dir, d->pos + k, c, chunk);
                if (rc != UDF_OK)
                    return rc;
                crc = crc_itu_update(crc, chunk, c);
                k += c;
            }
            if (crc != get_le16(fid + 8))
                return UDF_ERR_CORRUPT;
        }
        fc = fid[18];
        icb = get_le32(fid + 20 + 4);
        d->pos += total;

        if (fc & (FC_PARENT | FC_DELETED))
            continue;
        if (get_le16(fid + 20 + 8) != 0)
            return UDF_ERR_UNSUPPORTED;

        memset(out, 0, sizeof(*out));
        rc = fid_name(fid + 38, l_fi, out);
        if (rc != UDF_OK)
            return rc;
        out->icb_lbn = icb;
        out->is_dir = (uint8_t)((fc & FC_DIR) != 0);

        /* Size and type live in the child's File Entry, one block read
         * through the volume's scratch buffer. */
        rc = read_part(v, icb, v->scratch, &v->scratch_off);
        if (rc != UDF_OK)
            return rc;
        {
            int tag = tag_check(v->scratch, BS, icb);

            if (tag != (int)TAG_FE && tag != (int)TAG_EFE)
                return UDF_ERR_CORRUPT;
        }
        ftype = v->scratch[27];
        out->size = get_le64(v->scratch + 56);
        if (out->is_dir != (ftype == FT_DIR))
            return UDF_ERR_CORRUPT;
        out->is_regular = (uint8_t)(ftype == FT_FILE);
        return 1;
    }
}

/* ---- edits ---- */

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

/* The tag's CRC over crc_len body bytes, then its checksum. */
static void tag_seal(uint8_t *d, uint16_t crc_len)
{
    uint8_t sum = 0;
    unsigned i;

    put_le16(d + 10, crc_len);
    put_le16(d + 8, crc_itu(d + 16, crc_len));
    for (i = 0; i < 16u; i++) {
        if (i != 4u)
            sum = (uint8_t)(sum + d[i]);
    }
    d[4] = sum;
}

static int name_is_ascii(const uint16_t *name, uint16_t len, const char *s,
                         size_t n)
{
    size_t i;

    if (len != n)
        return 0;
    for (i = 0; i < n; i++) {
        uint16_t a = name[i], b = (uint16_t)(unsigned char)s[i];

        if (a >= 'A' && a <= 'Z')
            a = (uint16_t)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (uint16_t)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

int udf_lookup(udf_vol *v, const char *path, udf_dirent *out)
{
    static udf_dir d;
    const char *p = path;
    int have = 0, rc;

    if (v == (udf_vol *)0 || path == (const char *)0 ||
        out == (udf_dirent *)0)
        return UDF_ERR_BAD_ARG;
    rc = udf_dir_open_root(v, &d);
    if (rc != UDF_OK)
        return rc;
    for (;;) {
        size_t n = 0;
        int found = 0;

        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        while (p[n] != '\0' && p[n] != '/')
            n++;
        if (have) {
            if (!out->is_dir)
                return UDF_ERR_NOT_FOUND;
            rc = udf_dir_open(v, &d, out);
            if (rc != UDF_OK)
                return rc;
        }
        while ((rc = udf_dir_next(&d, out)) == 1) {
            if (name_is_ascii(out->name, out->name_len, p, n)) {
                found = 1;
                break;
            }
        }
        if (rc < 0)
            return rc;
        if (!found)
            return UDF_ERR_NOT_FOUND;
        have = 1;
        p += n;
    }
    return have ? UDF_OK : UDF_ERR_BAD_ARG;
}

int udf_pd_blocks(udf_vol *v, uint32_t out[2], size_t *count)
{
    uint8_t *b;
    uint32_t seq_len[2], seq_loc[2];
    size_t s;
    int rc;

    if (v == (udf_vol *)0 || out == (uint32_t *)0 || count == (size_t *)0)
        return UDF_ERR_BAD_ARG;
    *count = 0;
    b = v->scratch;
    v->scratch_off = NO_CACHE;
    rc = read_abs(v, 256u, b, (uint64_t *)0);
    if (rc != UDF_OK)
        return rc;
    if (tag_check(b, BS, 256u) != (int)TAG_AVDP)
        return UDF_ERR_BAD_SUPER;
    seq_len[0] = get_le32(b + 16);
    seq_loc[0] = get_le32(b + 20);
    seq_len[1] = get_le32(b + 24);
    seq_loc[1] = get_le32(b + 28);
    for (s = 0; s < 2u; s++) {
        uint32_t n = seq_len[s] / BS, i;

        if (n > VDS_BLOCKS_MAX)
            n = VDS_BLOCKS_MAX;
        for (i = 0; i < n; i++) {
            int tag;

            rc = read_abs(v, (uint64_t)seq_loc[s] + i, b, (uint64_t *)0);
            if (rc != UDF_OK)
                return rc;
            tag = tag_check(b, BS, seq_loc[s] + i);
            if (tag == (int)TAG_TD || tag < 0)
                break;
            if (tag == (int)TAG_PD) {
                out[(*count)++] = seq_loc[s] + i;
                break;
            }
        }
    }
    return *count != 0 ? UDF_OK : UDF_ERR_CORRUPT;
}

int udf_pd_set_length(uint8_t *blk, uint32_t loc, uint32_t len)
{
    if (blk == (uint8_t *)0)
        return UDF_ERR_BAD_ARG;
    if (tag_check(blk, BS, loc) != (int)TAG_PD)
        return UDF_ERR_CORRUPT;
    put_le32(blk + 192, len);
    tag_seal(blk, get_le16(blk + 10));
    return UDF_OK;
}

int udf_fe_append(uint8_t *blk, uint32_t loc, uint64_t add, uint32_t lbn,
                  uint32_t *moved)
{
    uint32_t base, l_ea, l_ad, adsz, n, last_raw, last_len, rem, i;
    uint64_t info, blocks = 0;
    uint16_t flags;
    unsigned kind;
    uint8_t *ads, *last;
    int tag, efe;

    if (blk == (uint8_t *)0 || moved == (uint32_t *)0 || add == 0)
        return UDF_ERR_BAD_ARG;
    tag = tag_check(blk, BS, loc);
    if (tag != (int)TAG_FE && tag != (int)TAG_EFE)
        return UDF_ERR_CORRUPT;
    efe = tag == (int)TAG_EFE;
    base = efe ? 216u : 176u;
    l_ea = get_le32(blk + (efe ? 208u : 168u));
    l_ad = get_le32(blk + (efe ? 212u : 172u));
    if (get_le16(blk + 20) != 4u || l_ea > BS || l_ad > BS ||
        base + l_ea + l_ad > BS)
        return UDF_ERR_UNSUPPORTED;
    flags = get_le16(blk + 34);
    kind = flags & 7u;
    if (kind > 1u)
        return UDF_ERR_UNSUPPORTED;
    adsz = kind == 0u ? 8u : 16u;
    if (l_ad == 0 || l_ad % adsz != 0)
        return UDF_ERR_UNSUPPORTED;
    n = l_ad / adsz;
    ads = blk + base + l_ea;
    for (i = 0; i < n; i++) {
        uint32_t raw = get_le32(ads + (size_t)i * adsz);

        /* Only recorded extents, and no continuation to another block. */
        if ((raw >> 30) != 0 || (raw & 0x3fffffffu) == 0)
            return UDF_ERR_UNSUPPORTED;
    }
    last = ads + (size_t)(n - 1u) * adsz;
    last_raw = get_le32(last);
    last_len = last_raw & 0x3fffffffu;
    rem = last_len % BS;
    if ((uint64_t)rem + add > 0x3fffffffu - BS)
        return UDF_ERR_UNSUPPORTED;
    info = get_le64(blk + 56);

    if (last_len == rem) {
        /* The whole last extent is a partial block: it moves entirely, and
         * the new extent takes its descriptor's place. */
        n--;
    } else {
        put_le32(last, last_len - rem);
        if (base + l_ea + l_ad + adsz > BS)
            return UDF_ERR_NOSPACE;
    }
    last = ads + (size_t)n * adsz;
    memset(last, 0, adsz);
    put_le32(last, (uint32_t)(rem + add));
    put_le32(last + 4, lbn);
    if (kind == 1u)
        put_le16(last + 8, get_le16(ads + 8));   /* same partition */
    n++;
    l_ad = n * adsz;
    put_le32(blk + (efe ? 212u : 172u), l_ad);
    put_le64(blk + 56, info + add);
    if (efe)
        put_le64(blk + 64, get_le64(blk + 64) + add);
    for (i = 0; i < n; i++)
        blocks += ((get_le32(ads + (size_t)i * adsz) & 0x3fffffffu) + BS - 1u) / BS;
    put_le64(blk + (efe ? 72u : 64u), blocks);
    memset(ads + l_ad, 0, BS - base - l_ea - l_ad);
    tag_seal(blk, (uint16_t)(base + l_ea + l_ad - 16u));
    *moved = rem;
    return UDF_OK;
}

int udf_avdp_at(uint8_t *blk, uint32_t loc)
{
    if (blk == (uint8_t *)0)
        return UDF_ERR_BAD_ARG;
    if (tag_check(blk, BS, ~(uint32_t)0) != (int)TAG_AVDP)
        return UDF_ERR_CORRUPT;
    put_le32(blk + 12, loc);
    tag_seal(blk, get_le16(blk + 10));
    return UDF_OK;
}
