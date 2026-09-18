/* Read-only exFAT per the Microsoft exFAT specification (August 2019).
 * Field offsets referenced by section: VBR fields spec 3.1, directory entry
 * layouts spec 6.x, FAT semantics spec 4.1.
 */

#include <fs/exfat.h>
#include <lib/string.h>

#define DIRENT_SIZE 32u

/* Directory entry types (spec 6.2, with the in-use bit 0x80 set).
 *
 * One file is not one 32-byte record but a SET of consecutive ones, and every
 * reader has to reassemble it:
 *
 *   ENT_FILE    attributes, and SecondaryCount at offset 1: how many records
 *               follow in this set.
 *   ENT_STREAM  exactly one, immediately after. Carries FirstCluster,
 *               DataLength, ValidDataLength, NameLength, and the NoFatChain
 *               flag. This is where a file's actual identity lives.
 *   ENT_NAME    ceil(NameLength / 15) of them, 15 UTF-16 units each, no
 *               terminator and no length of their own.
 *
 * Nothing separates one set from the next except the type bytes, so a reader
 * that loses sync mid-set reads a name fragment as a file. exfat_dir_next
 * resynchronises by skipping to the next ENT_FILE rather than trying to
 * repair the set. */
#define ENT_END        0x00
#define ENT_FILE       0x85
#define ENT_STREAM     0xC0
#define ENT_NAME       0xC1

#define ATTR_DIRECTORY 0x0010u

#define FLAG_NO_FAT_CHAIN 0x02u

#define FAT_ENTRY_BAD  0xFFFFFFF7u
#define FAT_ENTRY_EOF  0xFFFFFFFFu

#define OFF_INVALID (~(uint64_t)0)

static uint32_t sector_size(const exfat_vol *v)
{
    return 1u << v->bps_shift;
}

static uint32_t cluster_bytes(const exfat_vol *v)
{
    return 1u << (v->bps_shift + v->spc_shift);
}

static int cluster_valid(const exfat_vol *v, uint32_t cl)
{
    return cl >= 2 && cl < v->cluster_count + 2;
}

/* Volume byte offset of a data cluster (heap starts at cluster 2). */
static uint64_t cluster_off(const exfat_vol *v, uint32_t cl)
{
    return ((uint64_t)v->cluster_heap_offset << v->bps_shift) +
           ((uint64_t)(cl - 2) << (v->bps_shift + v->spc_shift));
}

int exfat_mount(exfat_vol *v, exfat_read_fn read, void *read_ctx)
{
    uint8_t vbr[512];

    if (v == (void *)0 || read == (void *)0)
        return EXFAT_ERR_BAD_ARG;

    memset(v, 0, sizeof(*v));
    v->read = read;
    v->read_ctx = read_ctx;
    v->fat_cache_off = OFF_INVALID;

    /* Every VBR field we need lives in the first 512 bytes regardless of
     * the volume's sector size. */
    if (read(read_ctx, 0, sizeof(vbr), vbr) != 0)
        return EXFAT_ERR_IO;

    if (memcmp(vbr + 3, "EXFAT   ", 8) != 0)
        return EXFAT_ERR_BAD_SUPER;
    if (vbr[510] != 0x55 || vbr[511] != 0xAA)
        return EXFAT_ERR_BAD_SUPER;

    v->bps_shift = vbr[108];
    v->spc_shift = vbr[109];
    if (v->bps_shift < 9 || v->bps_shift > 12)
        return EXFAT_ERR_BAD_SUPER;
    if (v->spc_shift > 25u - v->bps_shift) /* spec: cluster <= 32 MB */
        return EXFAT_ERR_BAD_SUPER;

    v->fat_offset = get_le32(vbr + 80);
    v->cluster_heap_offset = get_le32(vbr + 88);
    v->cluster_count = get_le32(vbr + 92);
    v->root_cluster = get_le32(vbr + 96);

    if (v->fat_offset == 0 || v->cluster_heap_offset == 0 ||
        v->cluster_count == 0)
        return EXFAT_ERR_BAD_SUPER;
    if (!cluster_valid(v, v->root_cluster))
        return EXFAT_ERR_BAD_SUPER;

    /* VolumeFlags bit 0 selects the active FAT; a second FAT only exists
     * for TexFAT, which flash media does not use. First FAT always. */
    return EXFAT_OK;
}

