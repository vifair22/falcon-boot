/* falcon-boot: TPM 2.0 commands for the TOTP seed. See tpm2.h.
 *
 * References are to the TPM 2.0 Library Specification, Part 1
 * (Architecture) and Part 3 (Commands), and to libtpms' implementation of
 * them, which swtpm runs and the host tests were settled against. */

#include <tpm/tpm2.h>
#include <tpm/tpmbuf.h>
#include <crypto/cfb.h>
#include <crypto/crypto.h>
#include <crypto/hmac.h>
#include <crypto/kdf.h>
#include <crypto/p256.h>
#include <crypto/sha256.h>
#include <lib/string.h>

#define ST_NO_SESSIONS 0x8001u
#define ST_SESSIONS    0x8002u

#define CC_NV_UNDEFINE   0x00000122u
#define CC_NV_DEFINE     0x0000012Au
#define CC_CREATEPRIMARY 0x00000131u
#define CC_NV_WRITE      0x00000137u
#define CC_STARTUP       0x00000144u
#define CC_NV_READ       0x0000014Eu
#define CC_FLUSH         0x00000165u
#define CC_NV_READPUBLIC 0x00000169u
#define CC_READPUBLIC    0x00000173u
#define CC_STARTSESSION  0x00000176u
#define CC_PCR_READ      0x0000017Eu
#define CC_POLICYPCR     0x0000017Fu
#define CC_PCR_EXTEND    0x00000182u

#define RS_PW   0x40000009u
#define RH_NULL 0x40000007u

#define ALG_AES    0x0006u
#define ALG_SHA256 0x000Bu
#define ALG_NULL   0x0010u
#define ALG_ECC    0x0023u
#define ALG_CFB    0x0043u
#define ECC_P256   0x0003u

#define SA_CONTINUE 0x01u
#define SA_DECRYPT  0x20u
#define SA_ENCRYPT  0x40u

#define SE_HMAC   0x00u
#define SE_POLICY 0x01u

#define RC_INITIALIZE   0x00000100u
#define RC_PCR_CHANGED  0x00000128u
#define RC_FMT1         0x00000080u
#define RC_POLICY_FAIL  0x0000001Du

/* TPMA_OBJECT for a storage key: fixedTPM, fixedParent,
 * sensitiveDataOrigin, userWithAuth, noDA, restricted, decrypt. */
#define SRK_ATTRS   0x00030472u
#define SRK_NEED    0x00030000u     /* restricted | decrypt */

/* TPMA_NV: OWNERWRITE, WRITEALL, POLICYREAD, NO_DA; the ordinary type.
 * Writing takes the owner's authorisation (as defining and removing do),
 * a partial write is refused, and reading takes the PCR policy and nothing
 * else: no owner or authValue read. NO_DA because a policy session carries
 * no password to guess, and a read refused for a PCR mismatch is not an
 * attack to count. */
#define NV_ATTRS    0x02081002u

/* ---- the command runner ---- */

typedef struct {
    tpm2_session  *s;           /* NULL: the password session */
    const uint8_t *auth;        /* the password, or the entity's authValue */
    size_t         auth_len;
    int            include_auth;/* an HMAC session over an unbound entity */
    uint8_t        attrs;
} auth_ent;

typedef struct {
    const uint8_t *p;
    size_t         len;
} span;

static int session_key(const tpm2_session *s, const auth_ent *a,
                       uint8_t *key, size_t *key_len)
{
    size_t n = sizeof s->key;

    memcpy(key, s->key, n);
    if (a->include_auth && a->auth_len != 0) {
        if (a->auth_len > TPM2_DIGEST_LEN) {
            return TPM_ERR_ARG;
        }
        memcpy(key + n, a->auth, a->auth_len);
        n += a->auth_len;
    }
    *key_len = n;
    return TPM_OK;
}

/* Parameter encryption (Part 1 Section 21.3): the first parameter, which is
 * a TPM2B, AES-128-CFB with key and IV from KDFa(sessionValue, "CFB",
 * nonceNewer, nonceOlder). Newer is the caller's nonce on a command and the
 * TPM's on a response. */
static int param_crypt(const tpm2_session *s, const auth_ent *a, uint8_t *p,
                       size_t len, int response)
{
    uint8_t key[2u * TPM2_DIGEST_LEN];
    uint8_t kiv[32];
    size_t kl;
    aes_key ak;
    uint16_t n;
    int rc;

    if (len < 2u) {
        return TPM_ERR_PROTO;
    }
    n = (uint16_t)((p[0] << 8) | p[1]);
    if ((size_t)n > len - 2u) {
        return TPM_ERR_PROTO;
    }
    rc = session_key(s, a, key, &kl);
    if (rc != TPM_OK) {
        return rc;
    }
    if (response) {
        rc = kdfa_sha256(key, kl, "CFB", s->nonce_tpm, s->nonce_tpm_len,
                         s->nonce_caller, sizeof s->nonce_caller,
                         kiv, 256);
    } else {
        rc = kdfa_sha256(key, kl, "CFB", s->nonce_caller,
                         sizeof s->nonce_caller, s->nonce_tpm,
                         s->nonce_tpm_len, kiv, 256);
    }
    if (rc == CRYPTO_OK) {
        (void)aes_setkey(&ak, kiv, 16);
        if (response) {
            aes_cfb_decrypt(&ak, kiv + 16, p + 2, p + 2, n);
        } else {
            aes_cfb_encrypt(&ak, kiv + 16, p + 2, p + 2, n);
        }
        crypto_wipe(&ak, sizeof ak);
    }
    crypto_wipe(key, sizeof key);
    crypto_wipe(kiv, sizeof kiv);
    return rc == CRYPTO_OK ? TPM_OK : TPM_ERR_ARG;
}

