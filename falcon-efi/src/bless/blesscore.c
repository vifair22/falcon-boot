/* The EFI-free half of the blessed-boot path. See blesscore.h for the
 * contract and for why the manifest is untrusted input rather than a
 * security decision.
 */

#include <bless/blesscore.h>

void bless_cache_init(bless_cache *c)
{
    if (c == (bless_cache *)0)
        return;
    c->n = 0;
    c->dropped = 0;
}

size_t bless_cache_add(bless_cache *c, const void *buf, size_t len,
                       const void *src)
{
    size_t room;
    size_t total = 0;
    size_t added;
    size_t i;

    if (c == (bless_cache *)0)
        return 0;

    room = BLESS_MAX_RECORDS - c->n;

    /* Parsed straight into the cache's free tail rather than into a scratch
     * array: a scratch array of the cap would be sixteen kilobytes of stack
     * in a loader whose frames are policed at 32 KB, and copying it in
     * afterwards would buy nothing. manifest_parse fills what fits and still
     * reports how many records the buffer actually held, which is exactly the
     * overflow accounting this needs.
     *
     * A full cache still parses, with nowhere to put the result: the count is
     * the whole point of the call by then, because "three blessed ISOs are
     * not shown" is a thing to tell the user and silently returning early
     * would make a full stick look like an empty one. */
    (void)manifest_parse(buf, len, (room != 0) ? &c->rec[c->n]
                                               : (manifest_record *)0,
                         room, &total);

    added = (total < room) ? total : room;
    for (i = 0; i < added; i++)
        c->src[c->n + i] = src;
    c->n += added;
    c->dropped += total - added;
    return added;
}

bless_state bless_cache_lookup(const bless_cache *c, const void *src,
                               const uint16_t *path, uint64_t cur_size,
                               bless_hit *out)
{
    char ascii[MANIFEST_PATH_MAX];
    size_t i;

    if (out != (bless_hit *)0) {
        out->rec = (const manifest_record *)0;
        out->src = (const void *)0;
        out->state = BLESS_NONE;
    }
    if (c == (const bless_cache *)0 || path == (const uint16_t *)0)
        return BLESS_NONE;

    /* A path no record could carry is answered without a search. This is the
     * one place the ASCII-only rule of the manifest format shows through to a
     * user: an ISO whose name is not ASCII cannot be blessed, because a
     * record naming it would have been dropped at parse time. */
    if (manifest_path_from_ucs2(path, ascii, sizeof ascii) != MANIFEST_OK)
        return BLESS_NONE;

    for (i = 0; i < c->n; i++) {
        const manifest_record *hit = (const manifest_record *)0;
        manifest_bless st;

        if (c->src[i] != src)
            continue;
        /* One record at a time through manifest_lookup rather than a
         * hand-rolled compare, so the "path AND size" rule of Section 8.1
         * has exactly one implementation. Records for a volume are appended
         * contiguously, but nothing here depends on that. */
        st = manifest_lookup(&c->rec[i], 1u, ascii, cur_size, &hit);
        if (st == MANIFEST_BLESS_NONE)
            continue;

        if (out != (bless_hit *)0) {
            out->rec = hit;
            out->src = src;
            out->state = (st == MANIFEST_BLESS_FRESH) ? BLESS_FRESH
                                                      : BLESS_STALE;
        }
        return (st == MANIFEST_BLESS_FRESH) ? BLESS_FRESH : BLESS_STALE;
    }

    return BLESS_NONE;
}

/* ---- what a hit means ---- */

/* The record behind a hit, or NULL when there is none. Every rule below reads
 * the kind through this, so "no record" and "a record of a kind nobody knows"
 * cannot take different paths by accident. */
static const manifest_record *hit_rec(const bless_hit *h)
{
    if (h == (const bless_hit *)0 || h->state == BLESS_NONE)
        return (const manifest_record *)0;
    return h->rec;
}

bless_action bless_hit_action(const bless_hit *h)
{
    const manifest_record *r = hit_rec(h);

    if (h == (const bless_hit *)0 || r == (const manifest_record *)0 ||
        h->state != BLESS_FRESH)
        return BLESS_ACT_PLAIN;
    if (r->kind == MANIFEST_KIND_PREPARED)
        return BLESS_ACT_OVERLAY;
    return BLESS_ACT_UKI;
}

bless_prep bless_prep_state(const bless_hit *h, family_id fam)
{
    const manifest_record *r = hit_rec(h);

    if (h == (const bless_hit *)0)
        return BLESS_PREP_NA;
    if (r != (const manifest_record *)0 &&
        r->kind == MANIFEST_KIND_PREPARED) {
        return (h->state == BLESS_FRESH) ? BLESS_PREP_READY : BLESS_PREP_STALE;
    }
    /* A blessed record on Windows media is not a preparation and must not be
     * reported as a missing one: it is a record the user's own tooling wrote,
     * and offering to prepare over it would be the loader second-guessing it.
     * The bless tool is what refuses to write that combination. */
    if (r != (const manifest_record *)0)
        return BLESS_PREP_NA;
    return (fam == FAMILY_WINDOWS) ? BLESS_PREP_ABSENT : BLESS_PREP_NA;
}

