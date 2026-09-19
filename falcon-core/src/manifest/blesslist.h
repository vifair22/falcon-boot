/* falcon-boot: the list of images the loader asks the signing appliance to
 * bless (docs/DESIGN.md Section 7.5, "the appliance's role, revised").
 *
 * Consent is given in the loader's menu. The loader writes the paths the user
 * confirmed into this list and hands it to the appliance as a credential on
 * the synthetic filesystem of Section 5.1, beside the passphrase; sd-stub
 * delivers it to /.extra/credentials/ and the appliance signs exactly what it
 * names. The list never touches disk, so nothing written to p2 can add to it.
 *
 * Format: one path per line, each ended by '\n', relative to the root of p2
 * and spelled the way a manifest record spells it (falcon-core/src/manifest/
 * manifest.h). The one definition of what an acceptable path is lives here,
 * so the writer and the reader cannot disagree about it; the reader applies it
 * again because the bytes crossed a boot.
 */

#ifndef FALCON_BLESSLIST_H
#define FALCON_BLESSLIST_H

#include <stddef.h>

enum {
    BLESSLIST_OK          = 0,
    BLESSLIST_ERR_BAD_ARG = -1,
    BLESSLIST_ERR_NOSPACE = -2,
    BLESSLIST_ERR_PATH    = -3   /* not a path this list may carry */
};

/* The credential's file name. sd-stub picks up *.cred from the image's
 * drop-in directory and keeps the name. */
#define BLESSLIST_CRED "falcon.bless.cred"

/* The most bytes a list may take: well over the menu's image cap at full
 * path length. */
#define BLESSLIST_MAX 32768u

/* Nonzero when path may be listed: printable ASCII without '#' or '\\',
 * relative, no empty, "." or ".." component, not under the reserved falcon/
 * directory, shorter than MANIFEST_PATH_MAX, and named .iso, .img or .efi. */
int blesslist_path_ok(const char *path);

/* Appends path and its newline at buf[*len]; *len is advanced.
 * BLESSLIST_ERR_PATH for a path blesslist_path_ok refuses,
 * BLESSLIST_ERR_NOSPACE when it does not fit in cap. buf is not
 * NUL-terminated by this. */
int blesslist_add(char *buf, size_t cap, size_t *len, const char *path);

/* Reads the line at *pos into out (NUL-terminated) and advances *pos past
 * it. 1 for an acceptable path, 0 at the end of the list,
 * BLESSLIST_ERR_PATH for a line that is not one (the caller reports it and
 * reads on). A last line without its newline counts. */
int blesslist_next(const char *buf, size_t len, size_t *pos, char *out,
                   size_t cap);

#endif /* FALCON_BLESSLIST_H */
