/* WIM reading and the winpeshl patch. See falcon-core/src/wim/wim.h.
 *
 * Layout references are wimlib's documentation of the format (the header,
 * the 24-byte resource header with a 56-bit stored size, the 50-byte lookup
 * table entry, the chunk table ahead of a compressed resource's chunks, and
 * the on-disk dentry), which is what Microsoft's own images were checked
 * against: `wimlib-imagex info/extract/verify` reads every WIM this code
 * writes in the tests.
 */

#include <wim/wim.h>
#include <wim/lzx.h>

#include <crypto/sha1.h>
#include <lib/string.h>

#define HDR_FLAG_COMPRESSION 0x00000002u
#define HDR_FLAG_SPANNED     0x00000008u
#define HDR_FLAG_XPRESS      0x00020000u
#define HDR_FLAG_LZX         0x00040000u
#define HDR_FLAG_LZMS        0x00080000u
#define RES_SPANNED          0x08u
#define RES_SOLID            0x10u
#define CHUNK                32768u
#define DENTRY_FIXED         102u
#define ATTR_DIRECTORY       0x10u

/* ---- little-endian fields ---- */

static uint16_t ld16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t ld64(const uint8_t *p)
{
    return (uint64_t)ld32(p) | ((uint64_t)ld32(p + 4) << 32);
}

static void st16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void st32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void st64(uint8_t *p, uint64_t v)
{
    st32(p, (uint32_t)v);
    st32(p + 4, (uint32_t)(v >> 32));
}

static void reshdr_load(const uint8_t *p, wim_reshdr *r)
{
    r->size = ld64(p) & 0x00ffffffffffffffull;
    r->flags = p[7];
    r->offset = ld64(p + 8);
    r->orig = ld64(p + 16);
}

static void reshdr_store(uint8_t *p, const wim_reshdr *r)
{
    st64(p, r->size & 0x00ffffffffffffffull);
    p[7] = r->flags;
    st64(p + 8, r->offset);
    st64(p + 16, r->orig);
}

static int in_file(const wim_info *w, const wim_reshdr *r)
{
    return r->offset <= w->file_size && r->size <= w->file_size - r->offset;
}

/* ---- the header ---- */

int wim_open(wim_read_fn read, void *ctx, uint64_t size, wim_info *w)
{
    static const uint8_t magic[8] = { 'M', 'S', 'W', 'I', 'M', 0, 0, 0 };
    uint32_t comp;

    if (read == (wim_read_fn)0 || w == (wim_info *)0)
        return WIM_ERR_BAD_ARG;
    memset(w, 0, sizeof *w);
    if (size < WIM_HDR_LEN)
        return WIM_ERR_FORMAT;
    if (read(ctx, 0, WIM_HDR_LEN, w->hdr) != 0)
        return WIM_ERR_IO;
    if (memcmp(w->hdr, magic, sizeof magic) != 0 ||
        ld32(w->hdr + 8) != WIM_HDR_LEN)
        return WIM_ERR_FORMAT;
    w->file_size = size;
    w->flags = ld32(w->hdr + 16);
    w->chunk_size = ld32(w->hdr + 20);
    if (w->chunk_size == 0)
        w->chunk_size = CHUNK;
    w->image_count = ld32(w->hdr + 44);
    w->boot_index = ld32(w->hdr + 120);
    reshdr_load(w->hdr + 48, &w->lookup);
    reshdr_load(w->hdr + 72, &w->xml);
    reshdr_load(w->hdr + 96, &w->boot_meta);

    comp = w->flags & (HDR_FLAG_XPRESS | HDR_FLAG_LZX | HDR_FLAG_LZMS);
    if ((w->flags & HDR_FLAG_SPANNED) != 0 || ld16(w->hdr + 40) != 1 ||
        ld16(w->hdr + 42) != 1)
        return WIM_ERR_FORMAT;
    if ((w->flags & HDR_FLAG_COMPRESSION) != 0 && comp != HDR_FLAG_LZX)
        return WIM_ERR_FORMAT;
    if (w->chunk_size != CHUNK || w->image_count == 0 ||
        w->boot_index == 0 || w->boot_index > w->image_count)
        return WIM_ERR_FORMAT;
    if ((w->lookup.flags & (WIM_RES_COMPRESSED | RES_SOLID)) != 0 ||
        w->lookup.size != w->lookup.orig ||
        w->lookup.orig % WIM_LOOKUP_LEN != 0 || !in_file(w, &w->lookup) ||
        !in_file(w, &w->boot_meta) ||
        (w->boot_meta.flags & WIM_RES_METADATA) == 0)
        return WIM_ERR_CORRUPT;
    return WIM_OK;
}

