/* Boot predictions (src/secboot: der, x509, authenticode, siglist, trust;
 * DESIGN Section 7.8).
 *
 * Three kinds of test:
 *
 *   - Directed ones over hand-built bytes for the DER reader, the signature
 *     list walk and the SBAT rule, where each malformation is one field.
 *   - The verdict table, over certificates and images
 *     falcon-core/tools/mk-test-trust.sh built with known relationships. Each
 *     row is a (stores, image) pair and the answer shim would give.
 *   - An oracle and a sweep. sbsign computed a digest for every image it
 *     signed; ac_hash must reproduce it exactly, or the hash here is not the
 *     one shim checks against dbx. And every byte of a real certificate table
 *     is flipped in turn to show that the parser never crashes and never
 *     turns junk into a trusted verdict.
 *
 * The real-world group runs a distro shim against the Microsoft certificates
 * from the secboot OVMF varstore when the rig has both.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include <crypto/sha256.h>
#include <trust/authenticode.h>
#include <trust/der.h>
#include <trust/probe.h>
#include <trust/siglist.h>
#include <trust/trust.h>
#include <trust/x509.h>
#include "tests.h"

/* ---- fixtures ---- */

typedef struct {
    uint8_t *p;
    size_t len;
} blob;

static blob load(const char *dir, const char *name)
{
    char path[512];
    blob b = { NULL, 0 };
    FILE *f;
    long n;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f == NULL)
        return b;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.p = malloc((size_t)n + 1u);
    assert_non_null(b.p);
    b.len = fread(b.p, 1, (size_t)n, f);
    fclose(f);
    return b;
}

static blob need(const char *name)
{
    blob b = load(TRUST_DIR, name);

    assert_non_null(b.p);
    return b;
}

static der_span span(blob b)
{
    der_span s;

    s.p = b.p;
    s.len = b.len;
    return s;
}

static int mem_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const blob *b = ctx;

    if (off > b->len || len > b->len - off)
        return -1;
    memcpy(buf, b->p + off, (size_t)len);
    return 0;
}

/* An image as the loader would present it: layout, certificate table,
 * .sbat, and the hash. */
typedef struct {
    blob       file;
    ac_layout  l;
    uint8_t    hash[32];
    trust_image im;
} image;

static void image_load(image *img, blob file)
{
    static uint8_t scratch[4096];

    memset(img, 0, sizeof(*img));
    img->file = file;
    assert_int_equal(ac_layout_read(file.p, 4096, file.len, &img->l), AC_OK);
    assert_int_equal(ac_hash(&img->l, mem_read, &img->file, scratch,
                             sizeof(scratch), img->hash), AC_OK);
    img->im.certs.p = file.p + img->l.certs_off;
    img->im.certs.len = img->l.certs_len;
    img->im.sbat.p = file.p + img->l.sbat_off;
    img->im.sbat.len = img->l.sbat_len;
    img->im.hash = img->hash;
}

/* ---- building stores ---- */

static const uint8_t G_X509[16] = {
    0xa1, 0x59, 0xc0, 0xa5, 0xe4, 0x94, 0xa7, 0x4a,
    0x87, 0xb5, 0xab, 0x15, 0x5c, 0x2b, 0xf0, 0x72
};
static const uint8_t G_SHA256[16] = {
    0x26, 0x16, 0xc4, 0xc1, 0x4c, 0x50, 0x92, 0x40,
    0xac, 0xa9, 0x41, 0xf9, 0x36, 0x93, 0x43, 0x28
};

typedef struct {
    uint8_t buf[32768];
    size_t len;
} store;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* One EFI_SIGNATURE_LIST holding one entry. */
static void store_add(store *s, const uint8_t guid[16], const void *data,
                      size_t len)
{
    uint8_t *p = s->buf + s->len;
    uint32_t sig = (uint32_t)(16u + len);

    assert_true(s->len + 28u + sig <= sizeof(s->buf));
    memcpy(p, guid, 16);
    put32(p + 16, 28u + sig);
    put32(p + 20, 0);
    put32(p + 24, sig);
    memset(p + 28, 0x5a, 16);                   /* owner GUID */
    memcpy(p + 44, data, len);
    s->len += 28u + sig;
}

static void store_cert(store *s, const char *name)
{
    blob b = need(name);

    store_add(s, G_X509, b.p, b.len);
    free(b.p);
}

static der_span sspan(const store *s)
{
    der_span d;

    d.p = s->buf;
    d.len = s->len;
    return d;
}

/* ---- DER ---- */

static void test_der_reader(void **state)
{
    static const uint8_t ok[] = { 0x30, 0x03, 0x02, 0x01, 0x05, 0xff };
    static const uint8_t longform[] = { 0x04, 0x81, 0x02, 0xaa, 0xbb };
    static const uint8_t hightag[] = { 0x1f, 0x01, 0x00 };
    static const uint8_t indef[] = { 0x30, 0x80, 0x00, 0x00 };
    static const uint8_t fivelen[] = { 0x04, 0x85, 0, 0, 0, 0, 1, 0 };
    static const uint8_t overrun[] = { 0x04, 0x05, 0x01 };
    static const uint8_t lenbytes_short[] = { 0x04, 0x82, 0x01 };
    der_span s;
    der_tlv t;

    (void)state;
    s.p = ok; s.len = sizeof(ok);
    assert_int_equal(der_read(&s, &t), DER_OK);
    assert_int_equal(t.tag, DER_SEQUENCE);
    assert_int_equal(t.body.len, 3);
    assert_int_equal(t.whole.len, 5);
    assert_int_equal(s.len, 1);
    /* A single trailing byte is not an element. */
    assert_int_equal(der_read(&s, &t), DER_ERR);
    s.len = 0;
    assert_int_equal(der_read(&s, &t), DER_END);

    s.p = longform; s.len = sizeof(longform);
    assert_int_equal(der_read(&s, &t), DER_OK);
    assert_int_equal(t.body.len, 2);

    s.p = hightag; s.len = sizeof(hightag);
    assert_int_equal(der_read(&s, &t), DER_ERR);
    s.p = indef; s.len = sizeof(indef);
    assert_int_equal(der_read(&s, &t), DER_ERR);
    s.p = fivelen; s.len = sizeof(fivelen);
    assert_int_equal(der_read(&s, &t), DER_ERR);
    s.p = overrun; s.len = sizeof(overrun);
    assert_int_equal(der_read(&s, &t), DER_ERR);
    assert_int_equal(s.len, sizeof(overrun));      /* not advanced */
    s.p = lenbytes_short; s.len = sizeof(lenbytes_short);
    assert_int_equal(der_read(&s, &t), DER_ERR);

    assert_int_equal(der_read(NULL, &t), DER_ERR);
    s.p = NULL; s.len = 4;
    assert_int_equal(der_read(&s, &t), DER_ERR);
}