/* HMAC(sessionValue, pHash || nonceNewer || nonceOlder [|| extra] || attrs)
 * (Part 1 Section 19.6). */
static int session_hmac(const tpm2_session *s, const auth_ent *a,
                        const uint8_t phash[TPM2_DIGEST_LEN], int response,
                        const tpm2_session *extra, uint8_t attrs,
                        uint8_t out[TPM2_DIGEST_LEN])
{
    uint8_t key[2u * TPM2_DIGEST_LEN];
    size_t kl;
    hmac_key hk;
    hmac_ctx c;
    int rc;

    rc = session_key(s, a, key, &kl);
    if (rc != TPM_OK) {
        return rc;
    }
    if (hmac_key_init(&hk, hash_sha256(), key, kl) != CRYPTO_OK) {
        crypto_wipe(key, sizeof key);
        return TPM_ERR_ARG;
    }
    hmac_begin(&c, &hk);
    hmac_update(&c, phash, TPM2_DIGEST_LEN);
    if (response) {
        hmac_update(&c, s->nonce_tpm, s->nonce_tpm_len);
        hmac_update(&c, s->nonce_caller, sizeof s->nonce_caller);
    } else {
        hmac_update(&c, s->nonce_caller, sizeof s->nonce_caller);
        hmac_update(&c, s->nonce_tpm, s->nonce_tpm_len);
        if (extra != NULL) {
            hmac_update(&c, extra->nonce_tpm, extra->nonce_tpm_len);
        }
    }
    hmac_update(&c, &attrs, 1);
    hmac_final(&c, out);
    crypto_wipe(&hk, sizeof hk);
    crypto_wipe(key, sizeof key);
    return TPM_OK;
}

/* Runs one command. params is the caller's buffer and is encrypted in place
 * when a session asks for it. On success *rp and *rpl are the response
 * parameters inside d->rsp, decrypted when a session asked for that, and the
 * sessions' nonces have rolled. A nonzero response code is TPM_ERR_RC with
 * the code in d->rc. */