const uint16_t *bless_suffix(const bless_hit *h, family_id fam)
{
    /* Two leading spaces, matching the survey suffix the same label already
     * carries, so an entry reads "...iso  [arch, signed]  [blessed]". */
    static const uint16_t blessed[] = u"  [blessed]";
    static const uint16_t blessed_stale[] = u"  [blessed, stale]";
    static const uint16_t prepared[] = u"  [prepared]";
    static const uint16_t prepared_stale[] = u"  [prepared, stale]";
    static const uint16_t unprepared[] = u"  [not prepared]";
    const manifest_record *r;

    if (h == (const bless_hit *)0)
        return (const uint16_t *)0;
    r = hit_rec(h);
    if (r != (const manifest_record *)0 && r->kind == MANIFEST_KIND_BLESSED)
        return (h->state == BLESS_FRESH) ? blessed : blessed_stale;

    switch (bless_prep_state(h, fam)) {
    case BLESS_PREP_READY:
        return prepared;
    case BLESS_PREP_STALE:
        return prepared_stale;
    case BLESS_PREP_ABSENT:
        return unprepared;
    case BLESS_PREP_NA:
    default:
        return (const uint16_t *)0;
    }
}

/* ---- the byte map ---- */

/* The splices a prepared record may ask for: the size-preserving shape needs
 * one, and four was the overlay's own cap when this was written. The overlay
 * grew for the loader's in-memory Windows patch (falcon-efi/src/win); a record
 * off the stick keeps the smaller bound. */
#define PREPARED_SPLICE_MAX 4u

int bless_prepared_overlay(const manifest_record *r, iso_overlay *out)
{
    size_t i;

    if (r == (const manifest_record *)0 || out == (iso_overlay *)0)
        return BLESS_ERR_BAD_ARG;

    for (i = 0; i < ISO_MAX_OVERRIDES; i++) {
        out->ovl[i].iso_off = 0;
        out->ovl[i].len = 0;
        out->ovl[i].src = (fs_file *)0;
        out->ovl[i].src_off = 0;
        out->ovl[i].mem = (const uint8_t *)0;
    }
    out->n = 0;
    /* No append region: the MVP patch shape is size-preserving (Section 6.3)
     * and the manifest format has no key that could ask for one, so a set
     * built from a record can never extend the medium. */
    out->extra = 0;

    if (r->kind != MANIFEST_KIND_PREPARED)
        return BLESS_ERR_KIND;
    if (r->splice_n == 0)
        return BLESS_ERR_NOSPLICE;
    if ((size_t)r->splice_n > PREPARED_SPLICE_MAX)
        return BLESS_ERR_SPLICES;

    for (i = 0; i < (size_t)r->splice_n; i++) {
        out->ovl[i].iso_off = r->splice[i].iso_off;
        out->ovl[i].len = r->splice[i].len;
        /* The sidecar IS the replacement, whole: Section 8.1's splice names a
         * file, not a range inside one. */
        out->ovl[i].src_off = 0;
    }
    out->n = (size_t)r->splice_n;
    return BLESS_OK;
}

/* ---- the next-boot marker ---- */

int bless_next_decode(const void *buf, size_t len, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t n;
    size_t i;

    if (out == (char *)0 || cap == 0)
        return BLESS_ERR_BAD_ARG;
    out[0] = '\0';
    if (p == (const unsigned char *)0 || len == 0)
        return BLESS_ERR_VALUE;

    /* One trailing NUL is what this loader writes, so the value reads as a
     * string in any variable dumper. A writer that omitted it is accepted for
     * the same reason: neither shape is ambiguous, and refusing one would be
     * a rule with no purpose behind it. */
    n = len;
    if (p[n - 1] == '\0')
        n--;
    if (n == 0 || n >= cap)
        return BLESS_ERR_VALUE;

    /* Judged whole before a byte of it is copied, so a refusal leaves the
     * caller's buffer empty rather than holding the prefix of something that
     * was rejected. Printable ASCII only, which is not a nicety: manifest_parse
     * drops any record carrying a byte outside it, so a value with one cannot
     * name a record and there is nothing to gain by carrying it further. An
     * embedded NUL lands here too and is refused rather than treated as a
     * terminator, so no two readers of this value can disagree about where it
     * ends. */
    for (i = 0; i < n; i++) {
        if (p[i] < 0x20u || p[i] > 0x7Eu)
            return BLESS_ERR_VALUE;
    }
    for (i = 0; i < n; i++)
        out[i] = (char)p[i];
    out[n] = '\0';
    return BLESS_OK;
}

int bless_next_names(const uint16_t *path, const char *name)
{
    char ascii[MANIFEST_PATH_MAX];

    if (path == (const uint16_t *)0 || name == (const char *)0 ||
        name[0] == '\0')
        return 0;
    if (manifest_path_from_ucs2(path, ascii, sizeof ascii) != MANIFEST_OK)
        return 0;
    return manifest_path_eq(ascii, name);
}

int bless_next_is_reserved(const char *name)
{
    return name != (const char *)0 && name[0] == ':';
}

int bless_next_is_personal(const char *name)
{
    const char *want = BLESS_NEXT_PERSONAL;
    size_t i;

    if (name == (const char *)0)
        return 0;
    /* Exact match, not a prefix: ":personalish" is not this target, and
     * treating it as one would make the reserved namespace impossible to
     * extend without breaking older markers. */
    for (i = 0; want[i] != '\0'; i++) {
        if (name[i] != want[i])
            return 0;
    }
    return name[i] == '\0';
}