static void test_der_expect_optional(void **state)
{
    static const uint8_t two[] = { 0x02, 0x01, 0x07, 0x04, 0x00 };
    der_span s;
    der_tlv t;

    (void)state;
    s.p = two; s.len = sizeof(two);
    /* A tag mismatch leaves the span where it was. */
    assert_int_equal(der_expect(&s, DER_OCTETS, &t), DER_ERR);
    assert_int_equal(s.len, sizeof(two));
    assert_int_equal(der_optional(&s, DER_OCTETS, &t), 0);
    assert_int_equal(der_optional(&s, DER_INTEGER, &t), 1);
    assert_int_equal(der_expect(&s, DER_OCTETS, &t), DER_OK);
    assert_int_equal(t.body.len, 0);
    assert_int_equal(der_optional(&s, DER_OCTETS, &t), 0);    /* empty */
    assert_int_equal(der_expect(&s, DER_OCTETS, &t), DER_ERR);
    assert_int_equal(der_expect(NULL, DER_OCTETS, &t), DER_ERR);
    assert_int_equal(der_optional(NULL, DER_OCTETS, &t), DER_ERR);

    /* Optional, and present, but malformed. */
    {
        static const uint8_t bad[] = { 0x04, 0x09, 0x00 };

        s.p = bad; s.len = sizeof(bad);
        assert_int_equal(der_optional(&s, DER_OCTETS, &t), DER_ERR);
    }
}

/* ---- X.509 ---- */

static void test_x509_fields(void **state)
{
    blob root = need("root.der"), inter = need("inter.der"),
         leaf = need("leaf.der"), dba = need("dba.der");
    x509_cert r, i, l, d;
    char cn[64];

    (void)state;
    assert_int_equal(x509_parse(span(root), &r), DER_OK);
    assert_int_equal(x509_parse(span(inter), &i), DER_OK);
    assert_int_equal(x509_parse(span(leaf), &l), DER_OK);
    assert_int_equal(x509_parse(span(dba), &d), DER_OK);

    assert_int_equal(r.der.len, root.len);
    assert_true(r.ski.len > 0);
    assert_true(der_eq(i.aki, r.ski));
    assert_true(der_eq(l.aki, i.ski));
    assert_true(der_eq(l.issuer, i.subject));
    /* sbctl's shape: no extensions, so neither identifier. */
    assert_int_equal(d.ski.len, 0);
    assert_int_equal(d.aki.len, 0);
    assert_true(der_eq(d.issuer, d.subject));

    assert_int_equal(x509_cn(l.subject, cn, sizeof(cn)), 18);
    assert_string_equal(cn, "Falcon Test Signer");
    assert_int_equal(x509_cn(l.subject, cn, 7), 6);
    assert_string_equal(cn, "Falcon");
    assert_int_equal(x509_cn(l.subject, NULL, 8), 0);

    free(root.p); free(inter.p); free(leaf.p); free(dba.p);
}

static void test_x509_reaches(void **state)
{
    blob root = need("root.der"), inter = need("inter.der"),
         leaf = need("leaf.der"), dba = need("dba.der"), dbb = need("dbb.der");
    x509_cert r, i, l, a, b;

    (void)state;
    assert_int_equal(x509_parse(span(root), &r), DER_OK);
    assert_int_equal(x509_parse(span(inter), &i), DER_OK);
    assert_int_equal(x509_parse(span(leaf), &l), DER_OK);
    assert_int_equal(x509_parse(span(dba), &a), DER_OK);
    assert_int_equal(x509_parse(span(dbb), &b), DER_OK);

    assert_true(x509_reaches(&l, &i));
    assert_true(x509_reaches(&i, &r));
    assert_false(x509_reaches(&l, &r));
    assert_true(x509_reaches(&l, &l));
    /* The case this rule exists for: same Name, no identifiers, other key. */
    assert_true(der_eq(a.subject, b.subject));
    assert_false(x509_reaches(&a, &b));
    assert_true(x509_reaches(&a, &a));
    assert_false(x509_reaches(NULL, &a));

    /* An intermediate whose AKI does not match the anchor's SKI. */
    {
        x509_cert fake = r;

        fake.der.len--;
        fake.ski = l.ski;
        assert_false(x509_reaches(&i, &fake));
    }

    free(root.p); free(inter.p); free(leaf.p); free(dba.p); free(dbb.p);
}

static void test_x509_refusals(void **state)
{
    blob leaf = need("leaf.der");
    x509_cert c;
    size_t cut;

    (void)state;
    assert_int_equal(x509_parse(span(leaf), NULL), DER_ERR);
    /* Every truncation is refused, none crashes. */
    for (cut = 0; cut < leaf.len; cut++) {
        der_span s = span(leaf);

        s.len = cut;
        assert_int_equal(x509_parse(s, &c), DER_ERR);
    }
    free(leaf.p);
}

/* A Name with a BMPString CN, and one with a control character. */
static void test_x509_cn_strings(void **state)
{
    static const uint8_t bmp[] = {
        0x30, 0x13, 0x31, 0x11, 0x30, 0x0f, 0x06, 0x03, 0x55, 0x04, 0x03,
        0x1e, 0x08, 0x00, 'D', 0x00, 'b', 0x20, 0xac, 0x00, 0x01
    };
    static const uint8_t nocn[] = {
        0x30, 0x0d, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06,
        0x13, 0x02, 'U', 'S'
    };
    der_span s;
    char cn[16];

    (void)state;
    s.p = bmp; s.len = sizeof(bmp);
    assert_int_equal(x509_cn(s, cn, sizeof(cn)), 4);
    assert_string_equal(cn, "Db??");
    s.p = nocn; s.len = sizeof(nocn);
    assert_int_equal(x509_cn(s, cn, sizeof(cn)), 0);
    assert_string_equal(cn, "");
}

/* ---- Authenticode ---- */

/* sbsign's digest is the oracle: every signature in every signed fixture must
 * carry exactly the hash computed here. */
static void test_hash_matches_sbsign(void **state)
{
    static const char *const names[] = {
        "leaf.efi", "leaf-bare.efi", "dba.efi", "dbb.efi", "mok.efi",
        "dual.efi"
    };
    size_t n;

    (void)state;
    for (n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        image img;
        der_span it, b;
        int sigs = 0;

        image_load(&img, need(names[n]));
        it = img.im.certs;
        while (ac_next_signature(&it, &b) == 1) {
            ac_signed s;

            assert_int_equal(ac_signed_parse(b, &s), DER_OK);
            assert_int_equal(s.digest.len, 32);
            if (memcmp(s.digest.p, img.hash, 32) != 0)
                fail_msg("%s: signature %d digest differs from ac_hash",
                         names[n], sigs);
            sigs++;
        }
        assert_int_equal(sigs, strcmp(names[n], "dual.efi") == 0 ? 2 : 1);
        free(img.file.p);
    }
}

