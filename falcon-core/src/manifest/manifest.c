/* Manifest reader (docs/DESIGN.md Section 8.1). See manifest.h for the
 * format and for why this parser is total over its input.
 *
 * Shape: a single forward pass over the caller's buffer with indices, no
 * line buffer and no allocator, so a pathological line (a megabyte with no
 * newline, a value far longer than any field) costs nothing beyond the scan
 * and cannot overrun anything. Values are copied into the record only after
 * they are known to fit, and a value that does not fit invalidates the
 * record rather than truncating it: a truncated path is a path that matches
 * the wrong file, which is the one failure mode worth refusing outright.
 */

#include <manifest/manifest.h>
#include <lib/string.h>

#define NUL ((void *)0)

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

static int is_sep(char c)
{
    return c == '/' || c == '\\';
}

static char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* Printable ASCII only. This is the rule that lets the loader compare
 * manifest paths against exFAT names as plain bytes (manifest.h). */
static int is_printable(char c)
{
    unsigned char u = (unsigned char)c;

    return u >= 0x20u && u <= 0x7Eu;
}

/* ------------------------------------------------------------ paths */

/* Next character of the normalised path, '\0' at the end. A separator run
 * collapses to one '/', and a run that reaches the end of the string yields
 * nothing, which is how a trailing separator gets ignored. Leading runs are
 * the caller's job, since there is no preceding character to collapse into. */
static char path_next(const char *s, size_t *i)
{
    char c = s[*i];

    if (c == '\0')
        return '\0';
    if (is_sep(c)) {
        while (is_sep(s[*i]))
            (*i)++;
        if (s[*i] == '\0')
            return '\0';
        return '/';
    }
    (*i)++;
    return to_lower(c);
}

int manifest_path_eq(const char *a, const char *b)
{
    size_t i = 0;
    size_t j = 0;

    if (a == NUL || b == NUL)
        return 0;
    while (is_sep(a[i]))
        i++;
    while (is_sep(b[j]))
        j++;
    for (;;) {
        char ca = path_next(a, &i);
        char cb = path_next(b, &j);

        if (ca != cb)
            return 0;
        if (ca == '\0')
            return 1;
    }
}

int manifest_path_from_ucs2(const uint16_t *src, char *dst, size_t cap)
{
    size_t i = 0;

    if (src == NUL || dst == NUL || cap == 0)
        return MANIFEST_ERR_BAD_ARG;
    while (src[i] != 0) {
        char c;

        if (i + 1u >= cap)
            return MANIFEST_ERR_BAD_ARG;
        if (src[i] > 0x7Eu || src[i] < 0x20u)
            return MANIFEST_ERR_BAD_ARG;
        c = (char)src[i];
        dst[i] = c;
        i++;
    }
    dst[i] = '\0';
    return MANIFEST_OK;
}

/* ------------------------------------------------------------ lookup */

const manifest_record *manifest_find(const manifest_record *recs, size_t n,
                                     const char *iso_path)
{
    size_t i;

    if (recs == NUL || iso_path == NUL)
        return NUL;
    for (i = 0; i < n; i++) {
        if (manifest_path_eq(recs[i].iso, iso_path))
            return &recs[i];
    }
    return NUL;
}

manifest_bless manifest_lookup(const manifest_record *recs, size_t n,
                               const char *iso_path, uint64_t cur_size,
                               const manifest_record **out)
{
    const manifest_record *r = manifest_find(recs, n, iso_path);

    if (out != NUL)
        *out = r;
    if (r == NUL)
        return MANIFEST_BLESS_NONE;
    return (r->size == cur_size) ? MANIFEST_BLESS_FRESH : MANIFEST_BLESS_STALE;
}

/* ------------------------------------------------------------ parsing */

/* Accumulator for the block being read. "bad" is sticky: once a block has
 * shown one unusable field it is dropped whole, because a record is only
 * meaningful as a set (a path without its size cannot be matched, a size
 * without its uki names nothing to boot). "retired" is sticky the same way,
 * for a block of the prepared kind an older tool wrote (manifest.h). */
typedef struct {
    manifest_record rec;
    int             active;   /* an iso= has been seen */
    int             bad;
    int             retired;  /* kind=prepared, or a splice= of one */
    int             has_size;
    int             has_uki;
} block;

/* Case-insensitive match of a length-counted token against a lowercase
 * literal. Used for key names and for the kind= values, which are the only
 * enumerated value in the format; everything else is a name or a number and
 * is compared, or not compared, as written. */
static int tok_is(const char *k, size_t n, const char *want)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (want[i] == '\0' || to_lower(k[i]) != want[i])
            return 0;
    }
    return want[n] == '\0';
}

/* Copies a value into a fixed field. Zero when it does not fit, which the
 * caller turns into a dropped record. */
static int field_set(char *dst, size_t cap, const char *src, size_t n)
{
    size_t i;

    if (n + 1u > cap)
        return 0;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
    dst[n] = '\0';
    return 1;
}

static int value_printable(const char *v, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (!is_printable(v[i]))
            return 0;
    }
    return 1;
}

static int parse_u64(const char *s, size_t n, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;

    /* 20 digits is already more than MANIFEST_SIZE_MAX needs; the bound is
     * here so a run of a million digits is rejected by inspection rather
     * than by arithmetic. */
    if (n == 0 || n > 20u)
        return 0;
    for (i = 0; i < n; i++) {
        uint64_t d;

        if (s[i] < '0' || s[i] > '9')
            return 0;
        d = (uint64_t)(s[i] - '0');
        if (v > (MANIFEST_SIZE_MAX - d) / 10u)
            return 0;
        v = v * 10u + d;
    }
    *out = v;
    return 1;
}