static int run(tpm_dev *d, uint32_t cc, const uint32_t *handles,
               const span *names, size_t nh, uint8_t *params, size_t plen,
               auth_ent *auths, size_t na, uint32_t *rsp_handle,
               const uint8_t **rp, size_t *rpl)
{
    tpmw w;
    tpmr r;
    size_t i, auth_off = 0, rl, rh = (rsp_handle != NULL) ? 1u : 0u;
    uint8_t cphash[TPM2_DIGEST_LEN];
    int rc;
    const tpm2_session *extra = NULL;

    /* Names are needed only for the cpHash, so only with sessions. */
    if (na > 2u || nh > 2u || (na != 0 && nh != 0 && names == NULL)) {
        return TPM_ERR_ARG;
    }
    for (i = 0; i < na; i++) {
        if (auths[i].s != NULL) {
            rc = tpm_random(d, auths[i].s->nonce_caller,
                            sizeof auths[i].s->nonce_caller);
            if (rc != TPM_OK) {
                return rc;
            }
            if ((auths[i].attrs & SA_DECRYPT) != 0u) {
                rc = param_crypt(auths[i].s, &auths[i], params, plen, 0);
                if (rc != TPM_OK) {
                    return rc;
                }
            }
        }
    }
    /* The first session's HMAC also covers the nonce of a separate
     * encryption session, so the TPM knows it was not swapped (Part 1
     * Section 19.6.5). */
    if (na == 2u && auths[0].s != NULL && auths[1].s != NULL &&
        (auths[1].attrs & (SA_DECRYPT | SA_ENCRYPT)) != 0u) {
        extra = auths[1].s;
    }

    /* cpHash = SHA-256(commandCode || names || parameters), the parameters
     * as sent, so after encryption. */
    if (na != 0) {
        sha256_ctx h;
        uint8_t ccb[4];

        tpm_put_be32(ccb, cc);
        sha256_init(&h);
        sha256_update(&h, ccb, 4);
        for (i = 0; i < nh; i++) {
            sha256_update(&h, names[i].p, names[i].len);
        }
        sha256_update(&h, params, plen);
        sha256_final(&h, cphash);
    }

    tpmw_init(&w, d->cmd, sizeof d->cmd);
    tpmw_u16(&w, (uint16_t)(na != 0 ? ST_SESSIONS : ST_NO_SESSIONS));
    tpmw_u32(&w, 0);
    tpmw_u32(&w, cc);
    for (i = 0; i < nh; i++) {
        tpmw_u32(&w, handles[i]);
    }
    if (na != 0) {
        auth_off = w.len;
        tpmw_u32(&w, 0);
        for (i = 0; i < na; i++) {
            auth_ent *a = &auths[i];

            if (a->s == NULL) {
                tpmw_u32(&w, RS_PW);
                tpmw_2b(&w, NULL, 0);
                tpmw_u8(&w, a->attrs);
                tpmw_2b(&w, a->auth, a->auth_len);
            } else {
                uint8_t mac[TPM2_DIGEST_LEN];

                rc = session_hmac(a->s, a, cphash, 0, i == 0 ? extra : NULL,
                                  a->attrs, mac);
                if (rc != TPM_OK) {
                    return rc;
                }
                tpmw_u32(&w, a->s->handle);
                tpmw_2b(&w, a->s->nonce_caller, sizeof a->s->nonce_caller);
                tpmw_u8(&w, a->attrs);
                tpmw_2b(&w, mac, sizeof mac);
            }
        }
        tpmw_patch32(&w, auth_off, (uint32_t)(w.len - auth_off - 4u));
    }
    tpmw_bytes(&w, params, plen);
    if (w.err) {
        return TPM_ERR_NOSPACE;
    }
    rc = tpm_transact(d, w.len, &rl);
    if (rc != TPM_OK) {
        return rc;
    }
    if (d->rc != 0u) {
        return TPM_ERR_RC;
    }

    tpmr_init(&r, d->rsp, rl);
    {
        uint16_t tag = tpmr_u16(&r);
        size_t plen_rsp;
        const uint8_t *pp;

        (void)tpmr_u32(&r);
        (void)tpmr_u32(&r);
        if (rh != 0u) {
            *rsp_handle = tpmr_u32(&r);
        }
        if (tag == ST_SESSIONS) {
            plen_rsp = tpmr_u32(&r);
        } else {
            plen_rsp = r.err ? 0u : r.len - r.off;
        }
        pp = tpmr_bytes(&r, NULL, plen_rsp);
        if (r.err || (tag == ST_SESSIONS) != (na != 0)) {
            return TPM_ERR_PROTO;
        }

        if (na != 0) {
            uint8_t rphash[TPM2_DIGEST_LEN];
            sha256_ctx h;
            uint8_t hdr[8];

            /* rpHash = SHA-256(responseCode || commandCode || parameters),
             * over the parameters as received. */
            tpm_put_be32(hdr, 0);
            tpm_put_be32(hdr + 4, cc);
            sha256_init(&h);
            sha256_update(&h, hdr, 8);
            sha256_update(&h, pp, plen_rsp);
            sha256_final(&h, rphash);

            for (i = 0; i < na; i++) {
                auth_ent *a = &auths[i];
                uint8_t nonce[TPM2_DIGEST_LEN];
                uint8_t mac[TPM2_DIGEST_LEN];
                uint8_t want[TPM2_DIGEST_LEN];
                uint16_t nl, ml;
                uint8_t attrs;

                tpmr_2b(&r, nonce, sizeof nonce, &nl);
                attrs = tpmr_u8(&r);
                tpmr_2b(&r, mac, sizeof mac, &ml);
                if (r.err) {
                    return TPM_ERR_PROTO;
                }
                if (a->s == NULL) {
                    continue;
                }
                memcpy(a->s->nonce_tpm, nonce, nl);
                a->s->nonce_tpm_len = nl;
                rc = session_hmac(a->s, a, rphash, 1, NULL, attrs, want);
                if (rc != TPM_OK) {
                    return rc;
                }
                if (ml != TPM2_DIGEST_LEN || !crypto_ct_eq(mac, want, ml)) {
                    return TPM_ERR_AUTH;
                }
            }
            for (i = 0; i < na; i++) {
                if (auths[i].s != NULL &&
                    (auths[i].attrs & SA_ENCRYPT) != 0u) {
                    /* The HMAC covered the ciphertext; decrypt in place. */
                    rc = param_crypt(auths[i].s, &auths[i],
                                     d->rsp + (size_t)(pp - d->rsp),
                                     plen_rsp, 1);
                    if (rc != TPM_OK) {
                        return rc;
                    }
                }
            }
        }
        if (rp != NULL) {
            *rp = pp;
            *rpl = plen_rsp;
        }
    }
    return TPM_OK;
}

static void put_pcr_select(tpmw *w, uint32_t mask)
{
    tpmw_u32(w, 1);             /* one bank */
    tpmw_u16(w, ALG_SHA256);
    tpmw_u8(w, 3);              /* 24 PCRs */
    tpmw_u8(w, (uint8_t)mask);
    tpmw_u8(w, (uint8_t)(mask >> 8));
    tpmw_u8(w, (uint8_t)(mask >> 16));
}

static int mask_ok(uint32_t mask)
{
    return mask != 0u && (mask >> TPM2_PCR_MAX) == 0u;
}

/* ---- PCRs and policy ---- */

int tpm2_startup(tpm_dev *d)
{
    uint8_t p[2] = { 0x00, 0x00 };          /* TPM_SU_CLEAR */
    int rc;

    if (d == NULL) {
        return TPM_ERR_ARG;
    }
    rc = run(d, CC_STARTUP, NULL, NULL, 0, p, sizeof p, NULL, 0, NULL,
             NULL, NULL);
    /* Already started is what a machine the firmware booted looks like. */
    if (rc == TPM_ERR_RC && d->rc == RC_INITIALIZE) {
        rc = TPM_OK;
    }
    return rc;
}