static void test_layout(void **state)
{
    blob u = need("unsigned.efi"), l = need("leaf.efi");
    ac_layout a;
    uint8_t scratch[64], h1[32], h2[32];

    (void)state;
    assert_int_equal(ac_layout_read(u.p, 4096, u.len, &a), AC_OK);
    assert_int_equal(a.certs_len, 0);
    assert_true(a.sbat_len > 0);
    assert_memory_equal(u.p + a.sbat_off, "sbat,1,", 7);
    assert_true(a.nsect > 0);

    /* A window too short for the section table. */
    assert_int_equal(ac_layout_read(u.p, 300, u.len, &a), AC_ERR_SHORT);
    assert_int_equal(ac_layout_read(u.p, 64, u.len, &a), AC_ERR_FORMAT);
    assert_int_equal(ac_layout_read(u.p, 4096, 100, &a), AC_ERR_BAD_ARG);
    assert_int_equal(ac_layout_read(NULL, 4096, u.len, &a), AC_ERR_BAD_ARG);

    /* A small scratch buffer changes the reads, not the hash. */
    assert_int_equal(ac_layout_read(l.p, 4096, l.len, &a), AC_OK);
    {
        blob ctx = l;
        static uint8_t big[65536];

        assert_int_equal(ac_hash(&a, mem_read, &ctx, scratch, sizeof(scratch),
                                 h1), AC_OK);
        assert_int_equal(ac_hash(&a, mem_read, &ctx, big, sizeof(big), h2),
                         AC_OK);
        assert_memory_equal(h1, h2, 32);
        /* ac_hash is ac_hash_with SHA-256; SHA-1, a TPM 1.2 log's, is the
         * same walk under another hash, so it too is independent of the
         * scratch size. The rig's 1.2 event log is its oracle. */
        assert_int_equal(ac_hash_with(&a, hash_sha256(), mem_read, &ctx,
                                      big, sizeof(big), h2), AC_OK);
        assert_memory_equal(h1, h2, 32);
        assert_int_equal(ac_hash_with(&a, hash_sha1(), mem_read, &ctx,
                                      scratch, sizeof(scratch), h1), AC_OK);
        assert_int_equal(ac_hash_with(&a, hash_sha1(), mem_read, &ctx,
                                      big, sizeof(big), h2), AC_OK);
        assert_memory_equal(h1, h2, 20);
        assert_int_equal(ac_hash_with(&a, NULL, mem_read, &ctx, big,
                                      sizeof(big), h2), AC_ERR_BAD_ARG);
    }

    /* A read failure is reported, not hashed around. */
    {
        blob shortf = l;

        shortf.len = 1000;
        assert_int_equal(ac_hash(&a, mem_read, &shortf, scratch,
                                 sizeof(scratch), h1), AC_ERR_IO);
    }
    assert_int_equal(ac_hash(NULL, mem_read, &l, scratch, 1, h1),
                     AC_ERR_BAD_ARG);

    /* A certificate table that does not end the file is one shim refuses. */
    a.certs_off -= 8;
    assert_int_equal(ac_hash(&a, mem_read, &l, scratch, sizeof(scratch), h1),
                     AC_ERR_FORMAT);

    free(u.p);
    free(l.p);
}

/* The real loader's headers, bent one field at a time. */
static void test_layout_refusals(void **state)
{
    blob u = need("unsigned.efi"), l = need("leaf.efi");
    uint8_t *h = malloc(u.len);
    ac_layout a;
    uint32_t lfanew, opt, table;
    uint16_t nsect;
    uint8_t sect0[40], sect1[40];

    (void)state;
    assert_non_null(h);
    lfanew = (uint32_t)u.p[0x3c] | (uint32_t)u.p[0x3d] << 8;
    opt = lfanew + 24u;
    nsect = (uint16_t)(u.p[lfanew + 6] | u.p[lfanew + 7] << 8);
    table = opt + (uint32_t)(u.p[lfanew + 20] | u.p[lfanew + 21] << 8);
    assert_true(nsect >= 2);

#define FRESH() memcpy(h, u.p, u.len)
    FRESH();
    h[opt] = 0x99;                              /* unknown magic */
    assert_int_equal(ac_layout_read(h, 4096, u.len, &a), AC_ERR_FORMAT);
    FRESH();
    put32(h + opt + 60, (uint32_t)u.len + 1u);  /* SizeOfHeaders past EOF */
    assert_int_equal(ac_layout_read(h, 4096, u.len, &a), AC_ERR_FORMAT);
    FRESH();
    put32(h + opt + 112 + 32, 0x100);           /* certificate table */
    put32(h + opt + 112 + 36, (uint32_t)u.len); /*   running past EOF */
    assert_int_equal(ac_layout_read(h, 4096, u.len, &a), AC_ERR_FORMAT);
    FRESH();
    h[lfanew + 6] = 97;                         /* more sections than allowed */
    h[lfanew + 7] = 0;
    assert_int_equal(ac_layout_read(h, 4096, u.len, &a), AC_ERR_FORMAT);
    FRESH();
    put32(h + table + 20, (uint32_t)u.len);     /* section 0 past EOF */
    assert_int_equal(ac_layout_read(h, 4096, u.len, &a), AC_ERR_FORMAT);
    FRESH();
    assert_int_equal(ac_layout_read(h, table + 40u, u.len, &a), AC_ERR_SHORT);

    /* Sections out of file order are hashed in file order: swapping two
     * section headers changes neither the sorted layout nor the hash. */
    {
        ac_layout sorted;

        FRESH();
        assert_int_equal(ac_layout_read(h, 4096, u.len, &sorted), AC_OK);
        memcpy(sect0, h + table, 40);
        memcpy(sect1, h + table + 40, 40);
        memcpy(h + table, sect1, 40);
        memcpy(h + table + 40, sect0, 40);
        assert_int_equal(ac_layout_read(h, 4096, u.len, &a), AC_OK);
        assert_memory_equal(a.sect, sorted.sect, sizeof(a.sect));
    }
#undef FRESH

    /* A read that fails only in the tail, after every section succeeded. */
    {
        static uint8_t scratch[4096];
        uint8_t out[32];
        blob cut = l;

        assert_int_equal(ac_layout_read(l.p, 4096, l.len, &a), AC_OK);
        cut.len = a.certs_off - 1u;
        assert_int_equal(ac_hash(&a, mem_read, &cut, scratch, sizeof(scratch),
                                 out), AC_ERR_IO);
    }
    free(h);
    free(u.p);
    free(l.p);
}

