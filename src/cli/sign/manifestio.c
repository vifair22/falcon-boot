/* Manifest file I/O shared by the bless and prepare paths. See manifestio.h. */

#include <cli/sign/sign.h>
#include <cli/sign/manifestio.h>

#include <stdio.h>
#include <stdlib.h>

int sgn_manifest_path(const char *stick, char *out, size_t cap)
{
    if (stick == NULL || out == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "sgn_manifest_path: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    if (snprintf(out, cap, "%s/%s/manifest", stick, SGN_BLESSED_DIR) >=
        (int)cap) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "the manifest's path under '%s' is too long", stick);
        return SGN_ERR_NOSPACE;
    }
    return SGN_OK;
}

int sgn_manifest_load(const char *path, manifest_record *recs, size_t cap,
                      size_t *n)
{
    char *text = NULL;
    size_t len = 0;
    int rc;

    if (path == NULL || recs == NULL || n == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "sgn_manifest_load: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    *n = 0;

    /* An absent manifest is the first bless or prepare on this stick, not an
     * error; a corrupt one degrades to the records that did parse, which is
     * the same rule the loader follows (DESIGN Section 8.1). */
    if (!sgn_file_exists(path))
        return SGN_OK;

    rc = sgn_read_file(path, &text, &len, SGN_MANIFEST_TEXT_MAX);
    if (rc != SGN_OK)
        return rc;

    rc = manifest_parse(text, len, recs, cap, n);
    free(text);
    if (rc == MANIFEST_ERR_NOSPACE) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "'%s' holds %zu records, more than this build's limit "
                      "of %zu", path, *n, cap);
        return SGN_ERR_NOSPACE;
    }
    if (rc != MANIFEST_OK) {
        sgn_set_error(SGN_ERR_INTERNAL,
                      "manifest parse rejected its own arguments");
        return SGN_ERR_INTERNAL;
    }
    sgn_log(SGN_LOG_DEBUG, "manifest: %zu existing record(s)", *n);
    return SGN_OK;
}

int sgn_manifest_update(const char *path, const manifest_record *rec,
                        int *replaced)
{
    manifest_record *recs = NULL;
    char *rendered = NULL;
    size_t n = 0;
    size_t used = 0;
    int rc;

    if (path == NULL || rec == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "sgn_manifest_update: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    if (replaced != NULL)
        *replaced = 0;

    recs = calloc(SGN_MANIFEST_RECORDS, sizeof(*recs));
    rendered = malloc(SGN_MANIFEST_TEXT_MAX);
    if (recs == NULL || rendered == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory building the manifest");
        rc = SGN_ERR_IO;
        goto out;
    }

    rc = sgn_manifest_load(path, recs, SGN_MANIFEST_RECORDS, &n);
    if (rc != SGN_OK)
        goto out;

    if (replaced != NULL)
        *replaced = (manifest_find(recs, n, rec->iso) != NULL);

    rc = manifest_upsert(recs, &n, SGN_MANIFEST_RECORDS, rec);
    if (rc != MANIFEST_OK) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "could not add the record to the manifest (%d)", rc);
        rc = SGN_ERR_NOSPACE;
        goto out;
    }
    rc = manifest_render(recs, n, rendered, SGN_MANIFEST_TEXT_MAX, &used);
    if (rc != MANIFEST_OK) {
        sgn_set_error(SGN_ERR_NOSPACE, "the manifest would need %zu bytes",
                      used);
        rc = SGN_ERR_NOSPACE;
        goto out;
    }
    rc = sgn_write_file_atomic(path, rendered, used);

out:
    free(rendered);
    free(recs);
    return rc;
}