/* FAT chain lookup: *next = 0 signals end of chain. */
static int next_cluster(exfat_vol *v, uint32_t cl, uint32_t *next)
{
    uint32_t ss = sector_size(v);
    uint64_t entry_off = ((uint64_t)v->fat_offset << v->bps_shift) +
                         (uint64_t)cl * 4u;
    uint64_t sec_off = entry_off & ~((uint64_t)ss - 1u);
    uint32_t val;

    if (!cluster_valid(v, cl))
        return EXFAT_ERR_CORRUPT;

    if (v->fat_cache_off != sec_off) {
        if (v->read(v->read_ctx, sec_off, ss, v->fat_cache) != 0)
            return EXFAT_ERR_IO;
        v->fat_cache_off = sec_off;
    }

    val = get_le32(v->fat_cache + (entry_off - sec_off));
    if (val == FAT_ENTRY_EOF) {
        *next = 0;
        return EXFAT_OK;
    }
    if (val == FAT_ENTRY_BAD || !cluster_valid(v, val))
        return EXFAT_ERR_CORRUPT;
    *next = val;
    return EXFAT_OK;
}

/* The root has no directory entry anywhere, so nothing states its length: it
 * ends where its FAT chain ends. ~0 is the sentinel that switches the
 * iterator's bound off, leaving ENT_END or the end of the chain to stop it.
 * Every other directory carries a DataLength and is bounded by it. */
int exfat_dir_open_root(exfat_vol *v, exfat_dir *d)
{
    if (v == (void *)0 || d == (void *)0)
        return EXFAT_ERR_BAD_ARG;
    memset(d, 0, sizeof(*d));
    d->vol = v;
    d->first_cluster = v->root_cluster;
    d->no_fat_chain = 0;
    d->size = ~(uint64_t)0;
    d->cur_cluster = v->root_cluster;
    d->sec_buf_off = OFF_INVALID;
    return EXFAT_OK;
}

int exfat_dir_open(exfat_vol *v, exfat_dir *d, const exfat_dirent *e)
{
    if (v == (void *)0 || d == (void *)0 || e == (void *)0 || !e->is_dir)
        return EXFAT_ERR_BAD_ARG;
    if (e->first_cluster != 0 && !cluster_valid(v, e->first_cluster))
        return EXFAT_ERR_CORRUPT;
    memset(d, 0, sizeof(*d));
    d->vol = v;
    d->first_cluster = e->first_cluster;
    d->no_fat_chain = e->no_fat_chain;
    d->size = e->size;
    d->cur_cluster = e->first_cluster;
    d->sec_buf_off = OFF_INVALID;
    return EXFAT_OK;
}

/* Fetch the next raw 32-byte entry; 1 = got one, 0 = end of stream. */
static int dir_read_raw(exfat_dir *d, uint8_t out[DIRENT_SIZE])
{
    exfat_vol *v = d->vol;
    uint32_t cb = cluster_bytes(v);
    uint32_t ss = sector_size(v);
    uint64_t off_in_cluster;
    uint64_t abs;
    uint64_t sec_off;

    if (d->first_cluster == 0)
        return 0; /* empty directory */
    if (d->size != ~(uint64_t)0 && d->pos + DIRENT_SIZE > d->size)
        return 0;

    /* cluster_pos is the file offset at which the current cluster began, so
     * the difference is the offset within it. Tracking the base rather than a
     * running remainder is what keeps this correct for a fragmented chain,
     * where cluster n of the file is not cluster first + n on disk. */
    off_in_cluster = d->pos - d->cluster_pos;
    if (off_in_cluster == cb) {
        /* Cluster boundary: advance. NoFatChain means the next cluster is
         * simply the next one on disk; otherwise it costs a FAT read. */
        if (d->no_fat_chain) {
            uint32_t next = d->cur_cluster + 1;
            if (!cluster_valid(v, next))
                return EXFAT_ERR_CORRUPT;
            d->cur_cluster = next;
        } else {
            uint32_t next;
            int rc = next_cluster(v, d->cur_cluster, &next);
            if (rc != EXFAT_OK)
                return rc;
            if (next == 0)
                return 0; /* chain ended */
            d->cur_cluster = next;
        }
        d->cluster_pos = d->pos;
        off_in_cluster = 0;
    }

    abs = cluster_off(v, d->cur_cluster) + off_in_cluster;
    sec_off = abs & ~((uint64_t)ss - 1u);
    if (d->sec_buf_off != sec_off) {
        if (v->read(v->read_ctx, sec_off, ss, d->sec_buf) != 0)
            return EXFAT_ERR_IO;
        d->sec_buf_off = sec_off;
    }

    memcpy(out, d->sec_buf + (abs - sec_off), DIRENT_SIZE);
    d->pos += DIRENT_SIZE;
    return 1;
}