int tpm2_pcr_read(tpm_dev *d, unsigned int pcr, uint8_t out[TPM2_DIGEST_LEN])
{
    uint8_t p[16];
    tpmw w;
    tpmr r;
    const uint8_t *rp;
    size_t rpl;
    uint16_t n;
    int rc;

    if (d == NULL || out == NULL || pcr >= TPM2_PCR_MAX) {
        return TPM_ERR_ARG;
    }
    tpmw_init(&w, p, sizeof p);
    put_pcr_select(&w, 1u << pcr);
    rc = run(d, CC_PCR_READ, NULL, NULL, 0, p, w.len, NULL, 0, NULL,
             &rp, &rpl);
    if (rc != TPM_OK) {
        return rc;
    }
    /* pcrUpdateCounter, pcrSelectionOut (one bank, echoed), pcrValues. */
    tpmr_init(&r, rp, rpl);
    (void)tpmr_u32(&r);
    if (tpmr_u32(&r) != 1u) {
        return TPM_ERR_PROTO;
    }
    (void)tpmr_u16(&r);
    (void)tpmr_bytes(&r, NULL, tpmr_u8(&r));
    if (tpmr_u32(&r) != 1u) {
        return TPM_ERR_PROTO;       /* the SHA-256 bank is not allocated */
    }
    tpmr_2b(&r, out, TPM2_DIGEST_LEN, &n);
    return (r.err || n != TPM2_DIGEST_LEN) ? TPM_ERR_PROTO : TPM_OK;
}

int tpm2_pcr_extend(tpm_dev *d, unsigned int pcr,
                    const uint8_t digest[TPM2_DIGEST_LEN])
{
    uint8_t p[4 + 2 + TPM2_DIGEST_LEN];
    uint32_t h = pcr;
    span nm;
    uint8_t hb[4];
    auth_ent a;
    tpmw w;

    if (d == NULL || digest == NULL || pcr >= TPM2_PCR_MAX) {
        return TPM_ERR_ARG;
    }
    tpmw_init(&w, p, sizeof p);
    tpmw_u32(&w, 1);
    tpmw_u16(&w, ALG_SHA256);
    tpmw_bytes(&w, digest, TPM2_DIGEST_LEN);
    tpm_put_be32(hb, h);
    nm.p = hb;
    nm.len = 4;
    memset(&a, 0, sizeof a);
    a.attrs = SA_CONTINUE;
    return run(d, CC_PCR_EXTEND, &h, &nm, 1, p, w.len, &a, 1, NULL, NULL,
               NULL);
}

int tpm2_policy_pcr_digest(uint32_t mask,
                           const uint8_t (*values)[TPM2_DIGEST_LEN],
                           uint8_t out[TPM2_DIGEST_LEN])
{
    uint8_t pcrdig[TPM2_DIGEST_LEN];
    uint8_t buf[TPM2_DIGEST_LEN + 4 + 10];
    sha256_ctx h;
    unsigned int i, k = 0;
    tpmw w;

    if (values == NULL || out == NULL || !mask_ok(mask)) {
        return TPM_ERR_ARG;
    }
    /* pcrDigest = SHA-256 of the selected values in ascending order. */
    sha256_init(&h);
    for (i = 0; i < TPM2_PCR_MAX; i++) {
        if ((mask >> i) & 1u) {
            sha256_update(&h, values[k++], TPM2_DIGEST_LEN);
        }
    }
    sha256_final(&h, pcrdig);

    /* policyDigest' = SHA-256(policyDigest || TPM_CC_PolicyPCR || pcrs ||
     * pcrDigest), from the all-zero digest of an empty policy (Part 3
     * Section 23.7). */
    memset(buf, 0, TPM2_DIGEST_LEN);
    tpmw_init(&w, buf + TPM2_DIGEST_LEN, sizeof buf - TPM2_DIGEST_LEN);
    tpmw_u32(&w, CC_POLICYPCR);
    put_pcr_select(&w, mask);
    sha256_init(&h);
    sha256_update(&h, buf, TPM2_DIGEST_LEN + w.len);
    sha256_update(&h, pcrdig, sizeof pcrdig);
    sha256_final(&h, out);
    return TPM_OK;
}

/* ---- the salt key ---- */

/* An ECC P-256 restricted decryption key's public area: the point and the
 * name, SHA-256 over the TPMT_PUBLIC exactly as the TPM sent it. */
static int parse_ecc_public(const uint8_t *pub, size_t len, tpm2_saltkey *k)
{
    tpmr r;
    uint16_t alg, n;
    uint32_t attrs;

    tpmr_init(&r, pub, len);
    if (tpmr_u16(&r) != ALG_ECC || tpmr_u16(&r) != ALG_SHA256) {
        return TPM_ERR_KEY;
    }
    attrs = tpmr_u32(&r);
    (void)tpmr_bytes(&r, NULL, tpmr_u16(&r));      /* authPolicy */
    alg = tpmr_u16(&r);                             /* symmetric */
    if (alg != ALG_AES) {
        return TPM_ERR_KEY;
    }
    (void)tpmr_u16(&r);
    (void)tpmr_u16(&r);
    if (tpmr_u16(&r) != ALG_NULL) {                 /* scheme */
        return TPM_ERR_KEY;
    }
    if (tpmr_u16(&r) != ECC_P256) {
        return TPM_ERR_KEY;
    }
    if (tpmr_u16(&r) != ALG_NULL) {                 /* kdf */
        return TPM_ERR_KEY;
    }
    tpmr_2b(&r, k->x, sizeof k->x, &n);
    if (n != 32u) {
        return TPM_ERR_KEY;
    }
    tpmr_2b(&r, k->y, sizeof k->y, &n);
    if (n != 32u || r.err || r.off != len ||
        (attrs & SRK_NEED) != SRK_NEED ||
        p256_check_point(k->x, k->y) != CRYPTO_OK) {
        return TPM_ERR_KEY;
    }
    k->name[0] = (uint8_t)(ALG_SHA256 >> 8);
    k->name[1] = (uint8_t)ALG_SHA256;
    sha256(pub, len, k->name + 2);
    return TPM_OK;
}