static void test_signature_walk(void **state)
{
    uint8_t tbl[64];
    der_span it, b;

    (void)state;
    /* A non-PKCS#7 entry is skipped; the PKCS#7 one after it is yielded. */
    memset(tbl, 0, sizeof(tbl));
    put32(tbl, 12);             /* 12 bytes, padded to 16 */
    tbl[4] = 0x00; tbl[5] = 0x02; tbl[6] = 0x01; /* type 1: X.509 */
    put32(tbl + 16, 10);
    tbl[20] = 0x00; tbl[21] = 0x02; tbl[22] = 0x02;
    tbl[24] = 0xab; tbl[25] = 0xcd;
    it.p = tbl; it.len = 26;
    assert_int_equal(ac_next_signature(&it, &b), 1);
    assert_int_equal(b.len, 2);
    assert_int_equal(b.p[0], 0xab);
    assert_int_equal(ac_next_signature(&it, &b), 0);

    /* A length that cannot be. */
    put32(tbl, 4);
    it.p = tbl; it.len = 26;
    assert_int_equal(ac_next_signature(&it, &b), DER_ERR);
    put32(tbl, 200);
    it.p = tbl; it.len = 26;
    assert_int_equal(ac_next_signature(&it, &b), DER_ERR);
    assert_int_equal(ac_next_signature(NULL, &b), DER_ERR);
}

static void test_signer_lookup(void **state)
{
    image img;
    der_span it, b;
    ac_signed s;
    x509_cert c, p;
    char cn[64];

    (void)state;
    image_load(&img, need("leaf.efi"));
    it = img.im.certs;
    assert_int_equal(ac_next_signature(&it, &b), 1);
    assert_int_equal(ac_signed_parse(b, &s), DER_OK);
    assert_int_equal(ac_signer_cert(&s, &c), DER_OK);
    x509_cn(c.subject, cn, sizeof(cn));
    assert_string_equal(cn, "Falcon Test Signer");
    assert_int_equal(ac_issuer_cert(&s, &c, &p), DER_OK);
    x509_cn(p.subject, cn, sizeof(cn));
    assert_string_equal(cn, "Falcon Test Intermediate");
    /* The root is not in the set. */
    assert_int_equal(ac_issuer_cert(&s, &p, &c), DER_ERR);

    /* A signer the set does not carry. */
    s.signer_serial.len = 0;
    assert_int_equal(ac_signer_cert(&s, &c), DER_ERR);
    /* SKI form of the SignerIdentifier. */
    s.signer_ski = p.ski;
    assert_int_equal(ac_signer_cert(&s, &c), DER_OK);
    x509_cn(c.subject, cn, sizeof(cn));
    assert_string_equal(cn, "Falcon Test Intermediate");

    assert_int_equal(ac_signed_parse(b, NULL), DER_ERR);
    assert_int_equal(ac_signer_cert(NULL, &c), DER_ERR);
    assert_int_equal(ac_issuer_cert(&s, NULL, &c), DER_ERR);
    assert_int_equal(ac_next_cert(NULL, &c), DER_ERR);
    free(img.file.p);
}

/* ---- signature lists and SBAT ---- */

static void test_siglist_walk(void **state)
{
    store s;
    uint8_t hash[32], other[32];
    siglist_iter it;
    sig_type t;
    der_span d;
    int n = 0;

    (void)state;
    memset(&s, 0, sizeof(s));
    memset(hash, 0x11, sizeof(hash));
    memset(other, 0x22, sizeof(other));
    store_cert(&s, "root.der");
    store_add(&s, G_SHA256, hash, 32);
    store_add(&s, (const uint8_t *)"0123456789abcdef", "xyz", 3);

    siglist_begin(&it, sspan(&s));
    while (siglist_next(&it, &t, &d) == 1)
        n++;
    assert_int_equal(n, 3);
    assert_true(siglist_has_hash(sspan(&s), hash));
    assert_false(siglist_has_hash(sspan(&s), other));
    assert_false(siglist_has_hash(sspan(&s), NULL));

    /* A SHA256-typed list with the wrong entry size is not a hash list. */
    memset(&s, 0, sizeof(s));
    store_add(&s, G_SHA256, hash, 31);
    assert_false(siglist_has_hash(sspan(&s), hash));

    /* Impossible sizes end the walk, and poison what follows. */
    memset(&s, 0, sizeof(s));
    store_add(&s, G_SHA256, hash, 32);
    put32(s.buf + 24, 7);                       /* SignatureSize <= owner */
    store_add(&s, G_SHA256, hash, 32);
    siglist_begin(&it, sspan(&s));
    assert_int_equal(siglist_next(&it, &t, &d), DER_ERR);
    assert_int_equal(siglist_next(&it, &t, &d), 0);

    memset(&s, 0, sizeof(s));
    store_add(&s, G_SHA256, hash, 32);
    put32(s.buf + 16, 9999);                    /* ListSize past the end */
    assert_false(siglist_has_hash(sspan(&s), hash));
    put32(s.buf + 16, 80);
    put32(s.buf + 20, 60);                      /* header past the list */
    assert_false(siglist_has_hash(sspan(&s), hash));
    put32(s.buf + 20, 0);
    put32(s.buf + 16, 20);                      /* smaller than a header */
    assert_false(siglist_has_hash(sspan(&s), hash));
    put32(s.buf + 16, 81);                      /* entries do not divide */
    assert_false(siglist_has_hash(sspan(&s), hash));

    /* A header too short to read at all. */
    {
        der_span tiny = sspan(&s);

        tiny.len = 10;
        siglist_begin(&it, tiny);
        assert_int_equal(siglist_next(&it, &t, &d), DER_ERR);
    }
    /* An empty list is skipped, not the end. */
    memset(&s, 0, sizeof(s));
    memcpy(s.buf, G_X509, 16);
    put32(s.buf + 16, 28);
    put32(s.buf + 24, 64);
    s.len = 28;
    store_add(&s, G_SHA256, hash, 32);
    assert_true(siglist_has_hash(sspan(&s), hash));

    siglist_begin(NULL, sspan(&s));
    assert_int_equal(siglist_next(NULL, &t, &d), DER_ERR);
    {
        der_span none = { NULL, 12 };

        siglist_begin(&it, none);
        assert_int_equal(siglist_next(&it, &t, &d), 0);
    }
}

static der_span text(const char *s)
{
    der_span d;

    d.p = (const uint8_t *)s;
    d.len = strlen(s);
    return d;
}

