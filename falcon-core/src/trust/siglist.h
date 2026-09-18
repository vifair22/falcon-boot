/* falcon-boot: the platform's trust stores as data (DESIGN Section 7.8).
 *
 * db, dbx, MokListRT and MokListXRT are all the same shape: a sequence of
 * EFI_SIGNATURE_LISTs (UEFI 2.10 Section 32.4.1), each a typed header over
 * fixed-size EFI_SIGNATURE_DATA entries, each of those an owner GUID and the
 * signature itself. Two entry types matter to a prediction: X.509
 * certificates (anchors, or revocations in dbx) and SHA-256 image hashes.
 * Every other type is skipped, not an error: dbx in particular carries
 * X509_SHA256 entries (a certificate's TBS hash plus a revocation time) that
 * this code has no use for.
 *
 * SbatLevelRT is the other revocation input: shim's text list of the minimum
 * generation each SBAT component must carry.
 *
 * Read-only, EFI-free, and as strict as the rest of src/secboot: a list whose
 * sizes do not add up ends the walk rather than being guessed at, and the
 * caller treats the rest of that variable as absent.
 */

#ifndef FALCON_SECBOOT_SIGLIST_H
#define FALCON_SECBOOT_SIGLIST_H

#include <trust/der.h>

typedef enum {
    SIG_OTHER = 0,
    SIG_X509,
    SIG_SHA256
} sig_type;

typedef struct {
    der_span rest;          /* lists not yet opened */
    der_span entries;       /* entries left in the current list */
    size_t   sig_size;      /* SignatureSize of the current list */
    sig_type type;          /* SignatureType of the current list */
} siglist_iter;

void siglist_begin(siglist_iter *it, der_span store);

/* The next signature in the store: its type and its data, owner GUID
 * stripped. Returns 1 with an entry, 0 at the end, DER_ERR at a list whose
 * sizes are impossible, after which the walk is over. */
int siglist_next(siglist_iter *it, sig_type *type, der_span *data);

/* Does the store hold this SHA-256 as an image-hash entry? */
int siglist_has_hash(der_span store, const uint8_t hash[32]);

/* Does image SBAT metadata `img` carry any component whose generation is
 * below the same component's line in `level` (SbatLevelRT)? That is shim's
 * revocation rule, and 1 means shim will refuse the image. 0 means no
 * component is revoked, including when either text is empty or a line does
 * not parse: a revocation is only reported when it is certain. */
int sbat_revoked(der_span level, der_span img);

#endif /* FALCON_SECBOOT_SIGLIST_H */
