/* falcon-boot: the X.509 fields a boot prediction compares.
 *
 * Not a certificate validator. Nothing here checks a signature, a validity
 * period, a key usage or a basic constraint: the platform does all of that
 * when it verifies the image, and DESIGN Section 7.8 is explicit that the
 * loader does not. What this module extracts is what DESIGN 7.8's matching
 * rule names and nothing more:
 *
 *   - the whole DER encoding, for "this anchor IS this certificate";
 *   - issuer and subject Names as raw DER, compared byte for byte, never
 *     normalised, because a normalising comparison is a second opinion about
 *     equality the platform might not share;
 *   - the serial, for finding a SignerInfo's certificate by issuer + serial;
 *   - the Subject and Authority Key Identifiers, which are what tell two
 *     certificates with the same Name apart. Every sbctl install names its
 *     db certificate "CN=Database Key", so without them a name match would
 *     call any sbctl-signed image trusted on every sbctl machine.
 *
 * Plus the subject's CN as ASCII, for the menu.
 */

#ifndef FALCON_SECBOOT_X509_H
#define FALCON_SECBOOT_X509_H

#include <secboot/der.h>

typedef struct {
    der_span der;       /* the whole Certificate element */
    der_span serial;    /* INTEGER contents */
    der_span issuer;    /* Name, whole element */
    der_span subject;   /* Name, whole element */
    der_span ski;       /* subjectKeyIdentifier value; empty when absent */
    der_span aki;       /* authorityKeyIdentifier keyIdentifier; empty when absent */
} x509_cert;

/* Parses the Certificate element at the front of `in` (anything after it is
 * ignored). DER_OK or DER_ERR. An extension this module does not know is
 * skipped; a malformed one is an error, because a certificate whose
 * extensions do not parse is not one to draw conclusions from. */
int x509_parse(der_span in, x509_cert *out);

/* Writes the last CN in `name` (a Name element) as NUL-terminated ASCII,
 * truncated to fit `cap`. Characters outside printable ASCII, and every
 * BMPString character past 0x7e, are written as '?'. Returns the number of
 * characters written; 0 when the Name has no CN or does not parse. */
size_t x509_cn(der_span name, char *out, size_t cap);

/* DESIGN 7.8's "reaches": may certificate `c` have been issued by anchor `a`,
 * judged without a signature check?
 *
 *   - a IS c (same DER): always.
 *   - c is self-issued: only that. A self-issued certificate names itself as
 *     issuer, so any other anchor with the same Name is a different key. This
 *     is not hypothetical: sbctl's db certificates carry no extensions at all,
 *     every one is "CN=Database Key", and the Dell that prompted Section 7.8
 *     has two.
 *   - otherwise: a's subject is c's issuer, and where c has an AKI and a has
 *     an SKI, they agree.
 *
 * The caller applies the name rule only at the top of a chain, to the
 * certificate whose issuer the signature does not include; a certificate
 * whose issuer IS included is judged through that issuer instead. */
int x509_reaches(const x509_cert *c, const x509_cert *a);

#endif /* FALCON_SECBOOT_X509_H */