static void test_sbat_rule(void **state)
{
    const char *img = "sbat,1,SBAT Version,sbat,1,https://example/\n"
                      "shim,4,UEFI shim,shim,1,https://example/\n";

    (void)state;
    assert_false(sbat_revoked(text("sbat,1,2024010900\nshim,4\n"), text(img)));
    assert_true(sbat_revoked(text("sbat,1,2024010900\nshim,5\n"), text(img)));
    /* CRLF and a trailing NUL, as SbatLevelRT is often stored. */
    {
        static const char lvl[] = "sbat,1,2024\r\nshim,5\r\n";
        der_span l;

        l.p = (const uint8_t *)lvl;
        l.len = sizeof(lvl);
        assert_true(sbat_revoked(l, text(img)));
    }
    /* A different component, or one the image lacks, revokes nothing. */
    assert_false(sbat_revoked(text("grub,9\n"), text(img)));
    assert_false(sbat_revoked(text("shimx,9\n"), text(img)));
    /* Unparseable lines are not revocations. */
    assert_false(sbat_revoked(text("shim,\nshim,x\n,5\nshim\n"), text(img)));
    assert_false(sbat_revoked(text("shim,99999999999\n"), text(img)));
    assert_false(sbat_revoked(text("shim,5\n"), text("shim,4x,foo\n")));
    assert_false(sbat_revoked(text(""), text(img)));
    {
        der_span none = { NULL, 0 };

        assert_false(sbat_revoked(none, text(img)));
        assert_false(sbat_revoked(text("shim,5\n"), none));
    }
}

/* ---- the verdict table ---- */

typedef struct {
    const char *image;
    const char *db[3];
    const char *dbx[2];
    const char *mok[2];
    trust_verdict want;
    trust_list by;
    const char *label;
} row;

static const row ROWS[] = {
    /* Chains, with and without the intermediate carried. */
    { "leaf.efi",      { "root.der" },  { 0 }, { 0 }, TRUST_OK, TRUST_LIST_DB,
      "Falcon Test Root" },
    { "leaf.efi",      { "inter.der" }, { 0 }, { 0 }, TRUST_OK, TRUST_LIST_DB,
      "Falcon Test Intermediate" },
    { "leaf-bare.efi", { "inter.der" }, { 0 }, { 0 }, TRUST_OK, TRUST_LIST_DB,
      "Falcon Test Intermediate" },
    { "leaf-bare.efi", { "root.der" },  { 0 }, { 0 }, TRUST_UNTRUSTED,
      TRUST_LIST_NONE, "Falcon Test Intermediate" },
    { "leaf.efi",      { 0 },           { 0 }, { "root.der" }, TRUST_OK,
      TRUST_LIST_MOK, "Falcon Test Root" },

    /* The Dell: two sbctl keys, same Name, no identifiers. */
    { "dba.efi", { "dbb.der" }, { 0 }, { 0 }, TRUST_UNTRUSTED,
      TRUST_LIST_NONE, "Database Key" },
    { "dba.efi", { "dba.der" }, { 0 }, { 0 }, TRUST_OK, TRUST_LIST_DB,
      "Database Key" },
    { "dbb.efi", { "dba.der", "dbb.der" }, { 0 }, { 0 }, TRUST_OK,
      TRUST_LIST_DB, "Database Key" },

    /* Two signatures: either passing is enough, any revoked is fatal. */
    { "dual.efi", { "dbb.der" }, { 0 }, { 0 }, TRUST_UNTRUSTED,
      TRUST_LIST_NONE, "Database Key" },
    { "dual.efi", { "root.der" }, { 0 }, { 0 }, TRUST_OK, TRUST_LIST_DB,
      "Falcon Test Root" },
    { "dual.efi", { "dba.der" }, { "inter.der" }, { 0 }, TRUST_REVOKED,
      TRUST_LIST_DBX, "Falcon Test Root" },
    { "leaf.efi", { "root.der" }, { "leaf.der" }, { 0 }, TRUST_REVOKED,
      TRUST_LIST_DBX, "Falcon Test Root" },

    /* Unsigned. */
    { "unsigned.efi", { "root.der" }, { 0 }, { 0 }, TRUST_UNSIGNED,
      TRUST_LIST_NONE, "unsigned" },
};

static void load_store(store *s, const char *const *names, size_t n)
{
    size_t i;

    memset(s, 0, sizeof(*s));
    for (i = 0; i < n && names[i] != NULL; i++)
        store_cert(s, names[i]);
}

static void test_verdict_table(void **state)
{
    static store db, dbx, mok;
    size_t r;

    (void)state;
    for (r = 0; r < sizeof(ROWS) / sizeof(ROWS[0]); r++) {
        const row *w = &ROWS[r];
        trust_stores st;
        trust_result res;
        image img;

        memset(&st, 0, sizeof(st));
        load_store(&db, w->db, 3);
        load_store(&dbx, w->dbx, 2);
        load_store(&mok, w->mok, 2);
        st.db = sspan(&db);
        st.dbx = sspan(&dbx);
        st.mok = sspan(&mok);
        image_load(&img, need(w->image));
        trust_evaluate(&st, &img.im, &res);
        if (res.verdict != w->want || res.by != w->by ||
            strcmp(res.label, w->label) != 0)
            fail_msg("row %zu (%s): verdict %d by %d label '%s', want %d by "
                     "%d '%s'", r, w->image, res.verdict, res.by, res.label,
                     w->want, w->by, w->label);
        free(img.file.p);
    }
}

static void test_verdict_mok_and_hashes(void **state)
{
    static store mok, dbx, db;
    trust_stores st;
    trust_result res;
    image img;
    blob own = need("mok.der");

    (void)state;
    memset(&st, 0, sizeof(st));
    load_store(&mok, (const char *const[]){ "mok.der" }, 1);
    st.mok = sspan(&mok);
    st.own_mok.p = own.p;
    st.own_mok.len = own.len;

    image_load(&img, need("mok.efi"));
    trust_evaluate(&st, &img.im, &res);
    assert_int_equal(res.verdict, TRUST_OK);
    assert_int_equal(res.by, TRUST_LIST_MOK);
    assert_true(res.own_mok);
    assert_string_equal(res.label, "MOK");
    assert_int_equal(res.nsigs, 1);
    assert_true(res.sig[0].parsed);
    assert_string_equal(res.sig[0].signer, "falcon-boot TEST MOK");
    free(img.file.p);

    /* An unsigned image enrolled by hash (MokManager's "enroll hash"). */
    image_load(&img, need("unsigned.efi"));
    memset(&mok, 0, sizeof(mok));
    store_add(&mok, G_SHA256, img.hash, 32);
    st.mok = sspan(&mok);
    trust_evaluate(&st, &img.im, &res);
    assert_int_equal(res.verdict, TRUST_OK);
    assert_true(res.by_hash);
    assert_string_equal(res.label, "enrolled hash");

    /* ...and the same hash in db. */
    memset(&db, 0, sizeof(db));
    store_add(&db, G_SHA256, img.hash, 32);
    st.db = sspan(&db);
    st.mok.len = 0;
    trust_evaluate(&st, &img.im, &res);
    assert_int_equal(res.verdict, TRUST_OK);
    assert_int_equal(res.by, TRUST_LIST_DB);

    /* The hash in dbx wins over the hash in db. */
    memset(&dbx, 0, sizeof(dbx));
    store_add(&dbx, G_SHA256, img.hash, 32);
    st.dbx = sspan(&dbx);
    trust_evaluate(&st, &img.im, &res);
    assert_int_equal(res.verdict, TRUST_REVOKED);
    assert_int_equal(res.by, TRUST_LIST_DBX);
    assert_true(res.by_hash);
    assert_string_equal(res.label, "unsigned");

    /* In MokListXRT instead. */
    st.mokx = st.dbx;
    st.dbx.len = 0;
    trust_evaluate(&st, &img.im, &res);
    assert_int_equal(res.by, TRUST_LIST_MOKX);

    /* A hash that was never computed decides nothing. */
    img.im.hash = NULL;
    trust_evaluate(&st, &img.im, &res);
    assert_int_equal(res.verdict, TRUST_UNSIGNED);
    free(img.file.p);

    /* A signed image whose signer is in MokListXRT. */
    image_load(&img, need("mok.efi"));
    memset(&dbx, 0, sizeof(dbx));
    store_cert(&dbx, "mok.der");
    st.mokx = sspan(&dbx);
    st.mok = sspan(&mok);
    trust_evaluate(&st, &img.im, &res);
    assert_int_equal(res.verdict, TRUST_REVOKED);
    assert_int_equal(res.by, TRUST_LIST_MOKX);
    assert_int_equal(res.sig[0].list, TRUST_LIST_MOKX);
    free(img.file.p);
    free(own.p);
}

