/* The two byte edits that make a BSD ISO boot itself. See bsdmd.h. */

#include <media/bsdmd.h>

#include <lib/string.h>   /* memcpy, memset and get_le32 */

#define ISO_SECTOR      2048u
#define PVD_LBA         16u
#define PVD_MAX         32u     /* descriptors past this are not a volume set */
#define PVD_ROOT_REC    156u    /* the root directory record inside the PVD */
#define REC_EXTENT      2u
#define REC_SIZE        10u
#define REC_FLAGS       25u
#define REC_NAME_LEN    32u
#define REC_NAME        33u
#define DIR_MAX_SECTORS 64u     /* a directory extent longer than 128 KiB is
                                 * not one we need to walk to its end */

/* ISO9660 stores its numbers twice, little-endian then big-endian, and a
 * reader is free to take either. Both are written here because a medium with
 * only one is malformed and some readers check. */
static void put_both32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

static void put_both16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static size_t cstr_len(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

/* The primary volume descriptor's root directory record. */
static int root_dir(bsdmd_read_fn read, void *ctx, uint32_t *lba,
                    uint32_t *size)
{
    uint8_t sec[ISO_SECTOR];
    uint32_t at;

    for (at = PVD_LBA; at < PVD_MAX; at++) {
        if (read(ctx, (uint64_t)at * ISO_SECTOR, ISO_SECTOR, sec) != 0)
            return BSDMD_ERR_IO;
        if (sec[1] != 'C' || sec[2] != 'D' || sec[3] != '0' ||
            sec[4] != '0' || sec[5] != '1')
            break;                              /* the descriptors ended */
        if (sec[0] == 1) {
            *lba = get_le32(sec + PVD_ROOT_REC + REC_EXTENT);
            *size = get_le32(sec + PVD_ROOT_REC + REC_SIZE);
            return (*size != 0) ? BSDMD_OK : BSDMD_ERR_NOT_ISO;
        }
        if (sec[0] == 255)                      /* the set terminator */
            break;
    }
    return BSDMD_ERR_NOT_ISO;
}

/* The named child of a directory extent, by its ISO name (upper case, and
 * with any version suffix ignored). Only directories are matched, because
 * the one caller is looking for /boot. */
static int child_dir(bsdmd_read_fn read, void *ctx, uint32_t lba,
                     uint32_t size, const char *name, uint32_t *out_lba,
                     uint32_t *out_size)
{
    uint8_t sec[ISO_SECTOR];
    size_t want = cstr_len(name);
    uint32_t s;

    for (s = 0; s * ISO_SECTOR < size && s < DIR_MAX_SECTORS; s++) {
        uint32_t o = 0;

        if (read(ctx, ((uint64_t)lba + s) * ISO_SECTOR, ISO_SECTOR, sec) != 0)
            return BSDMD_ERR_IO;
        while (o + REC_NAME < ISO_SECTOR && sec[o] != 0) {
            uint32_t rl = sec[o];
            uint32_t nl = sec[o + REC_NAME_LEN];

            if (rl < REC_NAME + nl || o + rl > ISO_SECTOR)
                break;                          /* malformed; stop here */
            if ((sec[o + REC_FLAGS] & 0x02u) != 0 && nl == want &&
                memcmp(sec + o + REC_NAME, name, want) == 0) {
                *out_lba = get_le32(sec + o + REC_EXTENT);
                *out_size = get_le32(sec + o + REC_SIZE);
                return BSDMD_OK;
            }
            o += rl;
        }
    }
    return BSDMD_ERR_NOT_ISO;
}

/* Where a new record can be appended in this directory: the byte just past
 * the last record, provided its sector has room for rec_len more bytes and a
 * zero terminator after them, and provided that still falls inside the
 * directory's declared length. */
static int append_point(bsdmd_read_fn read, void *ctx, uint32_t lba,
                        uint32_t size, uint32_t rec_len, uint64_t *off)
{
    uint8_t sec[ISO_SECTOR];
    uint32_t s;
    uint32_t best_sec = 0;
    uint32_t best_end = 0;
    int found = 0;

    for (s = 0; s * ISO_SECTOR < size && s < DIR_MAX_SECTORS; s++) {
        uint32_t o = 0;

        if (read(ctx, ((uint64_t)lba + s) * ISO_SECTOR, ISO_SECTOR, sec) != 0)
            return BSDMD_ERR_IO;
        while (o + REC_NAME < ISO_SECTOR && sec[o] != 0) {
            uint32_t rl = sec[o];

            if (rl < REC_NAME || o + rl > ISO_SECTOR)
                break;
            o += rl;
            /* The last record wins, and the last sector holding any record
             * wins over an earlier one: a directory is written in order. */
            best_sec = s;
            best_end = o;
            found = 1;
        }
    }
    if (!found)
        return BSDMD_ERR_NOT_ISO;
    if (best_end + rec_len + 1u > ISO_SECTOR)
        return BSDMD_ERR_NO_ROOM;
    if (best_sec * ISO_SECTOR + best_end + rec_len > size)
        return BSDMD_ERR_NO_ROOM;
    *off = ((uint64_t)lba + best_sec) * ISO_SECTOR + best_end;
    return BSDMD_OK;
}

static void build_record(uint32_t iso_size, uint8_t *rec, uint32_t rec_len)
{
    size_t n = cstr_len(BSDMD_ISO_NAME);

    memset(rec, 0, rec_len);
    rec[0] = (uint8_t)rec_len;
    rec[1] = 0;                                 /* no extended attributes */
    /* The file IS the medium: it starts at the first sector and runs the
     * whole length. Nothing is stored twice. */
    put_both32(rec + REC_EXTENT, 0);
    put_both32(rec + REC_SIZE, iso_size);
    rec[18] = 126;                              /* 2026 */
    rec[19] = 1;
    rec[20] = 1;
    rec[25] = 0;                                /* a plain file */
    put_both16(rec + 28, 1);                    /* volume sequence number */
    rec[REC_NAME_LEN] = (uint8_t)n;
    memcpy(rec + REC_NAME, BSDMD_ISO_NAME, n);
}

int bsdmd_record(uint64_t iso_size, bsdmd_read_fn read, void *ctx,
                 bsdmd_record_edit *out, char *path, size_t path_cap)
{
    static const char *const dirs[] = { "", "BOOT" };
    uint32_t rec_len = (uint32_t)(REC_NAME + cstr_len(BSDMD_ISO_NAME));
    uint32_t root_lba = 0;
    uint32_t root_size = 0;
    size_t i;
    int rc;

    if (read == (bsdmd_read_fn)0 || out == (bsdmd_record_edit *)0 ||
        path == (char *)0 || path_cap < BSDMD_PATH_MAX || iso_size == 0)
        return BSDMD_ERR_ARG;
    /* An ISO9660 data length is 32 bits, so 4 GiB is the format's ceiling
     * and not a limit chosen here. */
    if (iso_size > 0xFFFFFFFFu)
        return BSDMD_ERR_TOO_BIG;
    if (rec_len % 2u != 0)
        rec_len++;

    rc = root_dir(read, ctx, &root_lba, &root_size);
    if (rc != BSDMD_OK)
        return rc;

    rc = BSDMD_ERR_NO_ROOM;
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        uint32_t lba = root_lba;
        uint32_t size = root_size;
        uint64_t off = 0;
        size_t at = 0;

        if (dirs[i][0] != '\0' &&
            child_dir(read, ctx, root_lba, root_size, dirs[i], &lba, &size) !=
                BSDMD_OK)
            continue;
        rc = append_point(read, ctx, lba, size, rec_len, &off);
        if (rc == BSDMD_ERR_IO)
            return rc;
        if (rc != BSDMD_OK)
            continue;

        out->off = off;
        out->len = rec_len;
        build_record((uint32_t)iso_size, out->bytes, rec_len);

        /* The path the loader is told to load. The directory component is
         * lower case because that is the Rock Ridge name every one of these
         * media carries; the file's own component is the ISO name, because
         * the record this adds has no Rock Ridge name. */
        path[at++] = '/';
        if (dirs[i][0] != '\0') {
            path[at++] = 'b';
            path[at++] = 'o';
            path[at++] = 'o';
            path[at++] = 't';
            path[at++] = '/';
        }
        memcpy(path + at, BSDMD_ISO_NAME, cstr_len(BSDMD_ISO_NAME) + 1u);
        return BSDMD_OK;
    }
    return rc;
}

