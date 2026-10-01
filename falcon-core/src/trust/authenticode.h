/* falcon-boot: what an image's Authenticode signature block says, and the
 * image's Authenticode hash (DESIGN Section 7.8).
 *
 * Three jobs, all of them reading rather than judging:
 *
 *   1. The image's layout for the hash: which byte ranges of the file the
 *      Authenticode digest covers, where the certificate table and the .sbat
 *      section are. Built from a header window, via falcon-core/src/pe.
 *   2. The hash itself, streamed through a read callback so a caller never
 *      holds the whole image. Only the ranges shim hashes are read.
 *   3. The certificate table: its WIN_CERTIFICATE entries, and inside each the
 *      PKCS#7 SignedData's certificate set, signer identity and signed digest.
 *
 * The digest in (3) is extracted and never compared against (2) by the boot
 * path: DESIGN 7.8 rejects in-loader verification and a digest check is half
 * of one. It is exposed because it is the test suite's oracle: sbsign computed
 * it, so an Authenticode hash here that disagrees with it is a bug here.
 *
 * The hash follows SHIM, not EDK2, where the two differ, because shim is what
 * verifies every image the loader launches (DESIGN 7.6). They differ in one
 * place: data past the last section is hashed by EDK2 always and by shim only
 * when the image carries a certificate table. That matters only for an
 * unsigned image enrolled by hash, which is exactly what MokManager's "enroll
 * hash" produces, so shim's rule is the one that predicts correctly.
 */

#ifndef FALCON_TRUST_AUTHENTICODE_H
#define FALCON_TRUST_AUTHENTICODE_H

#include <crypto/hash.h>
#include <trust/der.h>
#include <trust/x509.h>

enum {
    AC_OK = 0,
    AC_ERR_BAD_ARG = -1,
    AC_ERR_FORMAT = -2,     /* not a PE, or a layout shim would refuse to hash */
    AC_ERR_SHORT = -3,      /* the headers are not all inside the window */
    AC_ERR_IO = -4          /* the read callback failed */
};

/* PE/COFF allows 96 sections; loaders in practice refuse more. */
#define AC_SECT_MAX 96u

/* A certificate table larger than this is not an Authenticode block anyone
 * ships (a triple-signed shim is under 8 KiB); past it the caller reads
 * nothing and predicts nothing. */
#define AC_CERTS_MAX (64u * 1024u)

typedef struct {
    uint64_t size;              /* the file's length */
    size_t   checksum_off;      /* CheckSum, skipped by the hash */
    size_t   secdir_off;        /* the security directory entry, or 0 */
    uint32_t size_of_headers;
    uint32_t certs_off;         /* certificate table: file offset and size, */
    uint32_t certs_len;         /*   both 0 for an unsigned image */
    uint32_t sbat_off;          /* .sbat raw data, both 0 when absent */
    uint32_t sbat_len;
    uint16_t nsect;             /* sections with raw data, in file order */
    struct {
        uint32_t off;
        uint32_t len;
    } sect[AC_SECT_MAX];
} ac_layout;

/* Builds the layout from the first `len` bytes of a file `total` bytes long.
 * AC_ERR_SHORT when the section table is not inside the window: read more
 * (SizeOfHeaders is the natural window) and call again. */
int ac_layout_read(const void *head, size_t len, uint64_t total,
                   ac_layout *out);

/* Reads [off, off + len) of the image into buf. 0 on success. The same shape as
 * fs_read_fn, so fs_file_read_cb (falcon-core/src/fs) serves a file on p2
 * directly. */
typedef int (*ac_read_fn)(void *ctx, uint64_t off, uint64_t len, void *buf);

/* The Authenticode SHA-256 of the image, streamed through `read` in chunks of
 * at most scratch_len bytes. */
int ac_hash(const ac_layout *l, ac_read_fn read, void *ctx, uint8_t *scratch,
            size_t scratch_len, uint8_t out[32]);

/* The same hash in another algorithm: SHA-1 is what a TPM 1.2 event log
 * records an image by, and the station's TOTP enrollment finds the station
 * in that log by it (docs/DESIGN.md Section 12.15). out holds
 * h->digest_len bytes. */
int ac_hash_with(const ac_layout *l, const hash_desc *h, ac_read_fn read,
                 void *ctx, uint8_t *scratch, size_t scratch_len,
                 uint8_t *out);

/* Walks a certificate table's WIN_CERTIFICATE entries. *iter starts as the
 * whole table; each call yields the next PKCS#7 SignedData blob. Entries of
 * any other certificate type are skipped, as shim and EDK2 skip them.
 * Returns 1 with a blob, 0 at the end, DER_ERR when an entry's length is
 * impossible (the walk cannot continue past it). */
int ac_next_signature(der_span *iter, der_span *blob);

typedef struct {
    der_span certs;             /* body of the [0] certificate set */
    der_span signer_issuer;     /* SignerInfo sid, IssuerAndSerialNumber form */
    der_span signer_serial;
    der_span signer_ski;        /* SignerInfo sid, subjectKeyIdentifier form */
    der_span digest_oid;        /* SpcIndirectDataContent's DigestInfo */
    der_span digest;
} ac_signed;

/* Parses one SignedData blob. Only the first SignerInfo is read: Authenticode
 * requires exactly one, and a nested second signature lives in its
 * unauthenticated attributes, which DESIGN 7.8 ignores as the platform does. */
int ac_signed_parse(der_span blob, ac_signed *out);

/* Iterates the certificate set. *iter starts as s->certs. Returns 1 with a
 * certificate, 0 at the end, DER_ERR on one that does not parse. */
int ac_next_cert(der_span *iter, x509_cert *out);

/* The signer's certificate, found in the set by issuer + serial (or by SKI).
 * DER_OK or DER_ERR when the set does not contain it. */
int ac_signer_cert(const ac_signed *s, x509_cert *out);

/* The certificate in the set that issued `c`: subject equal to c's issuer and,
 * where both carry one, SKI equal to c's AKI. A self-issued certificate has no
 * issuer here. DER_OK or DER_ERR. */
int ac_issuer_cert(const ac_signed *s, const x509_cert *c, x509_cert *out);

#endif /* FALCON_TRUST_AUTHENTICODE_H */
