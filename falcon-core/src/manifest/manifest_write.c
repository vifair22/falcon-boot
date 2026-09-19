/* Manifest writer (docs/DESIGN.md Section 8.1): record editing and text
 * rendering for the bless tool of Section 8.
 *
 * Separate from manifest.c because the loader has no business carrying it:
 * the loader reads the manifest and nothing in EFI-land ever writes one
 * (DESIGN Section 7.5 keeps every write operation in Linux userland). It is
 * still written to the same rules as the reader, freestanding-clean and
 * allocator-free, so the split is about footprint and not about capability.
 *
 * Round-trip is the contract: render() followed by parse() must yield the
 * records that went in. That is what makes re-blessing safe, since a
 * re-bless is parse, upsert, render, replace-file, and any field the round
 * trip mangled would silently point the loader at the wrong artifact.
 * falcon-core/tests/manifest_test.c pins it.
 */

#include <manifest/manifest.h>
#include <lib/string.h>

#define NUL ((void *)0)

/* Deliberately duplicated from manifest.c rather than exported from it:
 * these are four-line character predicates, and a shared internal header
 * between two files this size would cost more than it saves. */
static int is_printable(char c)
{
    unsigned char u = (unsigned char)c;

    return u >= 0x20u && u <= 0x7Eu;
}

static size_t cstr_len(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

/* A field is representable when it is NUL-terminated inside its array and
 * every byte is printable ASCII. '#' and the newline are excluded on top of
 * that: the first would turn the rest of the line into a comment, the second
 * would split one record into two. Neither can appear in a filename that
 * arrived through the tool's own checks, but the writer is the last place
 * either could be caught before the file is on the stick. */
static int field_ok(const char *s, size_t cap, int allow_empty)
{
    size_t n;
    size_t i;

    n = 0;
    while (n < cap && s[n] != '\0')
        n++;
    if (n == cap)
        return 0;              /* not terminated inside the array */
    if (n == 0)
        return allow_empty;
    for (i = 0; i < n; i++) {
        if (!is_printable(s[i]) || s[i] == '#')
            return 0;
    }
    return 1;
}

int manifest_record_valid(const manifest_record *r)
{
    size_t i;

    if (r == NUL)
        return 0;
    if (!field_ok(r->iso, sizeof(r->iso), 0))
        return 0;
    if (!field_ok(r->family, sizeof(r->family), 1))
        return 0;
    if (r->size > MANIFEST_SIZE_MAX)
        return 0;
    if (r->has_sha256) {
        if (cstr_len(r->sha256) != MANIFEST_SHA256_HEX)
            return 0;
        for (i = 0; i < MANIFEST_SHA256_HEX; i++) {
            char c = r->sha256[i];

            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return 0;
        }
    }
    /* The same rule manifest_parse applies on the way back in: whatever this
     * function accepts, render() writes and parse() must return unchanged. */
    return field_ok(r->uki, sizeof(r->uki), 0);
}

int manifest_upsert(manifest_record *recs, size_t *n, size_t cap,
                    const manifest_record *rec)
{
    size_t i;

    if (recs == NUL || n == NUL || rec == NUL || *n > cap)
        return MANIFEST_ERR_BAD_ARG;
    if (!manifest_record_valid(rec))
        return MANIFEST_ERR_BAD_RECORD;

    /* Match on the ISO path, which is the record's identity: re-blessing the
     * same ISO under a different name must replace, not accumulate, or the
     * loader would find a stale record first and boot the old UKI. */
    for (i = 0; i < *n; i++) {
        if (manifest_path_eq(recs[i].iso, rec->iso)) {
            recs[i] = *rec;
            return MANIFEST_OK;
        }
    }
    if (*n == cap)
        return MANIFEST_ERR_NOSPACE;
    recs[*n] = *rec;
    (*n)++;
    return MANIFEST_OK;
}

/* ------------------------------------------------------------ rendering */

/* Append helpers. Every one of them is a no-op once the output has
 * overflowed, and *used keeps counting, so the caller learns the size it
 * would have needed instead of getting a half-written file. */
typedef struct {
    char  *out;
    size_t cap;
    size_t used;
    int    overflow;
} sink;

static void put_bytes(sink *k, const char *s, size_t n)
{
    size_t i;

    if (k->used + n > k->cap) {
        k->overflow = 1;
        k->used += n;
        return;
    }
    for (i = 0; i < n; i++)
        k->out[k->used + i] = s[i];
    k->used += n;
}

static void put_str(sink *k, const char *s)
{
    put_bytes(k, s, cstr_len(s));
}

static void put_u64(sink *k, uint64_t v)
{
    char tmp[20];
    size_t n = 0;

    if (v == 0) {
        put_str(k, "0");
        return;
    }
    while (v != 0 && n < sizeof(tmp)) {
        tmp[n] = (char)('0' + (char)(v % 10u));
        v /= 10u;
        n++;
    }
    while (n > 0) {
        n--;
        put_bytes(k, &tmp[n], 1);
    }
}

/* The header exists so a human who opens the file on another OS knows what
 * it is and that hand-edits survive only until the next bless. It is the
 * only comment render() emits, and the only one a rewrite preserves. */
static const char g_header[] =
    "# falcon-boot blessed-artifact manifest (docs/DESIGN.md Section 8.1)\n"
    "# Generated by falcon-boot-sign. Rewritten in full on every bless:\n"
    "# hand-added comments here are not preserved.\n";

int manifest_render(const manifest_record *recs, size_t n,
                    char *out, size_t cap, size_t *used)
{
    sink k;
    size_t i;

    if (used == NUL || (recs == NUL && n != 0) || (out == NUL && cap != 0))
        return MANIFEST_ERR_BAD_ARG;
    for (i = 0; i < n; i++) {
        if (!manifest_record_valid(&recs[i]))
            return MANIFEST_ERR_BAD_RECORD;
    }

    k.out = out;
    k.cap = cap;
    k.used = 0;
    k.overflow = 0;

    put_bytes(&k, g_header, sizeof(g_header) - 1u);
    for (i = 0; i < n; i++) {
        put_str(&k, "\niso=");
        put_str(&k, recs[i].iso);
        put_str(&k, "\nsize=");
        put_u64(&k, recs[i].size);
        if (recs[i].has_sha256) {
            put_str(&k, "\nsha256=");
            put_str(&k, recs[i].sha256);
        }
        put_str(&k, "\nuki=");
        put_str(&k, recs[i].uki);
        if (recs[i].family[0] != '\0') {
            put_str(&k, "\nfamily=");
            put_str(&k, recs[i].family);
        }
        put_str(&k, "\n");
    }

    *used = k.used;
    return k.overflow ? MANIFEST_ERR_NOSPACE : MANIFEST_OK;
}