int exfat_file_open(exfat_vol *v, exfat_file *f, const exfat_dirent *e)
{
    if (v == (void *)0 || f == (void *)0 || e == (void *)0)
        return EXFAT_ERR_BAD_ARG;
    if (e->is_dir)
        return EXFAT_ERR_BAD_ARG;
    if (e->size != 0 && !cluster_valid(v, e->first_cluster))
        return EXFAT_ERR_CORRUPT;

    memset(f, 0, sizeof(*f));
    f->vol = v;
    f->first_cluster = e->first_cluster;
    f->no_fat_chain = e->no_fat_chain;
    f->size = e->size;
    f->valid_len = (e->valid_len > e->size) ? e->size : e->valid_len;
    f->cur_cluster = e->first_cluster;
    f->cur_index = 0;
    f->sec_buf_off = OFF_INVALID;
    return EXFAT_OK;
}

/* Resolve the index'th cluster of the file, moving the cached cursor.
 *
 * A FAT chain is singly linked, so there is no way to step backwards: a seek
 * to a lower index rewinds to the first cluster and walks forward again. That
 * makes a backward seek O(index) FAT reads, which is acceptable only because
 * every caller in the loader reads forward. A random-access consumer would
 * want a cached cluster map instead.
 *
 * NoFatChain files have no chain to walk and answer in constant time. */
static int file_cluster_at(exfat_file *f, uint64_t index, uint32_t *out)
{
    exfat_vol *v = f->vol;

    if (f->no_fat_chain) {
        uint64_t cl = (uint64_t)f->first_cluster + index;

        if (cl > 0xFFFFFFFFu || !cluster_valid(v, (uint32_t)cl))
            return EXFAT_ERR_CORRUPT;
        *out = (uint32_t)cl;
        return EXFAT_OK;
    }

    if (index < f->cur_index) {
        f->cur_cluster = f->first_cluster;
        f->cur_index = 0;
    }
    while (f->cur_index < index) {
        uint32_t next;
        int rc = next_cluster(v, f->cur_cluster, &next);

        if (rc != EXFAT_OK)
            return rc;
        if (next == 0)
            return EXFAT_ERR_CORRUPT; /* chain shorter than the size claims */
        f->cur_cluster = next;
        f->cur_index++;
    }
    *out = f->cur_cluster;
    return EXFAT_OK;
}

/* Move run bytes from volume offset abs into dst. Sector-aligned spans go
 * straight to the caller's buffer; only the ragged ends of an unaligned
 * request pay for the bounce buffer. */
static int file_read_span(exfat_file *f, uint64_t abs, uint64_t run,
                          uint8_t *dst)
{
    exfat_vol *v = f->vol;
    uint32_t ss = sector_size(v);

    while (run > 0) {
        uint64_t sec_off = abs & ~((uint64_t)ss - 1u);
        uint32_t in_sec = (uint32_t)(abs - sec_off);

        if (in_sec == 0 && run >= ss) {
            uint64_t whole = (run / ss) * ss;

            if (v->read(v->read_ctx, abs, whole, dst) != 0)
                return EXFAT_ERR_IO;
            dst += whole;
            abs += whole;
            run -= whole;
        } else {
            uint32_t take = ss - in_sec;

            if ((uint64_t)take > run)
                take = (uint32_t)run;
            if (f->sec_buf_off != sec_off) {
                if (v->read(v->read_ctx, sec_off, ss, f->sec_buf) != 0) {
                    f->sec_buf_off = OFF_INVALID;
                    return EXFAT_ERR_IO;
                }
                f->sec_buf_off = sec_off;
            }
            memcpy(dst, f->sec_buf + in_sec, take);
            dst += take;
            abs += take;
            run -= take;
        }
    }
    return EXFAT_OK;
}