static int read_public(tpm_dev *d, uint32_t handle, tpm2_saltkey *k)
{
    const uint8_t *rp;
    size_t rpl;
    tpmr r;
    uint16_t n;
    const uint8_t *pub;
    int rc;

    rc = run(d, CC_READPUBLIC, &handle, NULL, 1, NULL, 0, NULL, 0, NULL,
             &rp, &rpl);
    if (rc != TPM_OK) {
        return rc;
    }
    tpmr_init(&r, rp, rpl);
    n = tpmr_u16(&r);
    pub = tpmr_bytes(&r, NULL, n);
    if (r.err) {
        return TPM_ERR_PROTO;
    }
    return parse_ecc_public(pub, n, k);
}

/* The storage key template: ECC P-256, AES-128-CFB, no scheme, empty
 * unique, the attributes above. `tpm2_createprimary -C o -G
 * ecc256:aes128cfb -a 'fixedtpm|fixedparent|sensitivedataorigin|
 * userwithauth|noda|restricted|decrypt'` makes the same key: the two names
 * were compared on one swtpm. */
static void put_srk_template(tpmw *w)
{
    tpmw_u16(w, ALG_ECC);
    tpmw_u16(w, ALG_SHA256);
    tpmw_u32(w, SRK_ATTRS);
    tpmw_2b(w, NULL, 0);
    tpmw_u16(w, ALG_AES);
    tpmw_u16(w, 128);
    tpmw_u16(w, ALG_CFB);
    tpmw_u16(w, ALG_NULL);
    tpmw_u16(w, ECC_P256);
    tpmw_u16(w, ALG_NULL);
    tpmw_2b(w, NULL, 0);
    tpmw_2b(w, NULL, 0);
}

int tpm2_saltkey_open(tpm_dev *d, const uint8_t *owner_auth, size_t auth_len,
                      tpm2_saltkey *k)
{
    uint8_t p[128];
    tpmw w;
    size_t tmpl_len_off;
    uint32_t h = TPM2_RH_OWNER;
    uint8_t hb[4];
    span nm;
    auth_ent a;
    const uint8_t *rp;
    size_t rpl;
    tpmr r;
    uint16_t n;
    const uint8_t *pub;
    int rc;

    if (d == NULL || k == NULL || (owner_auth == NULL && auth_len != 0)) {
        return TPM_ERR_ARG;
    }
    memset(k, 0, sizeof *k);

    /* The persistent SRK when it is the kind of key the salt needs. Windows
     * puts an RSA one there, which does not qualify. */
    if (read_public(d, TPM2_SRK_HANDLE, k) == TPM_OK) {
        k->handle = TPM2_SRK_HANDLE;
        k->transient = 0;
        return TPM_OK;
    }
    memset(k, 0, sizeof *k);

    tpmw_init(&w, p, sizeof p);
    tpmw_u16(&w, 4);                /* inSensitive: empty userAuth, data */
    tpmw_u16(&w, 0);
    tpmw_u16(&w, 0);
    tmpl_len_off = w.len;
    tpmw_u16(&w, 0);
    put_srk_template(&w);
    if (!w.err) {
        size_t tl = w.len - tmpl_len_off - 2u;

        p[tmpl_len_off] = (uint8_t)(tl >> 8);
        p[tmpl_len_off + 1u] = (uint8_t)tl;
    }
    tpmw_2b(&w, NULL, 0);           /* outsideInfo */
    tpmw_u32(&w, 0);                /* creationPCR: none */
    if (w.err) {
        return TPM_ERR_NOSPACE;
    }
    tpm_put_be32(hb, h);
    nm.p = hb;
    nm.len = 4;
    memset(&a, 0, sizeof a);
    a.auth = owner_auth;
    a.auth_len = auth_len;
    a.attrs = SA_CONTINUE;
    rc = run(d, CC_CREATEPRIMARY, &h, &nm, 1, p, w.len, &a, 1, &k->handle,
             &rp, &rpl);
    if (rc != TPM_OK) {
        return rc;
    }
    k->transient = 1;
    tpmr_init(&r, rp, rpl);
    n = tpmr_u16(&r);
    pub = tpmr_bytes(&r, NULL, n);
    if (r.err) {
        rc = TPM_ERR_PROTO;
    } else {
        uint32_t handle = k->handle;

        rc = parse_ecc_public(pub, n, k);
        k->handle = handle;
        k->transient = 1;
    }
    if (rc != TPM_OK) {
        (void)tpm2_flush(d, k->handle);
        k->handle = 0;
        k->transient = 0;
    }
    return rc;
}

int tpm2_saltkey_close(tpm_dev *d, tpm2_saltkey *k)
{
    int rc = TPM_OK;

    if (d == NULL || k == NULL) {
        return TPM_ERR_ARG;
    }
    if (k->transient && k->handle != 0u) {
        rc = tpm2_flush(d, k->handle);
    }
    k->handle = 0;
    k->transient = 0;
    return rc;
}

int tpm2_flush(tpm_dev *d, uint32_t handle)
{
    uint8_t p[4];

    if (d == NULL) {
        return TPM_ERR_ARG;
    }
    tpm_put_be32(p, handle);
    return run(d, CC_FLUSH, NULL, NULL, 0, p, sizeof p, NULL, 0, NULL,
               NULL, NULL);
}