/* Lowercases a 64-character hex digest in place. Zero when the value is not
 * one, which drops the field alone and never the record (manifest.h). */
static int sha256_set(char *dst, const char *src, size_t n)
{
    size_t i;

    if (n != MANIFEST_SHA256_HEX)
        return 0;
    for (i = 0; i < n; i++) {
        char c = to_lower(src[i]);

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
        dst[i] = c;
    }
    dst[n] = '\0';
    return 1;
}

static void block_reset(block *b)
{
    memset(b, 0, sizeof(*b));
}

/* The validity rule (manifest.h), in the one place that decides whether a
 * block becomes a record. */
static int block_valid(const block *b)
{
    if (b->bad || b->retired || !b->has_size || b->rec.iso[0] == '\0')
        return 0;
    return b->has_uki;
}

/* Emits the accumulated block if it is usable. Counts every valid record,
 * writes only the first cap of them: *count is the honest total so a caller
 * that got NOSPACE knows what to resize to. */
static void block_flush(block *b, manifest_record *out, size_t cap,
                        size_t *count)
{
    if (!b->active)
        return;
    if (block_valid(b)) {
        if (*count < cap)
            out[*count] = b->rec;
        (*count)++;
    }
    block_reset(b);
}

int manifest_parse(const void *buf, size_t len,
                   manifest_record *out, size_t cap, size_t *count)
{
    const char *p = buf;
    block b;
    size_t i = 0;

    if (count == NUL)
        return MANIFEST_ERR_BAD_ARG;
    *count = 0;
    if (len != 0 && p == NUL)
        return MANIFEST_ERR_BAD_ARG;
    if (cap != 0 && out == NUL)
        return MANIFEST_ERR_BAD_ARG;

    block_reset(&b);
    while (i < len) {
        size_t eol = i;
        size_t s;
        size_t e;
        size_t k;
        size_t eq;
        size_t klen;
        const char *key;
        const char *val;
        size_t vlen;

        while (eol < len && p[eol] != '\n')
            eol++;

        /* Line body is [s, e). Strip the CR of a CRLF, then anything from
         * the first '#', then surrounding whitespace. */
        s = i;
        e = eol;
        if (e > s && p[e - 1] == '\r')
            e--;
        for (k = s; k < e; k++) {
            if (p[k] == '#') {
                e = k;
                break;
            }
        }
        while (s < e && is_space(p[s]))
            s++;
        while (e > s && is_space(p[e - 1]))
            e--;

        i = (eol < len) ? eol + 1u : len;
        if (s == e)
            continue;

        /* No '=' at all: junk, or a comment-only line already emptied
         * above. Ignored rather than treated as a record boundary. */
        for (eq = s; eq < e && p[eq] != '='; eq++)
            ;
        if (eq == e)
            continue;

        key = p + s;
        klen = eq - s;
        while (klen > 0 && is_space(key[klen - 1]))
            klen--;

        val = p + eq + 1u;
        vlen = e - (eq + 1u);
        while (vlen > 0 && is_space(val[0])) {
            val++;
            vlen--;
        }

        if (tok_is(key, klen, "iso")) {
            /* A new iso= closes the previous block, which is what makes the
             * format streamable and a missing key a local failure. */
            block_flush(&b, out, cap, count);
            b.active = 1;
            if (vlen == 0 || !value_printable(val, vlen) ||
                !field_set(b.rec.iso, sizeof(b.rec.iso), val, vlen))
                b.bad = 1;
            continue;
        }

        if (!b.active)
            continue;   /* keys before the first iso= belong to no block */

        if (tok_is(key, klen, "size")) {
            if (!parse_u64(val, vlen, &b.rec.size))
                b.bad = 1;
            else
                b.has_size = 1;
        } else if (tok_is(key, klen, "uki")) {
            if (vlen == 0 || !value_printable(val, vlen) ||
                !field_set(b.rec.uki, sizeof(b.rec.uki), val, vlen))
                b.bad = 1;
            else
                b.has_uki = 1;
        } else if (tok_is(key, klen, "family")) {
            if (!value_printable(val, vlen) ||
                !field_set(b.rec.family, sizeof(b.rec.family), val, vlen))
                b.bad = 1;
        } else if (tok_is(key, klen, "sha256")) {
            b.rec.has_sha256 = sha256_set(b.rec.sha256, val, vlen) ? 1u : 0u;
            if (!b.rec.has_sha256)
                b.rec.sha256[0] = '\0';
        } else if (tok_is(key, klen, "kind")) {
            /* Only an older tool wrote this key, and only to say "prepared"
             * (manifest.h). That record is retired: skipped whole, and the
             * rest of the file parses as if it were not there. "blessed" is
             * what an absent key means anyway. A kind nobody knows is a
             * record nobody should act on, so it drops the record rather
             * than falling back to blessed. */
            if (tok_is(val, vlen, "prepared"))
                b.retired = 1;
            else if (!tok_is(val, vlen, "blessed"))
                b.bad = 1;
        } else if (tok_is(key, klen, "splice")) {
            /* The byte map of a retired prepared record. Never part of a
             * blessed one: there is nothing to serve it over. */
            b.retired = 1;
        }
        /* Any other key: skipped, per the forward-compatibility rule. */
    }
    block_flush(&b, out, cap, count);

    return (*count > cap) ? MANIFEST_ERR_NOSPACE : MANIFEST_OK;
}
