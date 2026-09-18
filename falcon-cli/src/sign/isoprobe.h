/* falcon-boot-sign: the two namespaces an ISO can hold, and the seam the
 * unit tests reach them through.
 *
 * A hybrid image carries the same tree twice, once in ISO9660 (plus Rock
 * Ridge) and once in UDF, and the two are not obliged to agree. Microsoft's
 * oscdimg makes the disagreement total: the ISO9660 side of a Windows install
 * image is a stub with one README and nothing else, while every file that
 * matters lives in UDF. So an iso_view is the union of one listing per
 * namespace, and each entry remembers which helper produced it, because that
 * is also the only helper that can extract it again.
 *
 * The public surface of the view is in sign.h; what is here is the namespace
 * tag and the in-memory constructor, which exists so the listing parsers and
 * the case-insensitive matching can be tested without a subprocess and
 * without a multi-gigabyte image on disk.
 */

#ifndef FALCON_CLI_SIGN_ISOPROBE_H
#define FALCON_CLI_SIGN_ISOPROBE_H

#include <stddef.h>

#include <sign/sign.h>

typedef enum {
    ISO_READER_ISO9660 = 0,   /* bsdtar: ISO9660 and Rock Ridge */
    ISO_READER_7Z      = 1,   /* 7-Zip: UDF, and ISO9660 when there is no UDF */
    ISO_READER_N       = 2,
} iso_reader;

/* Chops one helper's raw output into NUL-terminated lines in place and
 * indexes every entry it names. text must outlive the view, which takes no
 * copy: each indexed name is a pointer into it. */
int iso_listing_index(iso_view *v, char *text, iso_reader reader);

/* Builds a view from listings already in memory rather than from a helper.
 * Either may be NULL. No ISO path is attached, so the result answers
 * iso_view_find and nothing that would have to read file content. */
int iso_view_from_listings(const char *iso9660, const char *udf,
                           iso_view **out);

/* Which helper owns the member iso_view_find returned, which is to say which
 * one can extract it. */
iso_reader iso_view_reader_of(const iso_view *v, const char *member);

/* How many entries came from one namespace. */
size_t iso_view_count(const iso_view *v, iso_reader reader);

#endif /* FALCON_CLI_SIGN_ISOPROBE_H */