void wim_lookup_entry(const uint8_t *table, size_t i, wim_reshdr *r,
                      uint32_t *refcnt, uint8_t hash[WIM_HASH_LEN])
{
    const uint8_t *e = table + i * WIM_LOOKUP_LEN;

    reshdr_load(e, r);
    if (refcnt != (uint32_t *)0)
        *refcnt = ld32(e + 26);
    if (hash != (uint8_t *)0)
        memcpy(hash, e + 30, WIM_HASH_LEN);
}

/* ---- resources ---- */

/* The shape of a resource, checked: its chunk count, the width of a chunk
 * table entry and the table's length. For an uncompressed resource n is 0.
 */
typedef struct {
    uint64_t n;
    uint64_t esz;
    uint64_t table_len;
} res_shape;

static int res_check(const wim_info *w, const wim_reshdr *r, res_shape *sh)
{
    if ((r->flags & (RES_SPANNED | RES_SOLID)) != 0 || !in_file(w, r))
        return WIM_ERR_FORMAT;
    sh->n = 0;
    sh->esz = 0;
    sh->table_len = 0;
    if ((r->flags & WIM_RES_COMPRESSED) == 0)
        return r->size == r->orig ? WIM_OK : WIM_ERR_CORRUPT;
    if (r->orig == 0)
        return WIM_ERR_CORRUPT;
    sh->n = (r->orig + CHUNK - 1u) / CHUNK;
    sh->esz = r->orig > 0xffffffffull ? 8u : 4u;
    sh->table_len = (sh->n - 1u) * sh->esz;
    if (sh->table_len > r->size)
        return WIM_ERR_CORRUPT;
    return WIM_OK;
}

/* Chunk i of a compressed resource into out, which holds CHUNK bytes. *got
 * is how many it produced: CHUNK, or less for the last. */
static int read_chunk(wim_read_fn read, void *ctx, const wim_reshdr *r,
                      const res_shape *sh, uint64_t i, uint8_t *out,
                      uint8_t *scratch, uint64_t *got)
{
    uint64_t start = 0, end = r->size - sh->table_len, csize, usize;
    uint8_t e[16];

    if (i > 0) {
        if (read(ctx, r->offset + (i - 1u) * sh->esz,
                 i + 1u < sh->n ? 2u * sh->esz : sh->esz, e) != 0)
            return WIM_ERR_IO;
        start = sh->esz == 8u ? ld64(e) : ld32(e);
        if (i + 1u < sh->n)
            end = sh->esz == 8u ? ld64(e + 8) : ld32(e + 4);
    } else if (sh->n > 1u) {
        if (read(ctx, r->offset, sh->esz, e) != 0)
            return WIM_ERR_IO;
        end = sh->esz == 8u ? ld64(e) : ld32(e);
    }
    usize = i + 1u < sh->n ? CHUNK : r->orig - (sh->n - 1u) * CHUNK;
    if (end < start || end > r->size - sh->table_len)
        return WIM_ERR_CORRUPT;
    csize = end - start;
    if (csize == 0 || csize > usize)
        return WIM_ERR_CORRUPT;
    *got = usize;
    if (csize == usize)
        return read(ctx, r->offset + sh->table_len + start, usize, out) == 0
                   ? WIM_OK : WIM_ERR_IO;
    if (csize > WIM_SCRATCH_LEN)
        return WIM_ERR_CORRUPT;
    if (read(ctx, r->offset + sh->table_len + start, csize, scratch) != 0)
        return WIM_ERR_IO;
    if (lzx_decompress(scratch, (size_t)csize, out, (size_t)usize) != LZX_OK)
        return WIM_ERR_CORRUPT;
    return WIM_OK;
}

