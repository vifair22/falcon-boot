/* The decisions behind `falcon-boot-sign prepare`. See prepcore.h. */

#include <sign/sign.h>
#include <sign/prepcore.h>

#include <stdio.h>
#include <string.h>

prep_state prep_lookup(const manifest_record *recs, size_t n,
                       const char *iso_rel, uint64_t cur_size,
                       const manifest_record **out)
{
    const manifest_record *r;

    if (out != NULL)
        *out = NULL;
    if (recs == NULL || iso_rel == NULL)
        return PREP_STATE_ABSENT;

    r = manifest_find(recs, n, iso_rel);
    if (r == NULL)
        return PREP_STATE_ABSENT;
    if (out != NULL)
        *out = r;

    if (r->kind != MANIFEST_KIND_PREPARED)
        return PREP_STATE_OTHER_KIND;
    if (r->size != cur_size)
        return PREP_STATE_STALE;
    return PREP_STATE_READY;
}

int prep_fits(uint64_t patched_len, uint64_t orig_len)
{
    uint64_t over;

    if (patched_len <= orig_len)
        return SGN_OK;
    over = patched_len - orig_len;

    /* The one condition that makes the size-preserving shape impossible. It
     * is named in full, with both numbers and the two things that actually
     * help, because the alternative is a sidecar larger than the extent it is
     * served over, which would corrupt the image Windows reads. */
    sgn_set_error(SGN_ERR_NOSPACE,
                  "the patched boot.wim is %llu bytes and the extent it must "
                  "fit is %llu (%llu too many). Retry with --recompress, "
                  "which does a full LZX repack and typically reclaims "
                  "several megabytes more. If that still does not fit, this "
                  "boot.wim has no repack margin left.",
                  (unsigned long long)patched_len,
                  (unsigned long long)orig_len,
                  (unsigned long long)over);
    return SGN_ERR_NOSPACE;
}

int prep_build_record(const char *iso_rel, uint64_t iso_size,
                      const char *sha_hex, uint64_t splice_off,
                      uint64_t splice_len, const char *splice_name,
                      manifest_record *out)
{
    manifest_record rec;
    size_t n;

    if (iso_rel == NULL || sha_hex == NULL || splice_name == NULL ||
        out == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "prep_build_record: bad arguments");
        return SGN_ERR_INTERNAL;
    }

    memset(&rec, 0, sizeof(rec));
    rec.kind = MANIFEST_KIND_PREPARED;
    rec.size = iso_size;

    n = strlen(iso_rel);
    if (n == 0 || n + 1u > sizeof(rec.iso)) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "the ISO's path on p2 is %zu bytes, which does not fit "
                      "the manifest's %zu-byte field: move it shallower or "
                      "pass --iso-path", n, sizeof(rec.iso) - 1u);
        return SGN_ERR_NOSPACE;
    }
    memcpy(rec.iso, iso_rel, n + 1u);

    if (strlen(sha_hex) != MANIFEST_SHA256_HEX) {
        sgn_set_error(SGN_ERR_INTERNAL,
                      "a SHA-256 digest is %u hex characters, not %zu",
                      (unsigned)MANIFEST_SHA256_HEX, strlen(sha_hex));
        return SGN_ERR_INTERNAL;
    }
    memcpy(rec.sha256, sha_hex, (size_t)MANIFEST_SHA256_HEX + 1u);
    rec.has_sha256 = 1;

    /* The family is fixed rather than passed: this record is only ever
     * written for media that already classified as Windows, and the loader
     * matches on this exact string (DESIGN Section 8.1). */
    memcpy(rec.family, "windows", sizeof("windows"));

    n = strlen(splice_name);
    if (n == 0 || n + 1u > sizeof(rec.splice[0].name)) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "'%s' does not fit the manifest's %zu-byte splice name",
                      splice_name, sizeof(rec.splice[0].name) - 1u);
        return SGN_ERR_NOSPACE;
    }
    memcpy(rec.splice[0].name, splice_name, n + 1u);
    rec.splice[0].iso_off = splice_off;
    rec.splice[0].len = splice_len;
    rec.splice_n = 1;

    /* The last gate before 600 MB of work: whatever this rejects, the loader
     * would have dropped on the way back in, and a sidecar nothing points at
     * is worse than a refusal. */
    if (!manifest_record_valid(&rec)) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "the record for this ISO cannot be written to the "
                      "manifest: check that its path carries no '#' and no "
                      "non-ASCII byte, and that the extent (%llu+%llu) is in "
                      "range. Rename the file or pass --iso-path.",
                      (unsigned long long)splice_off,
                      (unsigned long long)splice_len);
        return SGN_ERR_FORMAT;
    }

    *out = rec;
    return SGN_OK;
}

int prep_cfg_render(const char *iso_rel, uint64_t iso_size,
                    char *out, size_t cap, size_t *used)
{
    int n;

    if (iso_rel == NULL || out == NULL || used == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "prep_cfg_render: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    *used = 0;
    n = snprintf(out, cap,
                 "# falcon-boot winjump configuration, generated by "
                 "falcon-boot-sign prepare\n"
                 "# DESIGN Section 6.3; read by falcon-winjump/src/winjump.c\n"
                 "iso=%s\n"
                 "size=%llu\n",
                 iso_rel, (unsigned long long)iso_size);
    if (n < 0 || (size_t)n >= cap) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "the winjump config does not fit %zu bytes", cap);
        return SGN_ERR_NOSPACE;
    }
    *used = (size_t)n;
    return SGN_OK;
}
