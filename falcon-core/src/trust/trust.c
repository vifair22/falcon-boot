#include <trust/trust.h>

#include <crypto/sha256.h>
#include <lib/string.h>
#include <trust/authenticode.h>
#include <trust/siglist.h>
#include <trust/x509.h>

/* The four Microsoft certificates the banner and the bracket name. Matched by
 * the SHA-256 of their DER, so a certificate that merely CALLS itself
 * "Microsoft Corporation UEFI CA 2011" is not one of them. Names only: the
 * matching that decides a verdict never looks at this table.
 *
 * The third-party CA signs distro shims, memtest and vendor tools; the
 * Windows CA signs bootmgr. They are separate certificates with separate
 * firmware toggles, which is why they are two names (DESIGN 7.8). The Option
 * ROM CA 2023 is deliberately absent: it signs device firmware, not anything
 * this menu lists, and counts as "+N". */
typedef enum { KNOWN_NONE = 0, KNOWN_MS_UEFI, KNOWN_MS_WINDOWS } known_id;

static const struct {
    uint8_t  fp[32];
    known_id id;
} KNOWN[] = {
    { /* Microsoft Corporation UEFI CA 2011 */
      { 0x48, 0xe9, 0x9b, 0x99, 0x1f, 0x57, 0xfc, 0x52, 0xf7, 0x61, 0x49,
        0x59, 0x9b, 0xff, 0x0a, 0x58, 0xc4, 0x71, 0x54, 0x22, 0x9b, 0x9f,
        0x8d, 0x60, 0x3a, 0xc4, 0x0d, 0x35, 0x00, 0x24, 0x85, 0x07 },
      KNOWN_MS_UEFI },
    { /* Microsoft UEFI CA 2023 */
      { 0xf6, 0x12, 0x4e, 0x34, 0x12, 0x5b, 0xee, 0x3f, 0xe6, 0xd7, 0x9a,
        0x57, 0x4e, 0xaa, 0x7b, 0x91, 0xc0, 0xe7, 0xbd, 0x9d, 0x92, 0x9c,
        0x1a, 0x32, 0x11, 0x78, 0xef, 0xd6, 0x11, 0xda, 0xd9, 0x01 },
      KNOWN_MS_UEFI },
    { /* Microsoft Windows Production PCA 2011 */
      { 0xe8, 0xe9, 0x5f, 0x07, 0x33, 0xa5, 0x5e, 0x8b, 0xad, 0x7b, 0xe0,
        0xa1, 0x41, 0x3e, 0xe2, 0x3c, 0x51, 0xfc, 0xea, 0x64, 0xb3, 0xc8,
        0xfa, 0x6a, 0x78, 0x69, 0x35, 0xfd, 0xdc, 0xc7, 0x19, 0x61 },
      KNOWN_MS_WINDOWS },
    { /* Windows UEFI CA 2023 */
      { 0x07, 0x6f, 0x1f, 0xea, 0x90, 0xac, 0x29, 0x15, 0x5e, 0xbf, 0x77,
        0xc1, 0x76, 0x82, 0xf7, 0x5f, 0x1f, 0xdd, 0x1b, 0xe1, 0x96, 0xda,
        0x30, 0x2d, 0xc8, 0x46, 0x1e, 0x35, 0x0a, 0x9a, 0xe3, 0x30 },
      KNOWN_MS_WINDOWS },
};

static known_id known(const x509_cert *c)
{
    uint8_t fp[SHA256_DIGEST_LEN];
    size_t i;

    sha256(c->der.p, c->der.len, fp);
    for (i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
        if (memcmp(fp, KNOWN[i].fp, sizeof(fp)) == 0)
            return KNOWN[i].id;
    }
    return KNOWN_NONE;
}