int wim_read_resource(const wim_info *w, wim_read_fn read, void *ctx,
                      const wim_reshdr *r, uint8_t *out, size_t cap,
                      uint8_t *scratch)
{
    res_shape sh;
    uint64_t i, got;
    int rc;

    if (w == (const wim_info *)0 || read == (wim_read_fn)0 ||
        r == (const wim_reshdr *)0 || out == (uint8_t *)0 ||
        scratch == (uint8_t *)0)
        return WIM_ERR_BAD_ARG;
    if (r->orig > cap)
        return WIM_ERR_NOSPACE;
    rc = res_check(w, r, &sh);
    if (rc != WIM_OK)
        return rc;
    if (sh.n == 0)
        return r->orig == 0 || read(ctx, r->offset, r->orig, out) == 0
                   ? WIM_OK : WIM_ERR_IO;
    for (i = 0; i < sh.n; i++) {
        rc = read_chunk(read, ctx, r, &sh, i, out + i * CHUNK, scratch, &got);
        if (rc != WIM_OK)
            return rc;
    }
    return WIM_OK;
}

int wim_read_head(const wim_info *w, wim_read_fn read, void *ctx,
                  const wim_reshdr *r, uint8_t *out, size_t *len,
                  uint8_t *scratch)
{
    res_shape sh;
    uint64_t got = 0;
    int rc;

    if (w == (const wim_info *)0 || read == (wim_read_fn)0 ||
        r == (const wim_reshdr *)0 || out == (uint8_t *)0 ||
        len == (size_t *)0 || scratch == (uint8_t *)0)
        return WIM_ERR_BAD_ARG;
    *len = 0;
    rc = res_check(w, r, &sh);
    if (rc != WIM_OK)
        return rc;
    if (sh.n == 0) {
        got = r->orig < CHUNK ? r->orig : CHUNK;
        if (got != 0 && read(ctx, r->offset, got, out) != 0)
            return WIM_ERR_IO;
    } else {
        rc = read_chunk(read, ctx, r, &sh, 0, out, scratch, &got);
        if (rc != WIM_OK)
            return rc;
    }
    *len = (size_t)got;
    return WIM_OK;
}

/* ---- metadata ---- */

typedef struct {
    const uint8_t *m;
    size_t len;
} meta_view;

/* The dentry at off: its fixed part checked to be inside the metadata, and
 * the offset of whatever follows it (its extra stream entries included).
 * *end is 0 for the end-of-directory marker. */
static int dentry_at(const meta_view *v, size_t off, size_t *next,
                     size_t *end_marker)
{
    uint64_t len;
    size_t p, s;
    uint16_t streams;

    *end_marker = 0;
    if (off > v->len || v->len - off < 8u)
        return WIM_ERR_CORRUPT;
    len = ld64(v->m + off);
    if (len == 0) {
        *end_marker = 1;
        *next = off + 8u;
        return WIM_OK;
    }
    if (len < DENTRY_FIXED || len > v->len - off)
        return WIM_ERR_CORRUPT;
    if ((size_t)ld16(v->m + off + 100) + DENTRY_FIXED > len)
        return WIM_ERR_CORRUPT;
    streams = ld16(v->m + off + 96);
    p = (off + (size_t)len + 7u) & ~(size_t)7u;
    for (s = 0; s < streams; s++) {
        uint64_t sl;

        if (p > v->len || v->len - p < 8u)
            return WIM_ERR_CORRUPT;
        sl = ld64(v->m + p);
        if (sl < 38u || sl > v->len - p)
            return WIM_ERR_CORRUPT;
        p = (p + (size_t)sl + 7u) & ~(size_t)7u;
    }
    if (p > v->len)
        return WIM_ERR_CORRUPT;
    *next = p;
    return WIM_OK;
}

