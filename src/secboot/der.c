#include <secboot/der.h>

#include <lib/string.h>

int der_read(der_span *in, der_tlv *out)
{
    const uint8_t *p;
    size_t avail, hdr, n;
    uint8_t first;

    if (in == (der_span *)0 || out == (der_tlv *)0)
        return DER_ERR;
    if (in->len == 0)
        return DER_END;
    if (in->p == (const uint8_t *)0 || in->len < 2u)
        return DER_ERR;

    p = in->p;
    avail = in->len;
    if ((p[0] & 0x1fu) == 0x1fu)
        return DER_ERR;     /* high-tag-number form */

    first = p[1];
    if (first < 0x80u) {
        n = first;
        hdr = 2u;
    } else {
        size_t k = first & 0x7fu;
        size_t i;

        /* 0x80 is indefinite length (BER only); more than four octets would
         * describe an element larger than anything this reader is shown. */
        if (k == 0 || k > 4u || avail - 2u < k)
            return DER_ERR;
        n = 0;
        for (i = 0; i < k; i++)
            n = (n << 8) | p[2u + i];
        hdr = 2u + k;
    }
    if (n > avail - hdr)
        return DER_ERR;

    out->tag = p[0];
    out->body.p = p + hdr;
    out->body.len = n;
    out->whole.p = p;
    out->whole.len = hdr + n;
    in->p += hdr + n;
    in->len -= hdr + n;
    return DER_OK;
}

int der_expect(der_span *in, uint8_t tag, der_tlv *out)
{
    der_span save;
    int rc;

    if (in == (der_span *)0)
        return DER_ERR;
    save = *in;
    rc = der_read(in, out);
    if (rc != DER_OK)
        return DER_ERR;
    if (out->tag != tag) {
        *in = save;
        return DER_ERR;
    }
    return DER_OK;
}

int der_optional(der_span *in, uint8_t tag, der_tlv *out)
{
    if (in == (der_span *)0 || out == (der_tlv *)0)
        return DER_ERR;
    if (in->len == 0 || in->p[0] != tag)
        return 0;
    return der_read(in, out) == DER_OK ? 1 : DER_ERR;
}

int der_eq(der_span a, der_span b)
{
    if (a.len != b.len)
        return 0;
    if (a.len == 0)
        return 1;
    return memcmp(a.p, b.p, a.len) == 0;
}

int der_oid_is(der_span body, const uint8_t *oid, size_t oid_len)
{
    der_span want;

    want.p = oid;
    want.len = oid_len;
    return der_eq(body, want);
}
