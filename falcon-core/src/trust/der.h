/* falcon-boot: a DER reader, just enough for X.509 and Authenticode.
 *
 * Every certificate table on every ISO dropped on p2 passes through this
 * before anything has verified it (DESIGN Section 7.8, "The parser is attack
 * surface"). So it is deliberately small and deliberately strict:
 *
 *   - One element at a time off a span, never a tree. Callers walk the
 *     structure they expect with der_read/der_expect and stop at the first
 *     surprise, so there is no recursion and no nesting depth to bound.
 *   - Low-tag-number form only. Every tag X.509 and PKCS#7 use fits in the
 *     identifier byte; the multi-byte form (0x1f) is refused.
 *   - Definite lengths only, at most four length octets. Indefinite length is
 *     BER, which Authenticode does not emit; refusing it keeps every length
 *     a bound known before the body is touched.
 *   - A length that runs past the span is an error, never a truncation.
 *
 * Nothing here allocates or copies: a span is a pointer into the caller's
 * buffer plus a length, and every result is another span inside it.
 *
 * EFI-free, like everything under src/secboot except the variable reads, so it
 * is tested off the firmware and fuzzed by the corruption sweep.
 */

#ifndef FALCON_SECBOOT_DER_H
#define FALCON_SECBOOT_DER_H

#include <stddef.h>
#include <stdint.h>

enum {
    DER_OK = 0,
    DER_ERR = -1,       /* malformed, or not the tag the caller required */
    DER_END = -2        /* the span is empty: no more elements */
};

/* Identifier octets for the tags this module's callers name. */
#define DER_BOOLEAN     0x01u
#define DER_INTEGER     0x02u
#define DER_OCTETS      0x04u
#define DER_NULL        0x05u
#define DER_OID         0x06u
#define DER_UTF8        0x0cu
#define DER_PRINTABLE   0x13u
#define DER_T61         0x14u
#define DER_IA5         0x16u
#define DER_BMP         0x1eu
#define DER_SEQUENCE    0x30u
#define DER_SET         0x31u
#define DER_CTX(n)      (0x80u | (n))   /* [n] IMPLICIT, primitive */
#define DER_CTXC(n)     (0xa0u | (n))   /* [n], constructed */

typedef struct {
    const uint8_t *p;
    size_t len;
} der_span;

typedef struct {
    uint8_t  tag;
    der_span body;      /* the contents octets */
    der_span whole;     /* identifier + length + contents */
} der_tlv;

/* Pops one element off the front of *in. DER_END on an empty span, DER_ERR on
 * anything malformed; *in is advanced only on DER_OK. */
int der_read(der_span *in, der_tlv *out);

/* der_read, then DER_ERR unless the tag is `tag`. */
int der_expect(der_span *in, uint8_t tag, der_tlv *out);

/* der_read of an OPTIONAL element: pops it only when its tag is `tag`.
 * Returns 1 when popped, 0 when the next element is something else or the
 * span is empty, DER_ERR when the next element is malformed. */
int der_optional(der_span *in, uint8_t tag, der_tlv *out);

/* Byte equality of two spans. Two empty spans are equal. */
int der_eq(der_span a, der_span b);

/* Does an OID element's body equal the given encoded arc bytes? */
int der_oid_is(der_span body, const uint8_t *oid, size_t oid_len);

#endif /* FALCON_SECBOOT_DER_H */