/* Does the dentry at off have this ASCII name, case folded? */
static int named(const meta_view *v, size_t off, const char *name)
{
    size_t n = ld16(v->m + off + 100) / 2u, i;
    const uint8_t *u = v->m + off + DENTRY_FIXED;

    for (i = 0; i < n; i++) {
        uint16_t c = ld16(u + 2u * i), a;

        if (name[i] == '\0')
            return 0;
        a = (uint16_t)(unsigned char)name[i];
        if (c >= 'A' && c <= 'Z')
            c = (uint16_t)(c - 'A' + 'a');
        if (a >= 'A' && a <= 'Z')
            a = (uint16_t)(a - 'A' + 'a');
        if (c != a)
            return 0;
    }
    return name[n] == '\0';
}

/* The child of the directory dentry at dir called name. */
static int child(const meta_view *v, size_t dir, const char *name,
                 size_t *out)
{
    uint64_t sub = ld64(v->m + dir + 16);
    size_t off, next, end;
    unsigned guard = 0;

    if ((ld32(v->m + dir + 8) & ATTR_DIRECTORY) == 0 || sub == 0 ||
        sub > v->len)
        return WIM_ERR_NOT_FOUND;
    off = (size_t)sub;
    for (;;) {
        int rc = dentry_at(v, off, &next, &end);

        if (rc != WIM_OK)
            return rc;
        if (end)
            return WIM_ERR_NOT_FOUND;
        if (named(v, off, name)) {
            *out = off;
            return WIM_OK;
        }
        off = next;
        if (++guard > 1000000u)
            return WIM_ERR_CORRUPT;
    }
}

/* Where a file's unnamed data stream hash lives: the dentry's own field, or
 * the extra stream entry with no name when the file has named streams. */
static size_t data_hash_at(const meta_view *v, size_t off)
{
    uint16_t streams = ld16(v->m + off + 96);
    size_t p, s;

    if (streams == 0)
        return off + 64u;
    p = (off + (size_t)ld64(v->m + off) + 7u) & ~(size_t)7u;
    for (s = 0; s < streams; s++) {
        if (ld16(v->m + p + 36) == 0)
            return p + 16u;
        p = (p + (size_t)ld64(v->m + p) + 7u) & ~(size_t)7u;
    }
    return off + 64u;
}

/* \Windows\System32 and its winpeshl.exe in the boot image's metadata: the
 * tree is the security data, then the root dentry at its aligned end. */
static int find_winpeshl(const meta_view *v, size_t *sys32, size_t *pe)
{
    size_t root, win, next, end;
    uint32_t sec;
    int rc;

    if (v->len < 8u)
        return WIM_ERR_CORRUPT;
    sec = ld32(v->m);
    root = (size_t)(sec < 8u ? 8u : sec);
    root = (root + 7u) & ~(size_t)7u;
    rc = dentry_at(v, root, &next, &end);
    if (rc != WIM_OK)
        return rc;
    if (end)
        return WIM_ERR_CORRUPT;
    rc = child(v, root, "Windows", &win);
    if (rc == WIM_OK)
        rc = child(v, win, "System32", sys32);
    if (rc == WIM_OK)
        rc = child(v, *sys32, "winpeshl.exe", pe);
    return rc;
}

