/* newc ("070701") cpio writer. Format reference: the SVR4 cpio format as
 * Linux implements it in init/initramfs.c and as GNU cpio documents under
 * "New ASCII Format".
 *
 * A record is a 110-byte header of ASCII hex fields, then the NUL-terminated
 * name padded so that header+name is a multiple of 4, then the file data
 * padded to a multiple of 4. The archive ends with a zero-length record named
 * TRAILER!!!, which is what tells the kernel's unpacker that this segment is
 * over and the next one may begin.
 *
 * Deliberately not emitted: the 512-byte block padding GNU cpio appends to
 * its output. The kernel does not need it, and a smaller segment is a smaller
 * copy at boot.
 */

#include <media/cpio.h>
#include <lib/string.h>

#define CPIO_MAGIC     "070701"
#define CPIO_HDR_LEN   110u
#define CPIO_TRAILER   "TRAILER!!!"
#define CPIO_ALIGN     4u

#define MODE_TYPE_MASK 0170000u
#define MODE_TYPE_DIR  0040000u

#define FIELD_MAX      0xFFFFFFFFu  /* 8 ASCII hex digits */

/* A sink that either fills a buffer or only counts. Counting and writing run
 * the identical record-building code, so the size cpio_size promises cannot
 * drift from the bytes cpio_write produces. */
typedef struct {
    uint8_t *buf;      /* NULL while sizing */
    size_t   cap;
    size_t   pos;
} cpio_out;

static void emit(cpio_out *o, const void *src, size_t n)
{
    if (o->buf != (void *)0 && n != 0) {
        /* The caller pre-checked capacity, so a short buffer here would be a
         * bug in this file rather than a caller error; clamp anyway so it
         * cannot become an overrun. */
        size_t room = (o->pos < o->cap) ? o->cap - o->pos : 0;
        size_t take = (n < room) ? n : room;

        memcpy(o->buf + o->pos, src, take);
    }
    o->pos += n;
}

static void emit_zeros(cpio_out *o, size_t n)
{
    if (o->buf != (void *)0 && n != 0) {
        size_t room = (o->pos < o->cap) ? o->cap - o->pos : 0;
        size_t take = (n < room) ? n : room;

        memset(o->buf + o->pos, 0, take);
    }
    o->pos += n;
}

/* Uppercase because that is what GNU cpio emits, which keeps our output
 * byte-comparable against the reference implementation; the kernel's parser
 * is case-insensitive either way. */
static void emit_hex8(cpio_out *o, uint32_t v)
{
    char d[8];
    size_t i = sizeof(d);

    while (i-- > 0) {
        uint32_t nib = v & 0xFu;

        d[i] = (char)(nib < 10u ? '0' + nib : 'A' + (nib - 10u));
        v >>= 4;
    }
    emit(o, d, sizeof(d));
}

static size_t pad_to(size_t n)
{
    return (CPIO_ALIGN - (n % CPIO_ALIGN)) % CPIO_ALIGN;
}