static void set_name(char *dst, const char *src)
{
    size_t i;

    for (i = 0; i + 1 < TRUST_NAME_MAX; i++) {
        if (src[i] == '\0')
            break;
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* The bracket's word for a certificate: the known Microsoft name, "MOK" for
 * this stick's own, otherwise the certificate's CN. */
static void anchor_name(const trust_stores *st, const x509_cert *a,
                        char *dst)
{
    switch (known(a)) {
    case KNOWN_MS_UEFI:
        set_name(dst, "MS UEFI CA");
        return;
    case KNOWN_MS_WINDOWS:
        set_name(dst, "MS Windows");
        return;
    case KNOWN_NONE:
    default:
        break;
    }
    if (der_eq(a->der, st->own_mok)) {
        set_name(dst, "MOK");
        return;
    }
    if (x509_cn(a->subject, dst, TRUST_NAME_MAX) == 0)
        set_name(dst, "?");
}

/* ---- one signature's chain ---- */

typedef struct {
    x509_cert c[TRUST_CHAIN_MAX];
    size_t    n;
} chain;

static int build_chain(const ac_signed *s, chain *ch)
{
    ch->n = 0;
    if (ac_signer_cert(s, &ch->c[0]) != DER_OK)
        return DER_ERR;
    ch->n = 1;
    while (ch->n < TRUST_CHAIN_MAX &&
           ac_issuer_cert(s, &ch->c[ch->n - 1u], &ch->c[ch->n]) == DER_OK) {
        size_t k;

        /* A certificate set can name issuers in a loop; stop at a repeat. */
        for (k = 0; k < ch->n; k++) {
            if (der_eq(ch->c[k].der, ch->c[ch->n].der))
                return DER_OK;
        }
        ch->n++;
    }
    return DER_OK;
}

/* Does the chain reach an X.509 anchor in `store`? Every certificate in the
 * chain matches an anchor that IS it; only the top one, whose issuer the
 * signature did not include, may match by name (x509.h). */
static int chain_reaches(const chain *ch, der_span store, x509_cert *anchor)
{
    siglist_iter it;
    sig_type type;
    der_span d;

    siglist_begin(&it, store);
    while (siglist_next(&it, &type, &d) == 1) {
        x509_cert a;
        size_t i;

        if (type != SIG_X509 || x509_parse(d, &a) != DER_OK)
            continue;
        for (i = 0; i < ch->n; i++) {
            if (der_eq(ch->c[i].der, a.der) ||
                (i + 1u == ch->n && x509_reaches(&ch->c[i], &a))) {
                if (anchor != (x509_cert *)0)
                    *anchor = a;
                return 1;
            }
        }
    }
    return 0;
}

/* The bracket for a signature nothing enrolled reaches: a known Microsoft CA
 * anywhere in the chain, otherwise the issuer the chain ends at. */
static void unreached_name(const trust_stores *st, const chain *ch,
                           char *dst)
{
    size_t i;

    for (i = 0; i < ch->n; i++) {
        if (known(&ch->c[i]) != KNOWN_NONE) {
            anchor_name(st, &ch->c[i], dst);
            return;
        }
    }
    if (x509_cn(ch->c[ch->n - 1u].issuer, dst, TRUST_NAME_MAX) == 0)
        set_name(dst, "?");
}

/* ---- the verdict ---- */

void trust_evaluate(const trust_stores *st, const trust_image *im,
                    trust_result *out)
{
    der_span it, blob;
    trust_list revoked = TRUST_LIST_NONE;
    trust_list trusted = TRUST_LIST_NONE;
    uint8_t revoked_hash = 0;
    int parsed = 0;
    int rc;

    if (out == (trust_result *)0)
        return;
    memset(out, 0, sizeof(*out));
    if (st == (const trust_stores *)0 || im == (const trust_image *)0)
        return;

    it = im->certs;
    if (it.p == (const uint8_t *)0)
        it.len = 0;
    while ((rc = ac_next_signature(&it, &blob)) == 1) {
        trust_sig *ds = (out->nsigs < TRUST_SIGS_MAX) ? &out->sig[out->nsigs]
                                                       : (trust_sig *)0;
        ac_signed s;
        chain ch;
        x509_cert a;

        if (out->nsigs < 255u)
            out->nsigs++;
        if (ac_signed_parse(blob, &s) != DER_OK ||
            build_chain(&s, &ch) != DER_OK)
            continue;
        parsed++;
        if (ds != (trust_sig *)0) {
            ds->parsed = 1;
            if (x509_cn(ch.c[0].subject, ds->signer, TRUST_NAME_MAX) == 0)
                set_name(ds->signer, "?");
        }

        /* Revocation first, and any one revoked signature is enough: EDK2
         * and shim both consult the deny lists before the allow lists. */
        if (chain_reaches(&ch, st->dbx, &a) ||
            chain_reaches(&ch, st->mokx, &a)) {
            trust_list which = chain_reaches(&ch, st->dbx, (x509_cert *)0)
                                   ? TRUST_LIST_DBX
                                   : TRUST_LIST_MOKX;

            if (revoked == TRUST_LIST_NONE) {
                revoked = which;
                unreached_name(st, &ch, out->label);
            }
            if (ds != (trust_sig *)0) {
                ds->list = which;
                anchor_name(st, &a, ds->top);
            }
            continue;
        }

        if (chain_reaches(&ch, st->db, &a) || chain_reaches(&ch, st->mok, &a)) {
            trust_list which = chain_reaches(&ch, st->db, (x509_cert *)0)
                                   ? TRUST_LIST_DB
                                   : TRUST_LIST_MOK;

            if (trusted == TRUST_LIST_NONE) {
                trusted = which;
                if (revoked == TRUST_LIST_NONE)
                    anchor_name(st, &a, out->label);
                out->own_mok = der_eq(a.der, st->own_mok) ? 1u : 0u;
            }
            if (ds != (trust_sig *)0) {
                ds->list = which;
                anchor_name(st, &a, ds->top);
            }
            continue;
        }

        if (out->label[0] == '\0')
            unreached_name(st, &ch, out->label);
        if (ds != (trust_sig *)0)
            unreached_name(st, &ch, ds->top);
    }

    /* Revocation by the image's own hash, then by SBAT generation. */
    if (revoked == TRUST_LIST_NONE && im->hash != (const uint8_t *)0) {
        if (siglist_has_hash(st->dbx, im->hash))
            revoked = TRUST_LIST_DBX;
        else if (siglist_has_hash(st->mokx, im->hash))
            revoked = TRUST_LIST_MOKX;
        revoked_hash = (revoked != TRUST_LIST_NONE) ? 1u : 0u;
    }
    if (revoked == TRUST_LIST_NONE && sbat_revoked(st->sbat_level, im->sbat))
        revoked = TRUST_LIST_SBAT;

    if (revoked != TRUST_LIST_NONE) {
        out->verdict = TRUST_REVOKED;
        out->by = revoked;
        out->by_hash = revoked_hash;
        out->own_mok = 0;
        if (out->label[0] == '\0')
            set_name(out->label, im->certs.len != 0 ? "?" : "unsigned");
        return;
    }
    if (trusted != TRUST_LIST_NONE) {
        out->verdict = TRUST_OK;
        out->by = trusted;
        return;
    }
    if (im->hash != (const uint8_t *)0 &&
        (siglist_has_hash(st->db, im->hash) ||
         siglist_has_hash(st->mok, im->hash))) {
        out->verdict = TRUST_OK;
        out->by = siglist_has_hash(st->db, im->hash) ? TRUST_LIST_DB
                                                      : TRUST_LIST_MOK;
        out->by_hash = 1;
        set_name(out->label, "enrolled hash");
        return;
    }
    if (st->incomplete) {
        out->label[0] = '\0';
        return;     /* TRUST_NO_VERDICT: see trust_stores.incomplete */
    }
    if (parsed != 0) {
        out->verdict = TRUST_UNTRUSTED;
        return;
    }
    /* A table with nothing readable in it, or one whose walk broke, is not
     * "unsigned": it is something this code could not read, and says so by
     * saying nothing. */
    if (im->certs.len == 0 && out->nsigs == 0 && rc == 0) {
        out->verdict = TRUST_UNSIGNED;
        set_name(out->label, "unsigned");
        return;
    }
    out->verdict = TRUST_NO_VERDICT;
    out->label[0] = '\0';
}

/* ---- the banner ---- */

static void count_store(const trust_stores *st, der_span store,
                        trust_summary *out)
{
    siglist_iter it;
    sig_type type;
    der_span d;

    siglist_begin(&it, store);
    while (siglist_next(&it, &type, &d) == 1) {
        x509_cert a;

        if (type != SIG_X509 || x509_parse(d, &a) != DER_OK)
            continue;
        if (der_eq(a.der, st->own_mok) && st->own_mok.len != 0) {
            out->mok = 1;
            continue;
        }
        switch (known(&a)) {
        case KNOWN_MS_UEFI:
            out->ms_uefi = 1;
            break;
        case KNOWN_MS_WINDOWS:
            out->ms_windows = 1;
            break;
        case KNOWN_NONE:
        default:
            if (out->other < 0xffffu)
                out->other++;
            break;
        }
    }
}

void trust_summarize(const trust_stores *st, trust_summary *out)
{
    if (out == (trust_summary *)0)
        return;
    memset(out, 0, sizeof(*out));
    if (st == (const trust_stores *)0)
        return;
    count_store(st, st->db, out);
    count_store(st, st->mok, out);
}