/* ---- sessions ---- */

int tpm2_session_start(tpm_dev *d, const tpm2_saltkey *k, int policy,
                       tpm2_session *s)
{
    uint8_t eph[32], ex[32], ey[32], z[32], salt[32];
    uint8_t p[160];
    uint32_t h[2];
    tpmw w;
    tpmr r;
    const uint8_t *rp;
    size_t rpl;
    int rc, tries;

    if (d == NULL || k == NULL || s == NULL) {
        return TPM_ERR_ARG;
    }
    memset(s, 0, sizeof *s);

    /* The ephemeral key. A draw outside [1, n-1] (odds about 2^-32) is
     * drawn again, a few times, before the randomness is blamed. */
    rc = TPM_ERR_RNG;
    for (tries = 0; tries < 4; tries++) {
        if (tpm_random(d, eph, sizeof eph) != TPM_OK) {
            break;
        }
        if (p256_public(eph, ex, ey) == CRYPTO_OK) {
            rc = TPM_OK;
            break;
        }
    }
    if (rc == TPM_OK && p256_ecdh(eph, k->x, k->y, z) != CRYPTO_OK) {
        rc = TPM_ERR_KEY;
    }
    /* salt = KDFe(SHA-256, Z, "SECRET", QeU.x, QsV.x, 256) (Part 1
     * Section C.6.1). */
    if (rc == TPM_OK &&
        kdfe_sha256(z, sizeof z, "SECRET", ex, sizeof ex, k->x, sizeof k->x,
                    salt, 256) != CRYPTO_OK) {
        rc = TPM_ERR_ARG;
    }
    if (rc == TPM_OK) {
        rc = tpm_random(d, s->nonce_caller, sizeof s->nonce_caller);
    }
    crypto_wipe(eph, sizeof eph);
    crypto_wipe(z, sizeof z);
    if (rc != TPM_OK) {
        crypto_wipe(salt, sizeof salt);
        return rc;
    }

    tpmw_init(&w, p, sizeof p);
    tpmw_2b(&w, s->nonce_caller, sizeof s->nonce_caller);
    tpmw_u16(&w, 2u + 32u + 2u + 32u);     /* encryptedSalt: TPMS_ECC_POINT */
    tpmw_2b(&w, ex, sizeof ex);
    tpmw_2b(&w, ey, sizeof ey);
    tpmw_u8(&w, policy ? SE_POLICY : SE_HMAC);
    tpmw_u16(&w, ALG_AES);
    tpmw_u16(&w, 128);
    tpmw_u16(&w, ALG_CFB);
    tpmw_u16(&w, ALG_SHA256);
    h[0] = k->handle;
    h[1] = RH_NULL;
    rc = run(d, CC_STARTSESSION, h, NULL, 2, p, w.len, NULL, 0, &s->handle,
             &rp, &rpl);
    if (rc == TPM_OK) {
        tpmr_init(&r, rp, rpl);
        tpmr_2b(&r, s->nonce_tpm, sizeof s->nonce_tpm, &s->nonce_tpm_len);
        if (r.err) {
            rc = TPM_ERR_PROTO;
        }
    }
    /* sessionKey = KDFa(SHA-256, salt, "ATH", nonceTPM, nonceCaller, 256);
     * with no bind entity the salt is the whole key. */
    if (rc == TPM_OK &&
        kdfa_sha256(salt, sizeof salt, "ATH", s->nonce_tpm, s->nonce_tpm_len,
                    s->nonce_caller, sizeof s->nonce_caller,
                    s->key, 256) != CRYPTO_OK) {
        rc = TPM_ERR_ARG;
    }
    crypto_wipe(salt, sizeof salt);
    if (rc != TPM_OK && s->handle != 0u) {
        (void)tpm2_flush(d, s->handle);
        memset(s, 0, sizeof *s);
    }
    return rc;
}

int tpm2_policy_pcr(tpm_dev *d, const tpm2_session *s, uint32_t mask)
{
    uint8_t p[16];
    tpmw w;
    uint32_t h;

    if (d == NULL || s == NULL || !mask_ok(mask)) {
        return TPM_ERR_ARG;
    }
    /* An empty pcrDigest: the TPM takes the current values, and a mismatch
     * surfaces at the read as TPM_RC_POLICY_FAIL. */
    tpmw_init(&w, p, sizeof p);
    tpmw_2b(&w, NULL, 0);
    put_pcr_select(&w, mask);
    h = s->handle;
    return run(d, CC_POLICYPCR, &h, NULL, 1, p, w.len, NULL, 0, NULL, NULL,
               NULL);
}

/* ---- NV ---- */

int tpm2_nv_define(tpm_dev *d, const uint8_t *owner_auth, size_t auth_len,
                   uint32_t index, uint16_t size,
                   const uint8_t policy[TPM2_DIGEST_LEN])
{
    uint8_t p[64];
    tpmw w;
    uint32_t h = TPM2_RH_OWNER;
    uint8_t hb[4];
    span nm;
    auth_ent a;

    if (d == NULL || policy == NULL || size == 0 || size > TPM2_NV_MAX ||
        (owner_auth == NULL && auth_len != 0)) {
        return TPM_ERR_ARG;
    }
    tpmw_init(&w, p, sizeof p);
    tpmw_2b(&w, NULL, 0);                   /* the index's authValue */
    tpmw_u16(&w, 4u + 2u + 4u + 2u + TPM2_DIGEST_LEN + 2u);
    tpmw_u32(&w, index);
    tpmw_u16(&w, ALG_SHA256);
    tpmw_u32(&w, NV_ATTRS);
    tpmw_2b(&w, policy, TPM2_DIGEST_LEN);
    tpmw_u16(&w, size);
    tpm_put_be32(hb, h);
    nm.p = hb;
    nm.len = 4;
    memset(&a, 0, sizeof a);
    a.auth = owner_auth;
    a.auth_len = auth_len;
    a.attrs = SA_CONTINUE;
    return run(d, CC_NV_DEFINE, &h, &nm, 1, p, w.len, &a, 1, NULL, NULL,
               NULL);
}