int exfat_file_read(exfat_file *f, uint64_t off, uint64_t len, void *buf)
{
    exfat_vol *v;
    uint8_t *dst = buf;
    uint32_t cb;

    if (f == (void *)0 || f->vol == (void *)0)
        return EXFAT_ERR_BAD_ARG;
    if (buf == (void *)0 && len != 0)
        return EXFAT_ERR_BAD_ARG;
    if (off > f->size || len > f->size - off)
        return EXFAT_ERR_RANGE;
    if (len == 0)
        return EXFAT_OK;
    if (f->first_cluster == 0)
        return EXFAT_ERR_CORRUPT;

    /* The tail between ValidDataLength and DataLength was never written and
     * the spec defines it to read as zeros; the device holds stale bytes
     * there, so serving them would leak whatever previously occupied the
     * clusters. Split the request: zeros for the tail, device for the rest. */
    if (off + len > f->valid_len) {
        uint64_t zeros = (off >= f->valid_len) ? len
                                               : off + len - f->valid_len;

        memset(dst + (len - zeros), 0, (size_t)zeros);
        len -= zeros;
        if (len == 0)
            return EXFAT_OK;
    }

    v = f->vol;
    cb = cluster_bytes(v);

    while (len > 0) {
        uint32_t in_cluster = (uint32_t)(off % cb);
        uint64_t run = cb - in_cluster;
        uint32_t cl;
        int rc = file_cluster_at(f, off / cb, &cl);

        if (rc != EXFAT_OK)
            return rc;
        if (f->no_fat_chain) {
            /* Contiguous allocation: one span covers the whole request, so
             * validate its far end and skip the per-cluster loop. */
            uint32_t end_cl;

            rc = file_cluster_at(f, (off + len - 1) / cb, &end_cl);
            if (rc != EXFAT_OK)
                return rc;
            run = len;
        } else if (run > len) {
            run = len;
        }

        rc = file_read_span(f, cluster_off(v, cl) + in_cluster, run, dst);
        if (rc != EXFAT_OK)
            return rc;
        dst += run;
        off += run;
        len -= run;
    }
    return EXFAT_OK;
}

/* Append one extent, or just count it when the caller's array is full. The
 * count is kept honest past the end of the array so a short call can report
 * exactly how much room it needed. */
static void extent_emit(exfat_extent *out, size_t cap, size_t *need,
                        uint64_t off, uint64_t len)
{
    if (*need < cap) {
        out[*need].vol_off = off;
        out[*need].len = len;
    }
    (*need)++;
}

int exfat_file_extents(exfat_vol *v, const exfat_dirent *e,
                       exfat_extent *out, size_t cap, size_t *count)
{
    uint64_t remaining;
    uint64_t run_off = 0;
    uint64_t run_len = 0;
    uint32_t cb;
    uint32_t cl;
    size_t need = 0;

    if (v == (void *)0 || e == (void *)0 || count == (void *)0)
        return EXFAT_ERR_BAD_ARG;
    if (out == (void *)0 && cap != 0)
        return EXFAT_ERR_BAD_ARG;
    if (e->is_dir)
        return EXFAT_ERR_BAD_ARG;

    *count = 0;
    if (e->size == 0)
        return EXFAT_OK; /* no allocation, no extents: not an error */
    if (!cluster_valid(v, e->first_cluster))
        return EXFAT_ERR_CORRUPT;

    cb = cluster_bytes(v);
    cl = e->first_cluster;

    if (e->no_fat_chain) {
        /* Contiguous allocation, which is what a file dropped onto a freshly
         * formatted stick almost always is: one extent, no FAT reads at all.
         * The far end is still validated, because a NoFatChain file whose
         * size runs off the end of the heap is corruption we must not hand
         * on as a mapping. */
        /* Overflow-safe round-up: `(size + cb - 1) / cb` wraps for a
         * hostile size near 2^64, yielding clusters == 0 and a bogus
         * in-range `last` that would map the whole claimed size onto the
         * volume. Divide first instead; nothing here can wrap. */
        uint64_t clusters = e->size / cb + (e->size % cb != 0 ? 1 : 0);
        uint64_t last = (uint64_t)cl + clusters - 1;

        if (last > 0xFFFFFFFFu || !cluster_valid(v, (uint32_t)last))
            return EXFAT_ERR_CORRUPT;
        extent_emit(out, cap, &need, cluster_off(v, cl), e->size);
        *count = need;
        return need <= cap ? EXFAT_OK : EXFAT_ERR_NOSPACE;
    }

    remaining = e->size;
    while (remaining > 0) {
        uint64_t off = cluster_off(v, cl);
        uint64_t take = (remaining < cb) ? remaining : cb;
        uint32_t next;
        int rc;

        /* Fragmentation is a property of the FAT chain, not of the file:
         * neighbouring clusters merge into the run in progress and only a
         * jump starts a new extent. */
        if (run_len != 0 && run_off + run_len == off) {
            run_len += take;
        } else {
            if (run_len != 0)
                extent_emit(out, cap, &need, run_off, run_len);
            run_off = off;
            run_len = take;
        }

        remaining -= take;
        if (remaining == 0)
            break;

        rc = next_cluster(v, cl, &next);
        if (rc != EXFAT_OK)
            return rc;
        if (next == 0)
            return EXFAT_ERR_CORRUPT; /* chain shorter than the size claims */
        cl = next;
    }
    extent_emit(out, cap, &need, run_off, run_len);

    *count = need;
    return need <= cap ? EXFAT_OK : EXFAT_ERR_NOSPACE;
}