/* ---- loader.conf ---- */

typedef struct {
    char  *p;
    size_t cap;
    size_t n;
    int    over;
} wbuf;

static void put(wbuf *w, const char *s, size_t len)
{
    if (w->over || len > w->cap - w->n) {
        w->over = 1;
        return;
    }
    memcpy(w->p + w->n, s, len);
    w->n += len;
}

static void puts_(wbuf *w, const char *s)
{
    put(w, s, cstr_len(s));
}

/* The length of the file's opening run of comment and blank lines, which is
 * what may be overwritten. Stops at the first line that is neither, so a
 * setting is never clobbered even if a future default file starts with
 * one. */
static size_t comment_block(const char *head, size_t len)
{
    size_t at = 0;

    for (;;) {
        size_t eol = at;

        while (eol < len && head[eol] != '\n')
            eol++;
        if (eol == len)
            return at;                          /* no newline: stop short */
        if (eol != at && head[at] != '#')
            return at;
        at = eol + 1u;
    }
}

int bsdmd_config(const char *path, const char *head, size_t head_len,
                 char *out, size_t cap, uint32_t *len_out)
{
    size_t size;
    wbuf w;

    if (path == (const char *)0 || head == (const char *)0 ||
        out == (char *)0 || len_out == (uint32_t *)0)
        return BSDMD_ERR_ARG;
    size = comment_block(head, head_len);
    if (size == 0 || size > cap || size > BSDMD_CFG_MAX)
        return BSDMD_ERR_NO_ROOM;

    w.p = out;
    w.cap = size;
    w.n = 0;
    w.over = 0;
    puts_(&w, "md_image_load=\"YES\"\nmd_image_type=\"md_image\"\n"
              "md_image_name=\"");
    puts_(&w, path);
    puts_(&w, "\"\nvfs.root.mountfrom=\"cd9660:/dev/md0\"\n");

    /* The rest of the block's length: a comment the parser skips, ending in
     * a newline, so the file is exactly as long as it was. */
    if (w.over || w.n + 2u > size)
        return BSDMD_ERR_NO_ROOM;
    out[w.n++] = '#';
    while (w.n < size - 1u)
        out[w.n++] = ' ';
    out[w.n++] = '\n';
    *len_out = (uint32_t)size;
    return BSDMD_OK;
}