int wim_winpeshl(const wim_info *w, const uint8_t *lookup,
                 const uint8_t *meta, size_t meta_len, wim_reshdr *out)
{
    meta_view v;
    size_t sys32, pe, at, n, i;
    uint8_t hash[WIM_HASH_LEN];
    int rc;

    if (w == (const wim_info *)0 || lookup == (const uint8_t *)0 ||
        meta == (const uint8_t *)0 || out == (wim_reshdr *)0)
        return WIM_ERR_BAD_ARG;
    if (w->lookup.orig > (uint64_t)SIZE_MAX)
        return WIM_ERR_NOSPACE;
    v.m = meta;
    v.len = meta_len;
    rc = find_winpeshl(&v, &sys32, &pe);
    if (rc != WIM_OK)
        return rc;
    at = data_hash_at(&v, pe);
    if (at > meta_len || meta_len - at < WIM_HASH_LEN)
        return WIM_ERR_CORRUPT;
    n = (size_t)w->lookup.orig / WIM_LOOKUP_LEN;
    for (i = 0; i < n; i++) {
        wim_lookup_entry(lookup, i, out, (uint32_t *)0, hash);
        if ((out->flags & WIM_RES_METADATA) == 0 &&
            memcmp(hash, meta + at, WIM_HASH_LEN) == 0)
            return WIM_OK;
    }
    return WIM_ERR_NOT_FOUND;
}

static const char g_orig_name[] = "winpeshl_orig.exe";

static uint16_t upcase(uint16_t c)
{
    return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 'a' + 'A') : c;
}

/* Ordinal comparison of two ASCII names, upcased. */
static int name_cmp(const char *a, const char *b)
{
    size_t i;

    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        uint16_t x = upcase((uint16_t)(unsigned char)a[i]);
        uint16_t y = upcase((uint16_t)(unsigned char)b[i]);

        if (x != y)
            return x < y ? -1 : 1;
    }
    return a[i] == b[i] ? 0 : (a[i] == '\0' ? -1 : 1);
}

/* The dentry at off's name against an ASCII one, the same way: negative,
 * zero or positive as the dentry sorts before, with or after it. */
static int dentry_cmp(const meta_view *v, size_t off, const char *name)
{
    size_t n = ld16(v->m + off + 100) / 2u, i;
    const uint8_t *u = v->m + off + DENTRY_FIXED;

    for (i = 0; i < n && name[i] != '\0'; i++) {
        uint16_t x = upcase(ld16(u + 2u * i));
        uint16_t y = upcase((uint16_t)(unsigned char)name[i]);

        if (x != y)
            return x < y ? -1 : 1;
    }
    if (i == n && name[i] == '\0')
        return 0;
    return i == n ? -1 : 1;
}
static const char g_cfg_name[] = "falcon-winjump.cfg";

static size_t align8(size_t v)
{
    return (v + 7u) & ~(size_t)7u;
}

size_t wim_patch_tail_max(size_t meta_len, size_t lookup_len,
                          size_t jump_len, size_t cfg_len)
{
    /* The jump binary and its configuration, the metadata with a second
     * copy of a directory's list (at most the whole metadata again) and two
     * new dentries, and the lookup table with two more entries. */
    return jump_len + cfg_len + 16u + 2u * meta_len + 512u + lookup_len +
           (size_t)2u * WIM_LOOKUP_LEN;
}

/* A new dentry at dst for a file in System32: the fixed part of the
 * template (winpeshl.exe's: its attributes, times and security), the given
 * name and data hash, no short name, no streams, no hard link. Returns its
 * length. */
static size_t put_dentry(uint8_t *dst, const uint8_t *tmpl, const char *name,
                         const uint8_t hash[WIM_HASH_LEN])
{
    size_t nl = 0, i, dlen;

    while (name[nl] != '\0')
        nl++;
    dlen = align8(DENTRY_FIXED + 2u * nl + 2u);
    memset(dst, 0, dlen);
    memcpy(dst, tmpl, DENTRY_FIXED);
    st64(dst, dlen);
    st64(dst + 16, 0);
    memcpy(dst + 64, hash, WIM_HASH_LEN);
    if ((ld32(tmpl + 8) & 0x400u) == 0)   /* not a reparse point */
        st64(dst + 88, 0);
    st16(dst + 96, 0);
    st16(dst + 98, 0);
    st16(dst + 100, (uint16_t)(2u * nl));
    for (i = 0; i < nl; i++)
        st16(dst + DENTRY_FIXED + 2u * i, (uint16_t)name[i]);
    return dlen;
}