static void test_verdict_sbat(void **state)
{
    static store db;
    trust_stores st;
    trust_result res;
    image img;
    char level[256];
    const uint8_t *p;
    size_t i, n;
    unsigned gen = 0;

    (void)state;
    memset(&st, 0, sizeof(st));
    load_store(&db, (const char *const[]){ "dba.der" }, 1);
    st.db = sspan(&db);
    image_load(&img, need("dba.efi"));

    /* The loader's own component is its second SBAT line. */
    p = img.im.sbat.p;
    n = img.im.sbat.len;
    for (i = 0; i < n && p[i] != '\n'; i++)
        ;
    i++;
    {
        size_t start = i, comma;

        for (comma = start; comma < n && p[comma] != ','; comma++)
            ;
        for (i = comma + 1u; i < n && p[i] >= '0' && p[i] <= '9'; i++)
            gen = gen * 10u + (unsigned)(p[i] - '0');
        snprintf(level, sizeof(level), "sbat,1,2024010900\n%.*s,%u\n",
                 (int)(comma - start), (const char *)p + start, gen);
    }
    st.sbat_level = text(level);
    trust_evaluate(&st, &img.im, &res);
    assert_int_equal(res.verdict, TRUST_OK);

    level[strlen(level) - 2]++;     /* one generation higher */
    st.sbat_level = text(level);
    trust_evaluate(&st, &img.im, &res);
    assert_int_equal(res.verdict, TRUST_REVOKED);
    assert_int_equal(res.by, TRUST_LIST_SBAT);
    assert_string_equal(res.label, "Database Key");
    free(img.file.p);
}

static void test_verdict_unreadable(void **state)
{
    trust_stores st;
    trust_image im;
    trust_result res;
    uint8_t tbl[32];

    (void)state;
    memset(&st, 0, sizeof(st));
    memset(&im, 0, sizeof(im));

    /* A PKCS#7 entry full of junk: signed, but nothing to say. */
    memset(tbl, 0, sizeof(tbl));
    put32(tbl, 24);
    tbl[4] = 0x00; tbl[5] = 0x02; tbl[6] = 0x02;
    memset(tbl + 8, 0x41, 16);
    im.certs.p = tbl;
    im.certs.len = 24;
    trust_evaluate(&st, &im, &res);
    assert_int_equal(res.verdict, TRUST_NO_VERDICT);
    assert_int_equal(res.nsigs, 1);
    assert_false(res.sig[0].parsed);
    assert_string_equal(res.label, "");

    /* A table with no PKCS#7 entry at all. */
    tbl[6] = 0x01;
    trust_evaluate(&st, &im, &res);
    assert_int_equal(res.verdict, TRUST_NO_VERDICT);

    /* A table whose walk breaks. */
    put32(tbl, 3);
    trust_evaluate(&st, &im, &res);
    assert_int_equal(res.verdict, TRUST_NO_VERDICT);

    trust_evaluate(NULL, &im, &res);
    assert_int_equal(res.verdict, TRUST_NO_VERDICT);
    trust_evaluate(&st, &im, NULL);

    /* Unsigned, but db could not be read: no finding. */
    im.certs.len = 0;
    trust_evaluate(&st, &im, &res);
    assert_int_equal(res.verdict, TRUST_UNSIGNED);
    st.incomplete = 1;
    trust_evaluate(&st, &im, &res);
    assert_int_equal(res.verdict, TRUST_NO_VERDICT);
    assert_string_equal(res.label, "");
}

static void test_summary(void **state)
{
    static store db, mok;
    trust_stores st;
    trust_summary sum;
    blob own = need("mok.der");

    (void)state;
    memset(&st, 0, sizeof(st));
    load_store(&db, (const char *const[]){ "dba.der", "dbb.der" }, 2);
    load_store(&mok, (const char *const[]){ "mok.der", "root.der" }, 2);
    st.db = sspan(&db);
    st.mok = sspan(&mok);
    trust_summarize(&st, &sum);
    /* Without knowing which MOK is ours, it is just another certificate. */
    assert_false(sum.mok);
    assert_int_equal(sum.other, 4);

    st.own_mok.p = own.p;
    st.own_mok.len = own.len;
    trust_summarize(&st, &sum);
    assert_true(sum.mok);
    assert_false(sum.ms_uefi);
    assert_false(sum.ms_windows);
    assert_int_equal(sum.other, 3);

    trust_summarize(NULL, &sum);
    assert_int_equal(sum.other, 0);
    trust_summarize(&st, NULL);
    free(own.p);
}

/* Every byte of a real two-signature certificate table, flipped in turn, and
 * every truncation of it. db holds only dbb: the key the table's first
 * signature does NOT use, with the same Name, and nothing the second
 * signature's chain can reach.
 *
 * What may come out of that is pinned exactly. The parser never crashes and
 * never reads past the table. And TRUST_OK appears for one class of flip
 * only: a byte inside the signing certificate's SUBJECT. That turns dba's
 * self-issued certificate into one named something else but still issued by
 * "CN=Database Key", which with no key identifiers on either side is exactly
 * what a certificate dbb really issued looks like by name. Telling the two
 * apart takes the signature check the loader deliberately does not do
 * (DESIGN 7.8: a tampered image predicts "boots" and shim refuses it). */
