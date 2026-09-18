#include <secboot/authenticode.h>

#include <crypto/sha256.h>
#include <lib/string.h>
#include <pe/pe.h>

/* WIN_CERTIFICATE: dwLength, wRevision, wCertificateType, then the blob.
 * Entries are padded to eight bytes. */
#define WIN_CERT_HDR            8u
#define WIN_CERT_TYPE_PKCS7     0x0002u

/* 1.2.840.113549.1.7.2 signedData, 1.3.6.1.4.1.311.2.1.4
 * SPC_INDIRECT_DATA_OBJID */
static const uint8_t OID_SIGNED_DATA[] = {
    0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02
};
static const uint8_t OID_SPC_INDIRECT[] = {
    0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x04
};

/* ---- layout ---- */

int ac_layout_read(const void *head, size_t len, uint64_t total,
                   ac_layout *out)
{
    pe_hdr h;
    uint32_t sec_rva, sec_size;
    size_t sbat_off, sbat_len;
    uint16_t i;
    int rc;

    if (head == (const void *)0 || out == (ac_layout *)0)
        return AC_ERR_BAD_ARG;
    memset(out, 0, sizeof(*out));
    out->size = total;
    if ((uint64_t)len > total)
        return AC_ERR_BAD_ARG;

    if (pe_parse(head, len, &h) != PE_OK)
        return AC_ERR_FORMAT;
    rc = pe_checksum_off(&h, &out->checksum_off);
    if (rc == PE_OK)
        rc = pe_size_of_headers(&h, head, &out->size_of_headers);
    /* Shim refuses an image with no security directory entry at all ("Image
     * header too small"), so there is no hash of one to predict with. */
    if (rc == PE_OK)
        rc = pe_dir(&h, head, PE_DIR_SECURITY, &out->secdir_off, &sec_rva,
                    &sec_size);
    if (rc == PE_ERR_SHORT)
        return AC_ERR_SHORT;
    if (rc != PE_OK)
        return AC_ERR_FORMAT;

    /* The three header ranges must be in order and inside SizeOfHeaders,
     * which must be inside the file. */
    if ((uint64_t)out->size_of_headers > total ||
        out->checksum_off + 4u > out->secdir_off ||
        out->secdir_off + 8u > out->size_of_headers)
        return AC_ERR_FORMAT;

    if (sec_size != 0) {
        if ((uint64_t)sec_rva + sec_size > total)
            return AC_ERR_FORMAT;
        out->certs_off = sec_rva;
        out->certs_len = sec_size;
    }

    if (h.nsect > AC_SECT_MAX)
        return AC_ERR_FORMAT;
    for (i = 0; i < h.nsect; i++) {
        pe_sect s;
        uint16_t j;

        /* i < nsect and the buffer is valid, so "past the window" is the
         * only way this can fail. */
        if (pe_section(&h, head, i, &s) != PE_OK)
            return AC_ERR_SHORT;
        if (s.raw_size == 0)
            continue;
        if ((uint64_t)s.raw_ptr + s.raw_size > total)
            return AC_ERR_FORMAT;
        /* Insertion by PointerToRawData, as shim sorts them; equal offsets
         * keep table order. */
        j = out->nsect;
        while (j > 0 && out->sect[j - 1u].off > s.raw_ptr) {
            out->sect[j] = out->sect[j - 1u];
            j--;
        }
        out->sect[j].off = s.raw_ptr;
        out->sect[j].len = s.raw_size;
        out->nsect++;
    }

    if (pe_find_section(&h, head, (size_t)total, ".sbat", &sbat_off,
                        &sbat_len) == PE_OK) {
        out->sbat_off = (uint32_t)sbat_off;
        out->sbat_len = (uint32_t)sbat_len;
    }
    return AC_OK;
}

/* ---- hash ---- */

static int hash_range(sha256_ctx *c, ac_read_fn read, void *ctx,
                      uint64_t off, uint64_t len, uint8_t *scratch,
                      size_t scratch_len)
{
    while (len > 0) {
        size_t n = (len < scratch_len) ? (size_t)len : scratch_len;

        if (read(ctx, off, n, scratch) != 0)
            return AC_ERR_IO;
        sha256_update(c, scratch, n);
        off += n;
        len -= n;
    }
    return AC_OK;
}

