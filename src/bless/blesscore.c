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

const uint16_t *bless_suffix(bless_state s)
{
    /* Two leading spaces, matching the survey suffix the same label already
     * carries, so an entry reads "...iso  [arch, signed]  [blessed]". */
    static const uint16_t fresh[] = u"  [blessed]";
    static const uint16_t stale[] = u"  [blessed, stale]";

    if (s == BLESS_FRESH)
        return fresh;
    if (s == BLESS_STALE)
        return stale;
    return (const uint16_t *)0;
}