static void test_corruption_sweep(void **state)
{
    static store db;
    trust_stores st;
    image img;
    uint8_t *tbl;
    size_t i, len, subj_lo, subj_hi;
    unsigned seen[5] = { 0 };

    (void)state;
    memset(&st, 0, sizeof(st));
    load_store(&db, (const char *const[]){ "dbb.der" }, 1);
    st.db = sspan(&db);
    image_load(&img, need("dual.efi"));
    len = img.im.certs.len;
    tbl = malloc(len);
    assert_non_null(tbl);

    /* Where the first signature's signer Subject lies in the table. */
    {
        der_span it = img.im.certs, b;
        ac_signed sd;
        x509_cert c;

        assert_int_equal(ac_next_signature(&it, &b), 1);
        assert_int_equal(ac_signed_parse(b, &sd), DER_OK);
        assert_int_equal(ac_signer_cert(&sd, &c), DER_OK);
        subj_lo = (size_t)(c.subject.p - img.im.certs.p);
        subj_hi = subj_lo + c.subject.len;
    }

    for (i = 0; i < len; i++) {
        trust_image im = img.im;
        trust_result res;

        memcpy(tbl, img.im.certs.p, len);
        tbl[i] ^= 0xffu;
        im.certs.p = tbl;
        trust_evaluate(&st, &im, &res);
        if (res.verdict == TRUST_OK && (i < subj_lo || i >= subj_hi))
            fail_msg("byte %zu flipped read as trusted, outside the signer "
                     "subject [%zu, %zu)", i, subj_lo, subj_hi);
        seen[res.verdict]++;
    }
    for (i = 0; i < len; i++) {
        trust_image im = img.im;
        trust_result res;

        im.certs.len = i;
        trust_evaluate(&st, &im, &res);
        assert_int_not_equal(res.verdict, TRUST_OK);
    }
    /* The sweep reached "untrusted" (a flip the structure does not care
     * about), "no verdict" (a flip in structure), and the documented case. */
    assert_true(seen[TRUST_UNTRUSTED] > 0);
    assert_true(seen[TRUST_NO_VERDICT] > 0);
    assert_true(seen[TRUST_OK] > 0);
    free(tbl);
    free(img.file.p);
}

/* ---- trust_probe: the same answers through a read callback ---- */

/* A reader over a blob that fails every read touching [bad_lo, bad_hi). */
typedef struct {
    blob     b;
    uint64_t bad_lo, bad_hi;
    unsigned calls;
} faulty;

static int faulty_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    faulty *f = ctx;

    f->calls++;
    if (off < f->bad_hi && off + len > f->bad_lo)
        return -1;
    return mem_read(&f->b, off, len, buf);
}

static void test_probe(void **state)
{
    static store db;
    trust_stores st;
    trust_result res;
    faulty f;
    ac_layout l;

    (void)state;
    memset(&st, 0, sizeof(st));
    load_store(&db, (const char *const[]){ "dba.der" }, 1);
    st.db = sspan(&db);

    memset(&f, 0, sizeof(f));
    f.b = need("dba.efi");
    trust_probe(&st, faulty_read, &f, f.b.len, &res);
    assert_int_equal(res.verdict, TRUST_OK);
    assert_string_equal(res.label, "Database Key");

    /* Every read failure is "no verdict", wherever it lands. */
    assert_int_equal(ac_layout_read(f.b.p, 4096, f.b.len, &l), AC_OK);
    f.bad_lo = 0; f.bad_hi = 1;
    trust_probe(&st, faulty_read, &f, f.b.len, &res);
    assert_int_equal(res.verdict, TRUST_NO_VERDICT);
    f.bad_lo = l.certs_off; f.bad_hi = l.certs_off + 1u;
    trust_probe(&st, faulty_read, &f, f.b.len, &res);
    assert_int_equal(res.verdict, TRUST_NO_VERDICT);
    f.bad_lo = l.sbat_off; f.bad_hi = l.sbat_off + 1u;
    trust_probe(&st, faulty_read, &f, f.b.len, &res);
    assert_int_equal(res.verdict, TRUST_NO_VERDICT);

    /* A hash read failure costs the hash, not the verdict. The last section
     * lies past the header window, so only the hash reads it. */
    assert_true(l.sect[l.nsect - 1u].off >= 4096u);
    f.bad_lo = l.sect[l.nsect - 1u].off;
    f.bad_hi = f.bad_lo + 1u;
    trust_probe(&st, faulty_read, &f, f.b.len, &res);
    assert_int_equal(res.verdict, TRUST_OK);

    /* Past TRUST_HASH_MAX nothing is hashed: an unsigned image enrolled by
     * hash is then not recognised, and says "unsigned". The file is claimed
     * larger than it is; nothing past the cert table is read. */
    {
        image img;
        static store mok;

        image_load(&img, need("unsigned.efi"));
        memset(&mok, 0, sizeof(mok));
        store_add(&mok, G_SHA256, img.hash, 32);
        st.mok = sspan(&mok);
        f.bad_lo = f.bad_hi = 0;
        free(f.b.p);
        f.b = img.file;
        trust_probe(&st, faulty_read, &f, f.b.len, &res);
        assert_int_equal(res.verdict, TRUST_OK);
        assert_true(res.by_hash);
        trust_probe(&st, faulty_read, &f, TRUST_HASH_MAX + 1u, &res);
        assert_int_equal(res.verdict, TRUST_UNSIGNED);
    }

    trust_probe(&st, faulty_read, &f, 0, &res);
    assert_int_equal(res.verdict, TRUST_NO_VERDICT);
    trust_probe(NULL, faulty_read, &f, f.b.len, &res);
    assert_int_equal(res.verdict, TRUST_NO_VERDICT);
    trust_probe(&st, NULL, &f, f.b.len, &res);
    assert_int_equal(res.verdict, TRUST_NO_VERDICT);
    trust_probe(&st, faulty_read, &f, f.b.len, NULL);
    free(f.b.p);
}

/* A section table past the first 4 KiB window: the probe reads again with a
 * larger one rather than giving up. */
static void test_probe_second_window(void **state)
{
    enum { OPT = 0x1000, SIZE = 0x2100 };
    static uint8_t img[SIZE];
    size_t opt = 0x80u + 24u;
    size_t table = opt + OPT;
    trust_stores st;
    trust_result res;
    faulty f;

    (void)state;
    memset(img, 0, sizeof(img));
    img[0] = 'M'; img[1] = 'Z';
    put32(img + 0x3c, 0x80);
    memcpy(img + 0x80, "PE\0\0", 4);
    img[0x80 + 6] = 1;                          /* one section */
    img[0x80 + 20] = (uint8_t)(OPT & 0xff);
    img[0x80 + 21] = (uint8_t)(OPT >> 8);
    img[opt] = 0x0b; img[opt + 1] = 0x02;       /* PE32+ */
    put32(img + opt + 60, 0x1200);              /* SizeOfHeaders */
    put32(img + opt + 108, 16);                 /* directories */
    memcpy(img + table, ".text", 5);
    put32(img + table + 16, 0x100);
    put32(img + table + 20, 0x2000);

    memset(&st, 0, sizeof(st));
    memset(&f, 0, sizeof(f));
    f.b.p = img;
    f.b.len = SIZE;
    trust_probe(&st, faulty_read, &f, SIZE, &res);
    assert_int_equal(res.verdict, TRUST_UNSIGNED);
    /* A failing second read is no verdict. */
    f.bad_lo = 0x1100; f.bad_hi = 0x1101;
    trust_probe(&st, faulty_read, &f, SIZE, &res);
    assert_int_equal(res.verdict, TRUST_NO_VERDICT);
}