static size_t cstr_len(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

/* One record. name_len excludes the terminating NUL, which the header's
 * namesize field includes. */
static void emit_record(cpio_out *o, uint32_t ino, uint32_t mode,
                        uint32_t nlink, const char *name, size_t name_len,
                        const void *data, size_t len)
{
    size_t namesize = name_len + 1;
    size_t start = o->pos;

    emit(o, CPIO_MAGIC, 6);
    emit_hex8(o, ino);
    emit_hex8(o, mode);
    emit_hex8(o, 0);            /* uid: root */
    emit_hex8(o, 0);            /* gid: root */
    emit_hex8(o, nlink);
    emit_hex8(o, 0);            /* mtime: fixed, so output is reproducible */
    emit_hex8(o, (uint32_t)len);
    emit_hex8(o, 0);            /* devmajor */
    emit_hex8(o, 0);            /* devminor */
    emit_hex8(o, 0);            /* rdevmajor */
    emit_hex8(o, 0);            /* rdevminor */
    emit_hex8(o, (uint32_t)namesize);
    emit_hex8(o, 0);            /* check: unused in newc, always zero */

    emit(o, name, name_len);
    emit_zeros(o, 1);
    emit_zeros(o, pad_to(o->pos - start));

    if (len != 0)
        emit(o, data, len);
    emit_zeros(o, pad_to(len));
}

/* Every '/'-separated component must be a name the archive can contain.
 *
 * An empty component is rejected because it names nothing: a leading '/' makes
 * the first one empty, a trailing '/' the last one, and "a//b" one in the
 * middle, which is what a path built by concatenating a directory that already
 * ended in a slash looks like. A ".." component is rejected because the
 * kernel's unpacker resolves it, so the record would land outside the
 * initramfs root this archive describes.
 *
 * A "." component stays legal on purpose. It resolves to the directory it
 * already sits in, so it cannot leave the root, and rejecting it would be
 * style policing in a check whose job is containment. Names that merely start
 * with dots ("..foo", ".hidden") and names with dots inside them ("a..b") are
 * ordinary names and are not touched by either rule. */
static int path_ok(const char *p, size_t n)
{
    size_t i = 0;

    for (;;) {
        size_t start = i;

        while (i < n && p[i] != '/')
            i++;
        if (i == start)
            return 0;
        if (i - start == 2 && p[start] == '.' && p[start + 1] == '.')
            return 0;
        if (i == n)
            return 1;
        i++; /* the separator; a trailing one leaves the next pass empty */
    }
}

static int check_entry(const cpio_entry *e, size_t *name_len)
{
    size_t nl;

    if (e->path == (void *)0)
        return CPIO_ERR_BAD_ARG;
    nl = cstr_len(e->path);
    if (nl == 0 || !path_ok(e->path, nl))
        return CPIO_ERR_BAD_ARG; /* archive-relative names only */
    if (e->len != 0 && e->data == (void *)0)
        return CPIO_ERR_BAD_ARG;
    if ((e->mode & MODE_TYPE_MASK) == MODE_TYPE_DIR && e->len != 0)
        return CPIO_ERR_BAD_ARG;
    if ((uint64_t)nl + 1u > FIELD_MAX || (uint64_t)e->len > FIELD_MAX)
        return CPIO_ERR_RANGE;

    *name_len = nl;
    return CPIO_OK;
}

static int build(const cpio_entry *entries, size_t count, cpio_out *o)
{
    size_t i;

    for (i = 0; i < count; i++) {
        const cpio_entry *e = &entries[i];
        size_t name_len = 0;
        uint32_t mode = e->mode != 0 ? e->mode : CPIO_MODE_FILE;
        int is_dir = (mode & MODE_TYPE_MASK) == MODE_TYPE_DIR;
        int rc = check_entry(e, &name_len);

        if (rc != CPIO_OK)
            return rc;

        /* Inodes count from 1 and nlink stays 1 for files: the kernel's
         * unpacker treats a regular file with nlink >= 2 as one end of a
         * hard link and defers its contents, which is not what any of our
         * entries mean. Directories conventionally carry 2. */
        emit_record(o, (uint32_t)(i + 1), mode, is_dir ? 2u : 1u,
                    e->path, name_len, e->data, e->len);
    }

    emit_record(o, 0, 0, 1, CPIO_TRAILER, sizeof(CPIO_TRAILER) - 1,
                (void *)0, 0);
    return CPIO_OK;
}

int cpio_size(const cpio_entry *entries, size_t count, size_t *out_size)
{
    cpio_out o = { (void *)0, 0, 0 };
    int rc;

    if (out_size == (void *)0 || (entries == (void *)0 && count != 0))
        return CPIO_ERR_BAD_ARG;

    rc = build(entries, count, &o);
    if (rc != CPIO_OK)
        return rc;
    *out_size = o.pos;
    return CPIO_OK;
}

int cpio_write(const cpio_entry *entries, size_t count, void *buf,
               size_t cap, size_t *out_size)
{
    cpio_out o = { (void *)0, 0, 0 };
    size_t need = 0;
    int rc;

    if (buf == (void *)0 || out_size == (void *)0 ||
        (entries == (void *)0 && count != 0))
        return CPIO_ERR_BAD_ARG;

    rc = cpio_size(entries, count, &need);
    if (rc != CPIO_OK)
        return rc;
    if (need > cap) {
        *out_size = need;
        return CPIO_ERR_NOSPACE;
    }

    o.buf = buf;
    o.cap = cap;
    rc = build(entries, count, &o);
    if (rc != CPIO_OK)
        return rc;
    *out_size = o.pos;
    return CPIO_OK;
}