static void put_stream_entry(uint8_t *e, uint64_t offset, size_t len,
                             const uint8_t hash[WIM_HASH_LEN])
{
    wim_reshdr r;

    r.size = len;
    r.flags = 0;
    r.offset = offset;
    r.orig = len;
    reshdr_store(e, &r);
    st16(e + 24, 1);
    st32(e + 26, 1);
    memcpy(e + 30, hash, WIM_HASH_LEN);
}

int wim_patch_build(const wim_info *w, const uint8_t *lookup,
                    const uint8_t *meta, size_t meta_len, const void *jump,
                    size_t jump_len, const void *cfg, size_t cfg_len,
                    uint8_t hdr[WIM_HDR_LEN], uint8_t *tail, size_t cap,
                    size_t *tail_len)
{
    meta_view v;
    size_t sys32, pe, off, next, end, list_start, list_end;
    size_t meta_off, meta_new_len, lk_off, entries, i, meta_seen = 0;
    size_t cfg_off, dlen, lk_len;
    uint64_t start;
    uint8_t jump_hash[WIM_HASH_LEN], meta_hash[WIM_HASH_LEN];
    uint8_t cfg_hash[WIM_HASH_LEN];
    uint8_t *nm, *lk;
    wim_reshdr r;
    int rc, replaced = 0;

    if (w == (const wim_info *)0 || lookup == (const uint8_t *)0 ||
        meta == (const uint8_t *)0 || jump == (const void *)0 ||
        jump_len == 0 || cfg == (const void *)0 || cfg_len == 0 ||
        hdr == (uint8_t *)0 || tail == (uint8_t *)0 ||
        tail_len == (size_t *)0)
        return WIM_ERR_BAD_ARG;
    v.m = meta;
    v.len = meta_len;

    /* The lookup table is in the caller's memory, so on a platform where
     * size_t is narrower than the WIM's 64-bit fields a length past it
     * cannot be one the caller holds. */
    if (w->lookup.orig > (uint64_t)SIZE_MAX)
        return WIM_ERR_NOSPACE;
    lk_len = (size_t)w->lookup.orig;

    rc = find_winpeshl(&v, &sys32, &pe);
    if (rc != WIM_OK)
        return rc;

    /* The list to copy: System32's children, up to its end marker. */
    /* Compared before it is narrowed: on an i686 build a value past the
     * metadata would otherwise wrap into it and be walked as a list. */
    start = ld64(meta + sys32 + 16);
    if (start > meta_len)
        return WIM_ERR_CORRUPT;
    list_start = (size_t)start;
    off = list_start;
    for (;;) {
        rc = dentry_at(&v, off, &next, &end);
        if (rc != WIM_OK)
            return rc;
        if (end)
            break;
        off = next;
    }
    list_end = off;     /* the marker's offset */

    dlen = align8(DENTRY_FIXED + 2u * (sizeof g_orig_name - 1u) + 2u) +
           align8(DENTRY_FIXED + 2u * (sizeof g_cfg_name - 1u) + 2u);
    meta_new_len = align8(meta_len) + (list_end - list_start) + dlen + 8u;
    cfg_off = align8(jump_len);
    meta_off = cfg_off + align8(cfg_len);
    if ((uint64_t)cap < (uint64_t)meta_off + meta_new_len + lk_len +
                            (uint64_t)2u * WIM_LOOKUP_LEN)
        return WIM_ERR_NOSPACE;

    /* [jump][cfg][metadata][lookup table], after the original's last
     * byte. */
    memcpy(tail, jump, jump_len);
    memset(tail + jump_len, 0, cfg_off - jump_len);
    memcpy(tail + cfg_off, cfg, cfg_len);
    memset(tail + cfg_off + cfg_len, 0, meta_off - cfg_off - cfg_len);
    sha1(jump, jump_len, jump_hash);
    sha1(cfg, cfg_len, cfg_hash);
    nm = tail + meta_off;
    memcpy(nm, meta, meta_len);
    memset(nm + meta_len, 0, align8(meta_len) - meta_len);
    off = align8(meta_len);

    /* The copied list, with winpeshl_orig.exe (the original data) and the
     * jump binary's configuration inserted where they sort. Windows keeps a
     * directory's entries in ordinal order of their upcased names and looks
     * them up that way; an entry out of place is one it cannot find. On the
     * way, winpeshl.exe's entry in the copy points at the jump binary. */
    {
        const char *add_name[2];
        const uint8_t *add_hash[2];
        size_t add_n = 0, src = list_start;

        add_name[0] = g_cfg_name;
        add_hash[0] = cfg_hash;
        add_name[1] = g_orig_name;
        add_hash[1] = meta + data_hash_at(&v, pe);
        if (name_cmp(g_orig_name, g_cfg_name) < 0) {
            add_name[0] = g_orig_name;
            add_hash[0] = meta + data_hash_at(&v, pe);
            add_name[1] = g_cfg_name;
            add_hash[1] = cfg_hash;
        }
        while (src < list_end) {
            rc = dentry_at(&v, src, &next, &end);
            if (rc != WIM_OK)
                return rc;
            while (add_n < 2u && dentry_cmp(&v, src, add_name[add_n]) > 0) {
                off += put_dentry(nm + off, meta + pe, add_name[add_n],
                                  add_hash[add_n]);
                add_n++;
            }
            memcpy(nm + off, meta + src, next - src);
            if (src == pe)
                memcpy(nm + off + (data_hash_at(&v, pe) - pe), jump_hash,
                       WIM_HASH_LEN);
            off += next - src;
            src = next;
        }
        while (add_n < 2u) {
            off += put_dentry(nm + off, meta + pe, add_name[add_n],
                              add_hash[add_n]);
            add_n++;
        }
    }
    memset(nm + off, 0, 8);

    /* System32 points at the new list. */
    st64(nm + sys32 + 16, align8(meta_len));
    sha1(nm, meta_new_len, meta_hash);

    /* The lookup table: the boot image's metadata entry replaced, the
     * jump binary's added. Metadata entries are in image order. */
    lk_off = meta_off + meta_new_len;
    lk = tail + lk_off;
    memcpy(lk, lookup, lk_len);
    entries = lk_len / WIM_LOOKUP_LEN;
    for (i = 0; i < entries; i++) {
        wim_lookup_entry(lookup, i, &r, (uint32_t *)0, (uint8_t *)0);
        if ((r.flags & WIM_RES_METADATA) == 0)
            continue;
        if (++meta_seen == w->boot_index) {
            r.size = meta_new_len;
            r.flags = WIM_RES_METADATA;
            r.offset = w->file_size + meta_off;
            r.orig = meta_new_len;
            reshdr_store(lk + i * WIM_LOOKUP_LEN, &r);
            memcpy(lk + i * WIM_LOOKUP_LEN + 30, meta_hash, WIM_HASH_LEN);
            replaced = 1;
            break;
        }
    }
    if (!replaced)
        return WIM_ERR_CORRUPT;
    put_stream_entry(lk + entries * WIM_LOOKUP_LEN, w->file_size, jump_len,
                     jump_hash);
    put_stream_entry(lk + (size_t)(entries + 1u) * WIM_LOOKUP_LEN,
                     w->file_size + cfg_off, cfg_len, cfg_hash);
    *tail_len = lk_off + lk_len + (size_t)2u * WIM_LOOKUP_LEN;

    /* The header: the new lookup table and boot metadata; no integrity
     * table, since the tail is not covered by it. */
    memcpy(hdr, w->hdr, WIM_HDR_LEN);
    r.size = w->lookup.orig + (uint64_t)2u * WIM_LOOKUP_LEN;
    r.flags = 0;
    r.offset = w->file_size + lk_off;
    r.orig = r.size;
    reshdr_store(hdr + 48, &r);
    r.size = meta_new_len;
    r.flags = WIM_RES_METADATA;
    r.offset = w->file_size + meta_off;
    r.orig = meta_new_len;
    reshdr_store(hdr + 96, &r);
    memset(hdr + 124, 0, WIM_RESHDR_LEN);
    return WIM_OK;
}