/* ---- real-world ---- */

static const char *shim_path(void)
{
    static char p[512];

    snprintf(p, sizeof(p), "%s/fedora-16.1-shimx64.efi", SHIM_DIR);
    return p;
}

static void test_real_distro_shim(void **state)
{
    static store db, mine;
    trust_stores st;
    trust_result res;
    trust_summary sum;
    image img;
    blob b, c;

    (void)state;
    b = load(SHIM_DIR, "fedora-16.1-shimx64.efi");
    assert_non_null(b.p);
    image_load(&img, b);

    memset(&db, 0, sizeof(db));
    c = load(TRUST_DIR, "ms/MicrosoftCorporationUEFICA2011.der");
    assert_non_null(c.p);
    store_add(&db, G_X509, c.p, c.len);
    free(c.p);
    c = load(TRUST_DIR, "ms/MicrosoftWindowsProductionPCA2011.der");
    assert_non_null(c.p);
    store_add(&db, G_X509, c.p, c.len);
    free(c.p);

    memset(&st, 0, sizeof(st));
    st.db = sspan(&db);
    trust_evaluate(&st, &img.im, &res);
    assert_int_equal(res.verdict, TRUST_OK);
    assert_string_equal(res.label, "MS UEFI CA");
    trust_summarize(&st, &sum);
    assert_true(sum.ms_uefi);
    assert_true(sum.ms_windows);
    assert_int_equal(sum.other, 0);

    /* The Dell: only the user's own key. */
    load_store(&mine, (const char *const[]){ "dba.der" }, 1);
    st.db = sspan(&mine);
    trust_evaluate(&st, &img.im, &res);
    assert_int_equal(res.verdict, TRUST_UNTRUSTED);
    assert_string_equal(res.label, "MS UEFI CA");
    free(img.file.p);
}

/* Every Microsoft certificate the rig's varstore carries, recognised by name
 * one at a time. The fingerprint table is 128 hand-copied bytes; this is what
 * catches a transcription error in it (one did get through: the UEFI CA 2023
 * entry, until this test existed). */
static void test_real_ms_names(void **state)
{
    static const struct {
        const char *file;
        int uefi, windows;
    } certs[] = {
        { "ms/MicrosoftCorporationUEFICA2011.der", 1, 0 },
        { "ms/MicrosoftUEFICA2023.der", 1, 0 },
        { "ms/MicrosoftWindowsProductionPCA2011.der", 0, 1 },
        { "ms/WindowsUEFICA2023.der", 0, 1 },
        { "ms/MicrosoftOptionROMUEFICA2023.der", 0, 0 },
    };
    static store db;
    size_t i;

    (void)state;
    for (i = 0; i < sizeof(certs) / sizeof(certs[0]); i++) {
        trust_stores st;
        trust_summary sum;
        blob c = load(TRUST_DIR, certs[i].file);

        assert_non_null(c.p);
        memset(&db, 0, sizeof(db));
        store_add(&db, G_X509, c.p, c.len);
        free(c.p);
        memset(&st, 0, sizeof(st));
        st.db = sspan(&db);
        trust_summarize(&st, &sum);
        if (sum.ms_uefi != certs[i].uefi || sum.ms_windows != certs[i].windows)
            fail_msg("%s: uefi %d windows %d", certs[i].file, sum.ms_uefi,
                     sum.ms_windows);
    }
}

int run_trust_tests(void)
{
    const struct CMUnitTest pure[] = {
        cmocka_unit_test(test_der_reader),
        cmocka_unit_test(test_der_expect_optional),
        cmocka_unit_test(test_x509_cn_strings),
        cmocka_unit_test(test_signature_walk),
        cmocka_unit_test(test_sbat_rule),
        cmocka_unit_test(test_verdict_unreadable),
        cmocka_unit_test(test_probe_second_window),
    };
    const struct CMUnitTest fixture[] = {
        cmocka_unit_test(test_x509_fields),
        cmocka_unit_test(test_x509_reaches),
        cmocka_unit_test(test_x509_refusals),
        cmocka_unit_test(test_hash_matches_sbsign),
        cmocka_unit_test(test_layout),
        cmocka_unit_test(test_layout_refusals),
        cmocka_unit_test(test_signer_lookup),
        cmocka_unit_test(test_siglist_walk),
        cmocka_unit_test(test_verdict_table),
        cmocka_unit_test(test_verdict_mok_and_hashes),
        cmocka_unit_test(test_verdict_sbat),
        cmocka_unit_test(test_summary),
        cmocka_unit_test(test_corruption_sweep),
        cmocka_unit_test(test_probe),
    };
    const struct CMUnitTest real[] = {
        cmocka_unit_test(test_real_distro_shim),
        cmocka_unit_test(test_real_ms_names),
    };
    int failed;
    FILE *f;
    blob b;

    failed = cmocka_run_group_tests_name("secboot (pure)", pure, NULL, NULL);

    b = load(TRUST_DIR, "leaf.efi");
    if (b.p == NULL) {
        printf("\n[  SKIPPED ] trust fixtures: %s not found.\n"
               "             Build them with: make test-trust "
               "(needs openssl and sbsign).\n", TRUST_DIR);
        return failed + test_skip_rc();
    }
    free(b.p);
    failed += cmocka_run_group_tests_name("secboot (fixtures)", fixture, NULL,
                                          NULL);

    /* The distro shim and the Microsoft set come from the QEMU rig and are
     * not part of test-fixtures; their absence is reported, never failed. */
    b = load(TRUST_DIR, "ms/MicrosoftCorporationUEFICA2011.der");
    f = fopen(shim_path(), "rb");
    if (f != NULL)
        fclose(f);
    if (b.p == NULL || f == NULL) {
        free(b.p);
        printf("\n[  SKIPPED ] secboot real-world: needs %s and the rig's "
               "OVMF varstore.\n", shim_path());
        return failed;
    }
    free(b.p);
    return failed + cmocka_run_group_tests_name("secboot (real shim)", real,
                                                NULL, NULL);
}