int tpm2_nv_undefine(tpm_dev *d, const uint8_t *owner_auth, size_t auth_len,
                     uint32_t index)
{
    uint32_t h[2];
    span nm[2];
    uint8_t hb[4];
    uint8_t name[TPM2_NAME_LEN];
    auth_ent a;
    int rc;

    if (d == NULL || (owner_auth == NULL && auth_len != 0)) {
        return TPM_ERR_ARG;
    }
    rc = tpm2_nv_name(d, index, name, NULL);
    if (rc != TPM_OK) {
        return rc;
    }
    h[0] = TPM2_RH_OWNER;
    h[1] = index;
    tpm_put_be32(hb, h[0]);
    nm[0].p = hb;
    nm[0].len = 4;
    nm[1].p = name;
    nm[1].len = sizeof name;
    memset(&a, 0, sizeof a);
    a.auth = owner_auth;
    a.auth_len = auth_len;
    a.attrs = SA_CONTINUE;
    return run(d, CC_NV_UNDEFINE, h, nm, 2, NULL, 0, &a, 1, NULL, NULL, NULL);
}

int tpm2_nv_name(tpm_dev *d, uint32_t index, uint8_t name[TPM2_NAME_LEN],
                 uint16_t *size)
{
    const uint8_t *rp;
    size_t rpl;
    tpmr r;
    uint16_t n;
    const uint8_t *pub;
    int rc;

    if (d == NULL || name == NULL) {
        return TPM_ERR_ARG;
    }
    rc = run(d, CC_NV_READPUBLIC, &index, NULL, 1, NULL, 0, NULL, 0, NULL,
             &rp, &rpl);
    if (rc != TPM_OK) {
        return rc;
    }
    /* The name is computed here from the public area rather than taken
     * from the response's nvName: both are unauthenticated, and a wrong one
     * fails the read's HMAC either way, but this way the name is the one
     * that matches the attributes and policy just parsed. */
    tpmr_init(&r, rp, rpl);
    n = tpmr_u16(&r);
    pub = tpmr_bytes(&r, NULL, n);
    if (r.err || n < 14u || tpm_be32(pub) != index ||
        pub[4] != (uint8_t)(ALG_SHA256 >> 8) || pub[5] != (uint8_t)ALG_SHA256) {
        return TPM_ERR_PROTO;
    }
    if (size != NULL) {
        *size = (uint16_t)((pub[n - 2u] << 8) | pub[n - 1u]);
    }
    name[0] = (uint8_t)(ALG_SHA256 >> 8);
    name[1] = (uint8_t)ALG_SHA256;
    sha256(pub, n, name + 2);
    return TPM_OK;
}

int tpm2_nv_write(tpm_dev *d, const uint8_t *owner_auth, size_t auth_len,
                  uint32_t index, const uint8_t *data, size_t len,
                  tpm2_session *enc)
{
    uint8_t p[2u + TPM2_NV_MAX + 2u];
    tpmw w;
    uint32_t h[2];
    span nm[2];
    uint8_t hb[4];
    uint8_t name[TPM2_NAME_LEN];
    auth_ent a[2];
    int rc;

    if (d == NULL || data == NULL || len == 0 || len > TPM2_NV_MAX ||
        (owner_auth == NULL && auth_len != 0)) {
        return TPM_ERR_ARG;
    }
    rc = tpm2_nv_name(d, index, name, NULL);
    if (rc != TPM_OK) {
        return rc;
    }
    tpmw_init(&w, p, sizeof p);
    tpmw_2b(&w, data, len);
    tpmw_u16(&w, 0);                        /* offset */
    h[0] = TPM2_RH_OWNER;
    h[1] = index;
    tpm_put_be32(hb, h[0]);
    nm[0].p = hb;
    nm[0].len = 4;
    nm[1].p = name;
    nm[1].len = sizeof name;
    memset(a, 0, sizeof a);
    a[0].auth = owner_auth;
    a[0].auth_len = auth_len;
    a[0].attrs = SA_CONTINUE;
    /* The encryption session ends with the command (continueSession
     * clear), so a success needs no flush. */
    a[1].s = enc;
    a[1].attrs = SA_DECRYPT;
    rc = run(d, CC_NV_WRITE, h, nm, 2, p, w.len, a, enc != NULL ? 2u : 1u,
             NULL, NULL, NULL);
    crypto_wipe(p, sizeof p);
    return rc;
}