int exfat_dir_next(exfat_dir *d, exfat_dirent *out)
{
    uint8_t ent[DIRENT_SIZE];

    if (d == (void *)0 || out == (void *)0)
        return EXFAT_ERR_BAD_ARG;

    for (;;) {
        int rc = dir_read_raw(d, ent);
        uint8_t sec_count;
        uint8_t name_len;
        uint8_t name_got;
        uint16_t attrs;
        uint8_t i;

        if (rc <= 0)
            return rc;
        if (ent[0] == ENT_END)
            return 0;
        if (ent[0] != ENT_FILE)
            continue; /* unused, bitmap, up-case, label, vendor, ... */
        /* From here to the end of the loop body, every `continue` is a
         * resync: the set is abandoned and scanning resumes at whatever
         * record comes next, which will be skipped by this same test until
         * the next ENT_FILE. That is why a malformed set costs its own
         * entries and nothing else. */

        /* File directory entry (spec 6.4). */
        sec_count = ent[1];
        attrs = get_le16(ent + 4);
        if (sec_count < 2)
            continue; /* spec requires stream + at least one name entry */

        /* Stream extension must follow immediately (spec 6.5). */
        rc = dir_read_raw(d, ent);
        if (rc < 0)
            return rc;
        if (rc == 0 || ent[0] != ENT_STREAM)
            continue; /* tolerate corruption: skip the set */

        memset(out, 0, sizeof(*out));
        out->is_dir = (attrs & ATTR_DIRECTORY) != 0;
        out->no_fat_chain = (ent[1] & FLAG_NO_FAT_CHAIN) != 0;
        name_len = ent[3];
        out->first_cluster = get_le32(ent + 20);
        /* DataLength (offset 24) is the file's length; ValidDataLength
         * (offset 8) is how much was actually written, and the spec says
         * bytes between the two read as zeros. A sparse copy onto the
         * stick produces valid < data, so reporting valid as the size
         * truncated such files (found via a sparse-copied fixture image,
         * 2026-09-13). A valid claim beyond data is corruption, clamped
         * rather than propagated. */
        out->size = get_le64(ent + 24);
        out->valid_len = out->is_dir ? out->size : get_le64(ent + 8);
        if (out->valid_len > out->size)
            out->valid_len = out->size;

        /* Name entries: 15 UTF-16 units each (spec 6.7). */
        name_got = 0;
        for (i = 1; i < sec_count && name_got < name_len; i++) {
            uint8_t take;
            uint8_t j;

            rc = dir_read_raw(d, ent);
            if (rc < 0)
                return rc;
            if (rc == 0)
                return 0;
            if (ent[0] != ENT_NAME)
                break; /* vendor secondary or corruption: name over */
            take = (uint8_t)(name_len - name_got);
            if (take > 15)
                take = 15;
            for (j = 0; j < take; j++)
                out->name[name_got + j] = get_le16(ent + 2 + 2u * j);
            name_got = (uint8_t)(name_got + take);
        }
        if (name_got == 0)
            continue; /* nameless set: not a real file */
        out->name[name_got] = 0;
        out->name_len = name_got;
        return 1;
    }
}
