/* falcon-boot-sign: reading and rewriting p2's manifest file.
 *
 * A bless ends by parsing whatever manifest is already on the stick,
 * replacing or appending this ISO's record, and rendering the whole file
 * back. A record the parser skips is therefore not written back: that is how
 * a prepared record an older tool left (DESIGN Section 8.1) leaves the file
 * the next time anything is blessed.
 *
 * The parse side is deliberately forgiving, for the same reason the loader's
 * is (manifest.h): the file lives on writable, unverified exFAT, so a corrupt
 * one degrades to the records that did parse rather than stopping the tool.
 */

#ifndef FALCON_CLI_SIGN_MANIFESTIO_H
#define FALCON_CLI_SIGN_MANIFESTIO_H

#include <stddef.h>

#include <manifest/manifest.h>

/* How many records one manifest may hold in this build, and the largest file
 * the reader will take. 512 records at the field caps of manifest.h is well
 * under the megabyte; both are reported by name when either is exceeded, so a
 * stick that outgrows them says so rather than losing records. */
#define SGN_MANIFEST_RECORDS 512u
#define SGN_MANIFEST_TEXT_MAX (1u << 20)

/* Where the manifest lives on p2, under the reserved directory of DESIGN
 * Section 6.2. */
#define SGN_BLESSED_DIR "falcon/blessed"

/* "<stick>/falcon/blessed/manifest". SGN_ERR_NOSPACE when it does not fit. */
int sgn_manifest_path(const char *stick, char *out, size_t cap);

/* Parses the manifest at path into a caller-supplied array. An absent file is
 * not an error: it is the first bless on this stick, and *n comes
 * back zero. SGN_ERR_NOSPACE when the file holds more records than cap. */
int sgn_manifest_load(const char *path, manifest_record *recs, size_t cap,
                      size_t *n);

/* Idempotent upsert, written through a sibling temporary and renamed (see
 * sgn_write_file_atomic), so an interrupted run leaves the previous manifest
 * intact rather than a truncated one. *replaced, when non-NULL, reports
 * whether a record for this ISO already existed. */
int sgn_manifest_update(const char *path, const manifest_record *rec,
                        int *replaced);

#endif /* FALCON_CLI_SIGN_MANIFESTIO_H */
