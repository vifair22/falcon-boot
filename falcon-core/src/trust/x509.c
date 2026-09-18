#include <trust/x509.h>

#include <lib/string.h>

/* 2.5.29.14, 2.5.29.35, 2.5.4.3 */
static const uint8_t OID_SKI[] = { 0x55, 0x1d, 0x0e };
static const uint8_t OID_AKI[] = { 0x55, 0x1d, 0x23 };
static const uint8_t OID_CN[] = { 0x55, 0x04, 0x03 };

/* extnValue is an OCTET STRING wrapping the extension's own DER. */
static int ext_ski(der_span value, der_span *out)
{
    der_tlv t;

    if (der_expect(&value, DER_OCTETS, &t) != DER_OK)
        return DER_ERR;
    *out = t.body;
    return DER_OK;
}

/* AuthorityKeyIdentifier ::= SEQUENCE { keyIdentifier [0] IMPLICIT OPTIONAL,
 * authorityCertIssuer [1] OPTIONAL, authorityCertSerialNumber [2] OPTIONAL }.
 * Only the first is wanted; its absence leaves *out empty. */
static int ext_aki(der_span value, der_span *out)
{
    der_tlv seq, t;
    int rc;

    if (der_expect(&value, DER_SEQUENCE, &seq) != DER_OK)
        return DER_ERR;
    rc = der_optional(&seq.body, DER_CTX(0), &t);
    if (rc == DER_ERR)
        return DER_ERR;
    if (rc == 1)
        *out = t.body;
    return DER_OK;
}

static int parse_extensions(der_span exts, x509_cert *out)
{
    der_tlv list;

    if (der_expect(&exts, DER_SEQUENCE, &list) != DER_OK)
        return DER_ERR;
    for (;;) {
        der_tlv ext, oid, crit, value;
        int rc = der_read(&list.body, &ext);

        if (rc == DER_END)
            return DER_OK;
        if (rc != DER_OK || ext.tag != DER_SEQUENCE)
            return DER_ERR;
        if (der_expect(&ext.body, DER_OID, &oid) != DER_OK ||
            der_optional(&ext.body, DER_BOOLEAN, &crit) == DER_ERR ||
            der_expect(&ext.body, DER_OCTETS, &value) != DER_OK)
            return DER_ERR;
        if (der_oid_is(oid.body, OID_SKI, sizeof(OID_SKI))) {
            if (ext_ski(value.body, &out->ski) != DER_OK)
                return DER_ERR;
        } else if (der_oid_is(oid.body, OID_AKI, sizeof(OID_AKI))) {
            if (ext_aki(value.body, &out->aki) != DER_OK)
                return DER_ERR;
        }
    }
}

int x509_parse(der_span in, x509_cert *out)
{
    der_tlv cert, tbs, t;
    int rc;

    if (out == (x509_cert *)0)
        return DER_ERR;
    memset(out, 0, sizeof(*out));

    if (der_expect(&in, DER_SEQUENCE, &cert) != DER_OK ||
        der_expect(&cert.body, DER_SEQUENCE, &tbs) != DER_OK)
        return DER_ERR;
    out->der = cert.whole;

    /* TBSCertificate ::= SEQUENCE { version [0] EXPLICIT DEFAULT v1,
     * serialNumber, signature, issuer, validity, subject,
     * subjectPublicKeyInfo, issuerUniqueID [1] OPTIONAL,
     * subjectUniqueID [2] OPTIONAL, extensions [3] EXPLICIT OPTIONAL } */
    if (der_optional(&tbs.body, DER_CTXC(0), &t) == DER_ERR)
        return DER_ERR;
    if (der_expect(&tbs.body, DER_INTEGER, &t) != DER_OK)
        return DER_ERR;
    out->serial = t.body;
    if (der_expect(&tbs.body, DER_SEQUENCE, &t) != DER_OK)       /* signature */
        return DER_ERR;
    if (der_expect(&tbs.body, DER_SEQUENCE, &t) != DER_OK)
        return DER_ERR;
    out->issuer = t.whole;
    if (der_expect(&tbs.body, DER_SEQUENCE, &t) != DER_OK)       /* validity */
        return DER_ERR;
    if (der_expect(&tbs.body, DER_SEQUENCE, &t) != DER_OK)
        return DER_ERR;
    out->subject = t.whole;
    if (der_expect(&tbs.body, DER_SEQUENCE, &t) != DER_OK)       /* SPKI */
        return DER_ERR;
    if (der_optional(&tbs.body, DER_CTX(1), &t) == DER_ERR ||
        der_optional(&tbs.body, DER_CTX(2), &t) == DER_ERR)
        return DER_ERR;
    rc = der_optional(&tbs.body, DER_CTXC(3), &t);
    if (rc == DER_ERR)
        return DER_ERR;
    if (rc == 1 && parse_extensions(t.body, out) != DER_OK)
        return DER_ERR;
    return DER_OK;
}

/* Copies one directory string as ASCII. BMPString is UCS-2 big-endian; every
 * other string type here is single-byte. */
static size_t put_string(const der_tlv *v, char *out, size_t cap)
{
    size_t n = 0, i;
    size_t step = (v->tag == DER_BMP) ? 2u : 1u;

    for (i = 0; i + step <= v->body.len && n + 1 < cap; i += step) {
        unsigned c = (step == 2u)
                         ? ((unsigned)v->body.p[i] << 8) | v->body.p[i + 1u]
                         : v->body.p[i];

        out[n++] = (c >= 0x20u && c <= 0x7eu) ? (char)c : '?';
    }
    out[n] = '\0';
    return n;
}

size_t x509_cn(der_span name, char *out, size_t cap)
{
    der_tlv seq;
    size_t n = 0;

    if (out == (char *)0 || cap == 0)
        return 0;
    out[0] = '\0';
    if (der_expect(&name, DER_SEQUENCE, &seq) != DER_OK)
        return 0;

    /* Name ::= SEQUENCE OF RelativeDistinguishedName (a SET OF
     * AttributeTypeAndValue). The LAST CN wins, the order openssl prints and
     * the most specific in the usual C, O, OU, CN layout. */
    for (;;) {
        der_tlv rdn;
        int rc = der_read(&seq.body, &rdn);

        if (rc == DER_END)
            return n;
        if (rc != DER_OK || rdn.tag != DER_SET)
            return 0;
        for (;;) {
            der_tlv atv, oid, val;

            rc = der_read(&rdn.body, &atv);
            if (rc == DER_END)
                break;
            if (rc != DER_OK || atv.tag != DER_SEQUENCE ||
                der_expect(&atv.body, DER_OID, &oid) != DER_OK ||
                der_read(&atv.body, &val) != DER_OK)
                return 0;
            if (der_oid_is(oid.body, OID_CN, sizeof(OID_CN)))
                n = put_string(&val, out, cap);
        }
    }
}

int x509_reaches(const x509_cert *c, const x509_cert *a)
{
    if (c == (const x509_cert *)0 || a == (const x509_cert *)0)
        return 0;
    if (der_eq(c->der, a->der))
        return 1;
    if (der_eq(c->issuer, c->subject) || !der_eq(a->subject, c->issuer))
        return 0;
    if (c->aki.len != 0 && a->ski.len != 0)
        return der_eq(c->aki, a->ski);
    return 1;
}