int ac_hash(const ac_layout *l, ac_read_fn read, void *ctx, uint8_t *scratch,
            size_t scratch_len, uint8_t out[32])
{
    sha256_ctx c;
    uint64_t sum;
    uint16_t i;
    int rc;

    if (l == (const ac_layout *)0 || read == (ac_read_fn)0 ||
        scratch == (uint8_t *)0 || scratch_len == 0 || out == (uint8_t *)0)
        return AC_ERR_BAD_ARG;

    sha256_init(&c);
    /* Start to CheckSum; CheckSum to the security entry; the security entry
     * to the end of the headers. */
    rc = hash_range(&c, read, ctx, 0, l->checksum_off, scratch, scratch_len);
    if (rc == AC_OK)
        rc = hash_range(&c, read, ctx, l->checksum_off + 4u,
                        l->secdir_off - (l->checksum_off + 4u), scratch,
                        scratch_len);
    if (rc == AC_OK)
        rc = hash_range(&c, read, ctx, l->secdir_off + 8u,
                        l->size_of_headers - (l->secdir_off + 8u), scratch,
                        scratch_len);

    sum = l->size_of_headers;
    for (i = 0; rc == AC_OK && i < l->nsect; i++) {
        rc = hash_range(&c, read, ctx, l->sect[i].off, l->sect[i].len,
                        scratch, scratch_len);
        sum += l->sect[i].len;
    }
    if (rc != AC_OK)
        return rc;

    /* Shim's tail rule (see the header): only a signed image has its tail
     * hashed, from SumOfBytesHashed up to the certificate table, which must
     * begin exactly there. Anything else is a binary shim refuses outright. */
    if (l->size > sum && l->certs_len != 0) {
        if (l->size - sum < l->certs_len ||
            l->size - l->certs_len != l->certs_off)
            return AC_ERR_FORMAT;
        rc = hash_range(&c, read, ctx, sum, l->size - l->certs_len - sum,
                        scratch, scratch_len);
        if (rc != AC_OK)
            return rc;
    }
    sha256_final(&c, out);
    return AC_OK;
}

/* ---- certificate table ---- */

int ac_next_signature(der_span *iter, der_span *blob)
{
    if (iter == (der_span *)0 || blob == (der_span *)0)
        return DER_ERR;
    for (;;) {
        uint32_t dw;
        uint16_t type;
        size_t step;

        if (iter->len < WIN_CERT_HDR)
            return 0;   /* padding, or the end */
        dw = get_le32(iter->p);
        type = get_le16(iter->p + 6);
        if (dw < WIN_CERT_HDR || dw > iter->len)
            return DER_ERR;
        step = ((size_t)dw + 7u) & ~(size_t)7u;
        if (step > iter->len)
            step = iter->len;   /* the last entry need not be padded */

        blob->p = iter->p + WIN_CERT_HDR;
        blob->len = dw - WIN_CERT_HDR;
        iter->p += step;
        iter->len -= step;
        if (type == WIN_CERT_TYPE_PKCS7)
            return 1;
    }
}

/* SignerInfo ::= SEQUENCE { version, sid SignerIdentifier, ... } where sid is
 * IssuerAndSerialNumber (version 1, what Authenticode uses) or [0]
 * SubjectKeyIdentifier (version 3). */
static int parse_signer(der_span infos, ac_signed *out)
{
    der_tlv set, si, t, name, serial;
    int rc;

    if (der_expect(&infos, DER_SET, &set) != DER_OK ||
        der_expect(&set.body, DER_SEQUENCE, &si) != DER_OK ||
        der_expect(&si.body, DER_INTEGER, &t) != DER_OK)
        return DER_ERR;
    rc = der_optional(&si.body, DER_CTX(0), &t);
    if (rc == DER_ERR)
        return DER_ERR;
    if (rc == 1) {
        out->signer_ski = t.body;
        return DER_OK;
    }
    if (der_expect(&si.body, DER_SEQUENCE, &t) != DER_OK ||
        der_expect(&t.body, DER_SEQUENCE, &name) != DER_OK ||
        der_expect(&t.body, DER_INTEGER, &serial) != DER_OK)
        return DER_ERR;
    out->signer_issuer = name.whole;
    out->signer_serial = serial.body;
    return DER_OK;
}

/* contentInfo ::= SEQUENCE { SPC_INDIRECT_DATA_OBJID, [0] EXPLICIT
 * SpcIndirectDataContent ::= SEQUENCE { data, messageDigest DigestInfo } } */