int tpm2_nv_read_policy(tpm_dev *d, tpm2_session *s, uint32_t index,
                        const uint8_t name[TPM2_NAME_LEN],
                        uint8_t *out, size_t len)
{
    uint8_t p[4];
    uint32_t h[2];
    span nm[2];
    auth_ent a;
    const uint8_t *rp;
    size_t rpl;
    int rc;

    if (d == NULL || s == NULL || name == NULL || out == NULL || len == 0 ||
        len > TPM2_NV_MAX) {
        return TPM_ERR_ARG;
    }
    p[0] = (uint8_t)(len >> 8);
    p[1] = (uint8_t)len;
    p[2] = 0;
    p[3] = 0;
    h[0] = index;                           /* authHandle: the index itself */
    h[1] = index;
    nm[0].p = name;
    nm[0].len = TPM2_NAME_LEN;
    nm[1] = nm[0];
    memset(&a, 0, sizeof a);
    a.s = s;
    a.attrs = SA_ENCRYPT;                   /* continueSession clear */
    rc = run(d, CC_NV_READ, h, nm, 2, p, sizeof p, &a, 1, NULL, &rp, &rpl);
    if (rc == TPM_ERR_RC &&
        (d->rc == RC_PCR_CHANGED ||
         ((d->rc & RC_FMT1) != 0u && (d->rc & 0x3Fu) == RC_POLICY_FAIL))) {
        return TPM_ERR_PCR;
    }
    if (rc != TPM_OK) {
        return rc;
    }
    if (rpl < 2u + len || rp[0] != (uint8_t)(len >> 8) ||
        rp[1] != (uint8_t)len) {
        return TPM_ERR_PROTO;
    }
    memcpy(out, rp + 2, len);
    /* The plaintext stays in d->rsp otherwise. */
    crypto_wipe(d->rsp, sizeof d->rsp);
    return TPM_OK;
}

/* ---- the two whole flows ---- */

/* Cleanup after a failure, keeping the failure's response code in d->rc for
 * the caller rather than the flush's. */
static void flush_keep_rc(tpm_dev *d, uint32_t handle)
{
    uint32_t keep = d->rc;

    (void)tpm2_flush(d, handle);
    d->rc = keep;
}

int tpm2_seed_read(tpm_dev *d, uint32_t index, uint32_t pcr_mask,
                   const uint8_t *pin, uint8_t *seed, size_t len)
{
    tpm2_saltkey k;
    tpm2_session s;
    uint8_t name[TPM2_NAME_LEN];
    int rc;

    if (d == NULL || seed == NULL || !mask_ok(pcr_mask)) {
        return TPM_ERR_ARG;
    }
    rc = tpm2_nv_name(d, index, name, NULL);
    if (rc != TPM_OK) {
        return rc;
    }
    rc = tpm2_saltkey_open(d, NULL, 0, &k);
    if (rc != TPM_OK) {
        return rc;
    }
    if (pin != NULL && !crypto_ct_eq(pin, k.name, TPM2_NAME_LEN)) {
        (void)tpm2_saltkey_close(d, &k);
        return TPM_ERR_KEY;
    }
    rc = tpm2_session_start(d, &k, 1, &s);
    (void)tpm2_saltkey_close(d, &k);
    if (rc != TPM_OK) {
        return rc;
    }
    rc = tpm2_policy_pcr(d, &s, pcr_mask);
    if (rc == TPM_OK) {
        rc = tpm2_nv_read_policy(d, &s, index, name, seed, len);
    }
    /* A command that failed leaves the session open. */
    if (rc != TPM_OK) {
        flush_keep_rc(d, s.handle);
    }
    crypto_wipe(&s, sizeof s);
    return rc;
}

int tpm2_seed_enroll(tpm_dev *d, const uint8_t *owner_auth, size_t auth_len,
                     uint32_t index, uint32_t pcr_mask,
                     const uint8_t *seed, size_t len,
                     uint8_t key_name[TPM2_NAME_LEN])
{
    uint8_t vals[TPM2_PCR_MAX][TPM2_DIGEST_LEN];
    uint8_t policy[TPM2_DIGEST_LEN];
    tpm2_saltkey k;
    tpm2_session s;
    unsigned int i, n = 0;
    int rc;

    if (d == NULL || seed == NULL || len == 0 || len > TPM2_NV_MAX ||
        !mask_ok(pcr_mask)) {
        return TPM_ERR_ARG;
    }
    for (i = 0; i < TPM2_PCR_MAX; i++) {
        if ((pcr_mask >> i) & 1u) {
            rc = tpm2_pcr_read(d, i, vals[n++]);
            if (rc != TPM_OK) {
                return rc;
            }
        }
    }
    rc = tpm2_policy_pcr_digest(pcr_mask, (const uint8_t (*)[32])vals, policy);
    if (rc != TPM_OK) {
        return rc;
    }
    rc = tpm2_nv_define(d, owner_auth, auth_len, index, (uint16_t)len, policy);
    if (rc != TPM_OK) {
        return rc;
    }
    rc = tpm2_saltkey_open(d, owner_auth, auth_len, &k);
    if (rc == TPM_OK) {
        if (key_name != NULL) {
            memcpy(key_name, k.name, TPM2_NAME_LEN);
        }
        rc = tpm2_session_start(d, &k, 0, &s);
        (void)tpm2_saltkey_close(d, &k);
    }
    if (rc == TPM_OK) {
        rc = tpm2_nv_write(d, owner_auth, auth_len, index, seed, len, &s);
        if (rc != TPM_OK) {
            flush_keep_rc(d, s.handle);
        }
        crypto_wipe(&s, sizeof s);
    }
    if (rc != TPM_OK) {
        uint32_t keep = d->rc;

        (void)tpm2_nv_undefine(d, owner_auth, auth_len, index);
        d->rc = keep;
    }
    return rc;
}