static int parse_indirect(der_span content, ac_signed *out)
{
    der_tlv oid, ex, spc, t, di, alg, aoid, dig;

    if (der_expect(&content, DER_OID, &oid) != DER_OK ||
        !der_oid_is(oid.body, OID_SPC_INDIRECT, sizeof(OID_SPC_INDIRECT)) ||
        der_expect(&content, DER_CTXC(0), &ex) != DER_OK ||
        der_expect(&ex.body, DER_SEQUENCE, &spc) != DER_OK ||
        der_expect(&spc.body, DER_SEQUENCE, &t) != DER_OK ||
        der_expect(&spc.body, DER_SEQUENCE, &di) != DER_OK ||
        der_expect(&di.body, DER_SEQUENCE, &alg) != DER_OK ||
        der_expect(&alg.body, DER_OID, &aoid) != DER_OK ||
        der_expect(&di.body, DER_OCTETS, &dig) != DER_OK)
        return DER_ERR;
    out->digest_oid = aoid.body;
    out->digest = dig.body;
    return DER_OK;
}

int ac_signed_parse(der_span blob, ac_signed *out)
{
    der_tlv ci, oid, ex, sd, t;
    int rc;

    if (out == (ac_signed *)0)
        return DER_ERR;
    memset(out, 0, sizeof(*out));

    /* ContentInfo ::= SEQUENCE { signedData OID, [0] EXPLICIT SignedData } */
    if (der_expect(&blob, DER_SEQUENCE, &ci) != DER_OK ||
        der_expect(&ci.body, DER_OID, &oid) != DER_OK ||
        !der_oid_is(oid.body, OID_SIGNED_DATA, sizeof(OID_SIGNED_DATA)) ||
        der_expect(&ci.body, DER_CTXC(0), &ex) != DER_OK ||
        der_expect(&ex.body, DER_SEQUENCE, &sd) != DER_OK)
        return DER_ERR;

    /* SignedData ::= SEQUENCE { version, digestAlgorithms SET,
     * contentInfo, certificates [0] IMPLICIT OPTIONAL,
     * crls [1] IMPLICIT OPTIONAL, signerInfos SET } */
    if (der_expect(&sd.body, DER_INTEGER, &t) != DER_OK ||
        der_expect(&sd.body, DER_SET, &t) != DER_OK ||
        der_expect(&sd.body, DER_SEQUENCE, &t) != DER_OK ||
        parse_indirect(t.body, out) != DER_OK)
        return DER_ERR;
    rc = der_optional(&sd.body, DER_CTXC(0), &t);
    if (rc == DER_ERR)
        return DER_ERR;
    if (rc == 1)
        out->certs = t.body;
    if (der_optional(&sd.body, DER_CTXC(1), &t) == DER_ERR)
        return DER_ERR;
    return parse_signer(sd.body, out);
}

int ac_next_cert(der_span *iter, x509_cert *out)
{
    der_tlv t;
    int rc;

    if (iter == (der_span *)0 || out == (x509_cert *)0)
        return DER_ERR;
    rc = der_read(iter, &t);
    if (rc == DER_END)
        return 0;
    if (rc != DER_OK || x509_parse(t.whole, out) != DER_OK)
        return DER_ERR;
    return 1;
}

int ac_signer_cert(const ac_signed *s, x509_cert *out)
{
    der_span it;

    if (s == (const ac_signed *)0 || out == (x509_cert *)0)
        return DER_ERR;
    it = s->certs;
    while (ac_next_cert(&it, out) == 1) {
        if (s->signer_ski.len != 0) {
            if (der_eq(out->ski, s->signer_ski))
                return DER_OK;
        } else if (der_eq(out->issuer, s->signer_issuer) &&
                   der_eq(out->serial, s->signer_serial)) {
            return DER_OK;
        }
    }
    return DER_ERR;
}

int ac_issuer_cert(const ac_signed *s, const x509_cert *c, x509_cert *out)
{
    der_span it;

    if (s == (const ac_signed *)0 || c == (const x509_cert *)0 ||
        out == (x509_cert *)0)
        return DER_ERR;
    if (der_eq(c->issuer, c->subject))
        return DER_ERR;     /* self-issued: the top of the chain */
    it = s->certs;
    while (ac_next_cert(&it, out) == 1) {
        if (der_eq(out->der, c->der) || !der_eq(out->subject, c->issuer))
            continue;
        if (c->aki.len != 0 && out->ski.len != 0 &&
            !der_eq(c->aki, out->ski))
            continue;
        return DER_OK;
    }
    return DER_ERR;
}
