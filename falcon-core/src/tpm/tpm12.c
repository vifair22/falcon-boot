/* falcon-boot: TPM 1.2 commands for the TOTP seed. See tpm12.h.
 *
 * References are to the TPM Main Specification 1.2 rev 116, Parts 2
 * (Structures) and 3 (Commands), and to libtpms' implementation, which
 * swtpm runs and the host tests were settled against. */

#include <tpm/tpm12.h>
#include <tpm/tpmbuf.h>
#include <crypto/crypto.h>
#include <crypto/hmac.h>
#include <crypto/rsa.h>
#include <crypto/sha1.h>
#include <lib/string.h>

#define TAG_RQU        0x00C1u
#define TAG_RQU_AUTH1  0x00C2u
#define TAG_RSP        0x00C4u
#define TAG_RSP_AUTH1  0x00C5u

#define ORD_OIAP               0x0000000Au
#define ORD_OSAP               0x0000000Bu
#define ORD_EXTEND             0x00000014u
#define ORD_GETRANDOM          0x00000046u
#define ORD_PCRREAD            0x00000015u
#define ORD_GETCAPABILITY      0x00000065u
#define ORD_OWNERREADINTPUB    0x00000081u
#define ORD_STARTUP            0x00000099u
#define ORD_CONTINUESELFTEST   0x00000053u
#define ORD_NV_DEFINE          0x000000CCu
#define ORD_NV_WRITE           0x000000CDu
#define ORD_NV_READ            0x000000CFu
#define ORD_ESTABLISHTRANSPORT 0x000000E6u
#define ORD_EXECUTETRANSPORT   0x000000E7u

#define TPM_KH_OWNER      0x40000001u
#define TPM_ET_OWNER      0x0002u
#define TPM_ALG_RSA       0x00000001u
#define TPM_ALG_MGF1      0x00000007u
#define TPM_ES_NONE       0x0001u
#define TPM_TRANSPORT_ENCRYPT 0x00000001u

#define TAG_NV_ATTRIBUTES   0x0017u
#define TAG_NV_DATA_PUBLIC  0x0018u
#define TAG_TRANSPORT_AUTH  0x001Du
#define TAG_TRANSPORT_PUBLIC 0x001Eu
#define TAG_PERMANENT_FLAGS 0x001Fu

#define TPM_CAP_FLAG           0x00000004u
#define TPM_CAP_FLAG_PERMANENT 0x00000108u

#define TPM_NV_PER_OWNERWRITE 0x00000002u
#define TPM_LOC_ALL           0x1Fu

#define RC_WRONGPCRVAL     24u
#define RC_INVALID_POSTINIT 38u

#define AUTH_TRAILER_RSP (20u + 1u + 20u)

/* An authorisation session as the caller holds it: OIAP (key = the
 * entity's usage secret) or OSAP (key = the shared secret). */
typedef struct {
    uint32_t handle;
    uint8_t  nonce_even[TPM12_DIGEST_LEN];
    uint8_t  key[TPM12_DIGEST_LEN];
} auth12;

/* HMAC-SHA1(key, paramDigest || nonceEven || nonceOdd || continue), the
 * one authorisation value every 1.2 command and response carries. */
static void auth_value(const uint8_t key[20], const uint8_t digest[20],
                       const uint8_t even[20], const uint8_t odd[20],
                       uint8_t cont, uint8_t out[20])
{
    hmac_key hk;
    hmac_ctx c;

    (void)hmac_key_init(&hk, hash_sha1(), key, 20);
    hmac_begin(&c, &hk);
    hmac_update(&c, digest, 20);
    hmac_update(&c, even, 20);
    hmac_update(&c, odd, 20);
    hmac_update(&c, &cont, 1);
    hmac_final(&c, out);
    crypto_wipe(&hk, sizeof hk);
}

static void be32put(uint8_t *p, uint32_t v)
{
    tpm_put_be32(p, v);
}

/* Runs one command: handles are sent but not digested, params are both.
 * With an authorisation, the session is used once (continueAuthSession
 * clear) and the response's HMAC is checked. On success *rp and *rpl are the
 * output parameters after out_hlen bytes of output handles. */
static int run12(tpm_dev *d, uint32_t ord, const uint8_t *handles,
                 size_t hlen, const uint8_t *params, size_t plen, auth12 *a,
                 size_t out_hlen, const uint8_t **rp, size_t *rpl)
{
    tpmw w;
    size_t rl, body;
    uint8_t digest[20], odd[20], mac[20];
    uint8_t ordb[4];
    int rc;

    tpmw_init(&w, d->cmd, sizeof d->cmd);
    tpmw_u16(&w, (uint16_t)(a != NULL ? TAG_RQU_AUTH1 : TAG_RQU));
    tpmw_u32(&w, 0);
    tpmw_u32(&w, ord);
    tpmw_bytes(&w, handles, hlen);
    tpmw_bytes(&w, params, plen);
    be32put(ordb, ord);
    if (a != NULL) {
        sha1_ctx h;

        rc = tpm_random(d, odd, sizeof odd);
        if (rc != TPM_OK) {
            return rc;
        }
        sha1_init(&h);
        sha1_update(&h, ordb, 4);
        sha1_update(&h, params, plen);
        sha1_final(&h, digest);
        auth_value(a->key, digest, a->nonce_even, odd, 0, mac);
        tpmw_u32(&w, a->handle);
        tpmw_bytes(&w, odd, sizeof odd);
        tpmw_u8(&w, 0);
        tpmw_bytes(&w, mac, sizeof mac);
    }
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
    if (d->rsp[0] != 0x00u ||
        d->rsp[1] != (uint8_t)(a != NULL ? TAG_RSP_AUTH1 : TAG_RSP)) {
        return TPM_ERR_PROTO;
    }
    body = rl - 10u;
    if (body < out_hlen + (a != NULL ? AUTH_TRAILER_RSP : 0u)) {
        return TPM_ERR_PROTO;
    }
    body -= out_hlen + (a != NULL ? AUTH_TRAILER_RSP : 0u);
    if (a != NULL) {
        const uint8_t *out = d->rsp + 10u + out_hlen;
        const uint8_t *tr = out + body;
        sha1_ctx h;
        uint8_t rcb[4] = { 0, 0, 0, 0 };

        /* outParamDigest = SHA-1(returnCode || ordinal || outParams). */
        sha1_init(&h);
        sha1_update(&h, rcb, 4);
        sha1_update(&h, ordb, 4);
        sha1_update(&h, out, body);
        sha1_final(&h, digest);
        memcpy(a->nonce_even, tr, 20);
        auth_value(a->key, digest, a->nonce_even, odd, tr[20], mac);
        if (!crypto_ct_eq(mac, tr + 21, 20)) {
            return TPM_ERR_AUTH;
        }
    }
    if (rp != NULL) {
        *rp = d->rsp + 10u + out_hlen;
        *rpl = body;
    }
    return TPM_OK;
}

static int oiap(tpm_dev *d, const uint8_t key[20], auth12 *a)
{
    const uint8_t *rp;
    size_t rpl;
    int rc;

    rc = run12(d, ORD_OIAP, NULL, 0, NULL, 0, NULL, 0, &rp, &rpl);
    if (rc != TPM_OK) {
        return rc;
    }
    if (rpl != 24u) {
        return TPM_ERR_PROTO;
    }
    a->handle = tpm_be32(rp);
    memcpy(a->nonce_even, rp + 4, 20);
    memcpy(a->key, key, 20);
    return TPM_OK;
}

/* An OSAP session on the owner: the shared secret is
 * HMAC-SHA1(ownerAuth, nonceEvenOSAP || nonceOddOSAP). */
static int osap_owner(tpm_dev *d, const uint8_t owner_auth[20], auth12 *a)
{
    uint8_t p[2 + 4 + 20];
    uint8_t odd_osap[20];
    const uint8_t *rp;
    size_t rpl;
    int rc;

    rc = tpm_random(d, odd_osap, sizeof odd_osap);
    if (rc != TPM_OK) {
        return rc;
    }
    p[0] = (uint8_t)(TPM_ET_OWNER >> 8);
    p[1] = (uint8_t)TPM_ET_OWNER;
    be32put(p + 2, TPM_KH_OWNER);
    memcpy(p + 6, odd_osap, 20);
    rc = run12(d, ORD_OSAP, NULL, 0, p, sizeof p, NULL, 0, &rp, &rpl);
    if (rc != TPM_OK) {
        return rc;
    }
    if (rpl != 44u) {
        return TPM_ERR_PROTO;
    }
    a->handle = tpm_be32(rp);
    memcpy(a->nonce_even, rp + 4, 20);
    {
        hmac_key hk;
        hmac_ctx c;

        (void)hmac_key_init(&hk, hash_sha1(), owner_auth, 20);
        hmac_begin(&c, &hk);
        hmac_update(&c, rp + 24, 20);
        hmac_update(&c, odd_osap, 20);
        hmac_final(&c, a->key);
        crypto_wipe(&hk, sizeof hk);
    }
    return TPM_OK;
}

static int mask_ok(uint32_t mask)
{
    return (mask >> TPM12_PCR_MAX) == 0u;
}

static void put_pcr_selection(tpmw *w, uint32_t mask)
{
    tpmw_u16(w, 3);
    tpmw_u8(w, (uint8_t)mask);
    tpmw_u8(w, (uint8_t)(mask >> 8));
    tpmw_u8(w, (uint8_t)(mask >> 16));
}

/* ---- PCRs and flags ---- */

int tpm12_startup(tpm_dev *d)
{
    uint8_t p[2] = { 0x00, 0x01 };          /* TPM_ST_CLEAR */
    int rc;

    if (d == NULL) {
        return TPM_ERR_ARG;
    }
    rc = run12(d, ORD_STARTUP, NULL, 0, p, sizeof p, NULL, 0, NULL, NULL);
    if (rc == TPM_ERR_RC && d->rc == RC_INVALID_POSTINIT) {
        rc = TPM_OK;
    }
    return rc;
}

int tpm12_pcr_read(tpm_dev *d, unsigned int pcr, uint8_t out[TPM12_DIGEST_LEN])
{
    uint8_t p[4];
    const uint8_t *rp;
    size_t rpl;
    int rc;

    if (d == NULL || out == NULL || pcr >= TPM12_PCR_MAX) {
        return TPM_ERR_ARG;
    }
    be32put(p, pcr);
    rc = run12(d, ORD_PCRREAD, NULL, 0, p, sizeof p, NULL, 0, &rp, &rpl);
    if (rc != TPM_OK) {
        return rc;
    }
    if (rpl != TPM12_DIGEST_LEN) {
        return TPM_ERR_PROTO;
    }
    memcpy(out, rp, TPM12_DIGEST_LEN);
    return TPM_OK;
}

int tpm12_get_random(tpm_dev *d, uint8_t *out, size_t n)
{
    uint8_t p[4];
    const uint8_t *rp;
    size_t rpl;
    int rc;

    if (d == NULL || out == NULL || n == 0 || n > TPM12_DIGEST_LEN) {
        return TPM_ERR_ARG;
    }
    be32put(p, (uint32_t)n);
    rc = run12(d, ORD_GETRANDOM, NULL, 0, p, sizeof p, NULL, 0, &rp, &rpl);
    if (rc != TPM_OK) {
        return rc;
    }
    /* randomBytesSize, then the bytes; a TPM may return fewer than asked,
     * which is refused rather than padded. */
    if (rpl != 4u + n || tpm_be32(rp) != n) {
        return TPM_ERR_PROTO;
    }
    memcpy(out, rp + 4, n);
    return TPM_OK;
}

int tpm12_extend(tpm_dev *d, unsigned int pcr,
                 const uint8_t digest[TPM12_DIGEST_LEN])
{
    uint8_t p[4 + TPM12_DIGEST_LEN];

    if (d == NULL || digest == NULL || pcr >= TPM12_PCR_MAX) {
        return TPM_ERR_ARG;
    }
    be32put(p, pcr);
    memcpy(p + 4, digest, TPM12_DIGEST_LEN);
    return run12(d, ORD_EXTEND, NULL, 0, p, sizeof p, NULL, 0, NULL, NULL);
}

int tpm12_flags(tpm_dev *d, int *owned, int *nv_locked)
{
    uint8_t p[12];
    const uint8_t *rp;
    size_t rpl;
    int rc;

    if (d == NULL) {
        return TPM_ERR_ARG;
    }
    be32put(p, TPM_CAP_FLAG);
    be32put(p + 4, 4);
    be32put(p + 8, TPM_CAP_FLAG_PERMANENT);
    rc = run12(d, ORD_GETCAPABILITY, NULL, 0, p, sizeof p, NULL, 0, &rp,
               &rpl);
    if (rc != TPM_OK) {
        return rc;
    }
    /* respSize, then TPM_PERMANENT_FLAGS: its tag and one byte per flag in
     * the order of Part 2 Section 7.1; ownership is the second flag and
     * nvLocked the sixteenth. */
    if (rpl < 4u + 2u + 16u || tpm_be32(rp) != rpl - 4u ||
        rp[4] != (uint8_t)(TAG_PERMANENT_FLAGS >> 8) ||
        rp[5] != (uint8_t)TAG_PERMANENT_FLAGS) {
        return TPM_ERR_PROTO;
    }
    if (owned != NULL) {
        *owned = rp[6 + 1] != 0u;
    }
    if (nv_locked != NULL) {
        *nv_locked = rp[6 + 15] != 0u;
    }
    return TPM_OK;
}

int tpm12_composite_digest(uint32_t mask,
                           const uint8_t (*values)[TPM12_DIGEST_LEN],
                           uint8_t out[TPM12_DIGEST_LEN])
{
    uint8_t hdr[5 + 4];
    sha1_ctx h;
    unsigned int i, n = 0;
    tpmw w;

    if (values == NULL || out == NULL || mask == 0u || !mask_ok(mask)) {
        return TPM_ERR_ARG;
    }
    for (i = 0; i < TPM12_PCR_MAX; i++) {
        n += (mask >> i) & 1u;
    }
    /* TPM_PCR_COMPOSITE: select || valueSize || the values (Part 2
     * Section 8.2). */
    tpmw_init(&w, hdr, sizeof hdr);
    put_pcr_selection(&w, mask);
    tpmw_u32(&w, n * TPM12_DIGEST_LEN);
    sha1_init(&h);
    sha1_update(&h, hdr, w.len);
    for (i = 0; i < n; i++) {
        sha1_update(&h, values[i], TPM12_DIGEST_LEN);
    }
    sha1_final(&h, out);
    return TPM_OK;
}

/* TPM_PUBKEY: TPM_KEY_PARMS (RSA, its TPM_RSA_KEY_PARMS inside), then
 * TPM_STORE_PUBKEY. */
static int parse_pubkey(const uint8_t *p, size_t len, tpm12_pubkey *pk)
{
    tpmr r;
    uint32_t parm_size, exp_size, klen;

    tpmr_init(&r, p, len);
    if (tpmr_u32(&r) != TPM_ALG_RSA) {
        return TPM_ERR_KEY;
    }
    (void)tpmr_u16(&r);
    (void)tpmr_u16(&r);
    parm_size = tpmr_u32(&r);
    if (parm_size < 12u) {
        return TPM_ERR_KEY;
    }
    (void)tpmr_u32(&r);                     /* keyLength, in bits */
    (void)tpmr_u32(&r);                     /* numPrimes */
    exp_size = tpmr_u32(&r);
    if (exp_size > sizeof pk->exp || parm_size != 12u + exp_size) {
        return TPM_ERR_KEY;
    }
    if (exp_size == 0u) {
        /* An absent exponent is the default, 2^16 + 1. */
        pk->exp[0] = 0x01;
        pk->exp[1] = 0x00;
        pk->exp[2] = 0x01;
        pk->exp_len = 3;
    } else {
        (void)tpmr_bytes(&r, pk->exp, exp_size);
        pk->exp_len = exp_size;
    }
    klen = tpmr_u32(&r);
    if (klen == 0u || klen > sizeof pk->mod) {
        return TPM_ERR_KEY;
    }
    (void)tpmr_bytes(&r, pk->mod, klen);
    pk->mod_len = klen;
    return (r.err || r.off != len) ? TPM_ERR_PROTO : TPM_OK;
}

int tpm12_srk_pub(tpm_dev *d, const uint8_t owner_auth[TPM12_DIGEST_LEN],
                  tpm12_pubkey *pk)
{
    uint8_t p[4];
    auth12 a;
    const uint8_t *rp;
    size_t rpl;
    int rc;

    if (d == NULL || owner_auth == NULL || pk == NULL) {
        return TPM_ERR_ARG;
    }
    rc = oiap(d, owner_auth, &a);
    if (rc != TPM_OK) {
        return rc;
    }
    /* The key handle is digested here, unlike a loaded key's: it is what
     * stops the SRK being swapped for the EK (Part 3 Section 27.6). */
    be32put(p, TPM12_KH_SRK);
    rc = run12(d, ORD_OWNERREADINTPUB, NULL, 0, p, sizeof p, &a, 0, &rp,
               &rpl);
    crypto_wipe(&a, sizeof a);
    if (rc != TPM_OK) {
        return rc;
    }
    return parse_pubkey(rp, rpl, pk);
}

/* ---- NV ---- */

/* TPM_NV_DATA_PUBLIC (Part 2 Section 19.3). */
static void put_nv_public(tpmw *w, uint32_t index, uint32_t size,
                          uint32_t mask, const uint8_t *digest)
{
    static const uint8_t zero[TPM12_DIGEST_LEN] = { 0 };

    tpmw_u16(w, TAG_NV_DATA_PUBLIC);
    tpmw_u32(w, index);
    put_pcr_selection(w, mask);             /* pcrInfoRead */
    tpmw_u8(w, TPM_LOC_ALL);
    tpmw_bytes(w, digest != NULL ? digest : zero, TPM12_DIGEST_LEN);
    put_pcr_selection(w, 0);                /* pcrInfoWrite: none */
    tpmw_u8(w, TPM_LOC_ALL);
    tpmw_bytes(w, zero, TPM12_DIGEST_LEN);
    tpmw_u16(w, TAG_NV_ATTRIBUTES);
    tpmw_u32(w, TPM_NV_PER_OWNERWRITE);
    tpmw_u8(w, 0);                          /* bReadSTClear */
    tpmw_u8(w, 0);                          /* bWriteSTClear */
    tpmw_u8(w, 0);                          /* bWriteDefine */
    tpmw_u32(w, size);
}

static int nv_define_raw(tpm_dev *d, const uint8_t owner_auth[20],
                         uint32_t index, uint32_t size, uint32_t mask,
                         const uint8_t *digest)
{
    uint8_t p[128];
    tpmw w;
    auth12 a;
    int rc;

    rc = osap_owner(d, owner_auth, &a);
    if (rc != TPM_OK) {
        return rc;
    }
    tpmw_init(&w, p, sizeof p);
    put_nv_public(&w, index, size, mask, digest);
    /* encAuth, the area's own secret, sealed with the OSAP secret (ADIP:
     * XOR with SHA-1(sharedSecret || nonceEven)). Nothing reads with it,
     * since reading takes the PCRs alone and writing the owner, so it is
     * zero before sealing. */
    {
        uint8_t pad[20];
        sha1_ctx h;

        sha1_init(&h);
        sha1_update(&h, a.key, 20);
        sha1_update(&h, a.nonce_even, 20);
        sha1_final(&h, pad);
        tpmw_bytes(&w, pad, sizeof pad);
        crypto_wipe(pad, sizeof pad);
    }
    if (w.err) {
        return TPM_ERR_NOSPACE;
    }
    rc = run12(d, ORD_NV_DEFINE, NULL, 0, p, w.len, &a, 0, NULL, NULL);
    crypto_wipe(&a, sizeof a);
    return rc;
}

int tpm12_nv_define(tpm_dev *d, const uint8_t owner_auth[TPM12_DIGEST_LEN],
                    uint32_t index, uint32_t size, uint32_t pcr_mask,
                    const uint8_t digest[TPM12_DIGEST_LEN])
{
    if (d == NULL || owner_auth == NULL || digest == NULL || size == 0u ||
        size > TPM12_NV_MAX || pcr_mask == 0u || !mask_ok(pcr_mask)) {
        return TPM_ERR_ARG;
    }
    return nv_define_raw(d, owner_auth, index, size, pcr_mask, digest);
}

int tpm12_nv_undefine(tpm_dev *d, const uint8_t owner_auth[TPM12_DIGEST_LEN],
                      uint32_t index)
{
    if (d == NULL || owner_auth == NULL) {
        return TPM_ERR_ARG;
    }
    /* A define of size zero deletes the area (Part 3 Section 20.1). */
    return nv_define_raw(d, owner_auth, index, 0, 0, NULL);
}

int tpm12_nv_write(tpm_dev *d, const uint8_t owner_auth[TPM12_DIGEST_LEN],
                   uint32_t index, const uint8_t *data, size_t len)
{
    uint8_t p[12 + TPM12_NV_MAX];
    auth12 a;
    int rc;

    if (d == NULL || owner_auth == NULL || data == NULL || len == 0u ||
        len > TPM12_NV_MAX) {
        return TPM_ERR_ARG;
    }
    rc = oiap(d, owner_auth, &a);
    if (rc != TPM_OK) {
        return rc;
    }
    be32put(p, index);
    be32put(p + 4, 0);
    be32put(p + 8, (uint32_t)len);
    memcpy(p + 12, data, len);
    rc = run12(d, ORD_NV_WRITE, NULL, 0, p, 12u + len, &a, 0, NULL, NULL);
    crypto_wipe(&a, sizeof a);
    crypto_wipe(p, sizeof p);
    return rc;
}

static int nv_read_result(tpm_dev *d, int rc, const uint8_t *rp, size_t rpl,
                          uint8_t *out, size_t len)
{
    if (rc == TPM_ERR_RC && d->rc == RC_WRONGPCRVAL) {
        return TPM_ERR_PCR;
    }
    if (rc != TPM_OK) {
        return rc;
    }
    if (rpl != 4u + len || tpm_be32(rp) != len) {
        return TPM_ERR_PROTO;
    }
    memcpy(out, rp + 4, len);
    return TPM_OK;
}

int tpm12_nv_read(tpm_dev *d, uint32_t index, uint8_t *out, size_t len)
{
    uint8_t p[12];
    const uint8_t *rp = NULL;
    size_t rpl = 0;
    int rc;

    if (d == NULL || out == NULL || len == 0u || len > TPM12_NV_MAX) {
        return TPM_ERR_ARG;
    }
    be32put(p, index);
    be32put(p + 4, 0);
    be32put(p + 8, (uint32_t)len);
    rc = run12(d, ORD_NV_READ, NULL, 0, p, sizeof p, NULL, 0, &rp, &rpl);
    return nv_read_result(d, rc, rp, rpl, out, len);
}

/* ---- the transport session ---- */

int tpm12_transport_open(tpm_dev *d, const uint8_t srk_auth[TPM12_DIGEST_LEN],
                         const tpm12_pubkey *srk, tpm12_transport *t)
{
    uint8_t p[12 + 4 + 256];               /* transPublic, secretSize, secret */
    uint8_t plain[2 + TPM12_DIGEST_LEN];
    uint8_t seed[20];
    uint8_t h[4];
    static const uint8_t label[4] = { 'T', 'C', 'P', 'A' };
    auth12 a;
    const uint8_t *rp;
    size_t rpl;
    tpmw w;
    int rc;

    if (d == NULL || srk_auth == NULL || srk == NULL || t == NULL ||
        srk->mod_len == 0u || srk->mod_len > sizeof srk->mod) {
        return TPM_ERR_ARG;
    }
    memset(t, 0, sizeof *t);

    /* The session secret, as TPM_TRANSPORT_AUTH, OAEP-encrypted to the SRK
     * with the encoding parameter TPM 1.2 fixes. */
    rc = tpm_random(d, t->secret, sizeof t->secret);
    if (rc == TPM_OK) {
        rc = tpm_random(d, seed, sizeof seed);
    }
    if (rc != TPM_OK) {
        return rc;
    }
    plain[0] = (uint8_t)(TAG_TRANSPORT_AUTH >> 8);
    plain[1] = (uint8_t)TAG_TRANSPORT_AUTH;
    memcpy(plain + 2, t->secret, TPM12_DIGEST_LEN);

    tpmw_init(&w, p, sizeof p);
    tpmw_u16(&w, TAG_TRANSPORT_PUBLIC);
    tpmw_u32(&w, TPM_TRANSPORT_ENCRYPT);
    tpmw_u32(&w, TPM_ALG_MGF1);
    tpmw_u16(&w, TPM_ES_NONE);
    tpmw_u32(&w, (uint32_t)srk->mod_len);
    if (!w.err && w.len + srk->mod_len <= sizeof p &&
        rsa_oaep_sha1_encrypt(srk->mod, srk->mod_len, srk->exp, srk->exp_len,
                              label, sizeof label, plain, sizeof plain, seed,
                              p + w.len) == CRYPTO_OK) {
        w.len += srk->mod_len;
    } else {
        rc = TPM_ERR_KEY;
    }
    crypto_wipe(plain, sizeof plain);
    crypto_wipe(seed, sizeof seed);
    if (rc == TPM_OK) {
        rc = oiap(d, srk_auth, &a);
    }
    if (rc == TPM_OK) {
        be32put(h, TPM12_KH_SRK);
        rc = run12(d, ORD_ESTABLISHTRANSPORT, h, 4, p, w.len, &a, 4, &rp,
                   &rpl);
        crypto_wipe(&a, sizeof a);
    }
    /* transHandle (the output handle), then locality, TPM_CURRENT_TICKS and
     * transNonceEven. */
    if (rc == TPM_OK) {
        if (rpl != 4u + 32u + 20u) {
            rc = TPM_ERR_PROTO;
        } else {
            t->handle = tpm_be32(d->rsp + 10);
            memcpy(t->nonce_even, rp + 36, 20);
        }
    }
    if (rc != TPM_OK) {
        crypto_wipe(t, sizeof *t);
    }
    return rc;
}

/* MGF1 of (nonceEven || nonceOdd || dir || secret), XORed over len bytes:
 * TPM_ALG_MGF1 transport encryption (Part 3 Section 24.2, steps 4 and 17;
 * dir is "in" or "out" without a terminator). */
static void transport_xor(const tpm12_transport *t, const uint8_t odd[20],
                          const char *dir, uint8_t *buf, size_t len)
{
    uint8_t z[20 + 20 + 3 + 20];
    size_t n = 0, dl = (dir[0] == 'i') ? 2u : 3u;

    memcpy(z, t->nonce_even, 20);
    n += 20;
    memcpy(z + n, odd, 20);
    n += 20;
    memcpy(z + n, dir, dl);
    n += dl;
    memcpy(z + n, t->secret, 20);
    n += 20;
    mgf1_sha1_xor(z, n, buf, len);
    crypto_wipe(z, sizeof z);
}

int tpm12_nv_read_transport(tpm_dev *d, tpm12_transport *t, uint32_t index,
                            uint8_t *out, size_t len)
{
    uint8_t wc[22];
    uint8_t odd[20], h1[20], digest[20], mac[20];
    uint8_t ordw[4], ordet[4], wsz[4];
    const uint8_t cont = 0;
    sha1_ctx h;
    tpmw w;
    tpmr r;
    size_t rl, wrl;
    const uint8_t *wr;
    const uint8_t *tr;
    int rc;

    if (d == NULL || t == NULL || out == NULL || len == 0u ||
        len > TPM12_NV_MAX) {
        return TPM_ERR_ARG;
    }
    rc = tpm_random(d, odd, sizeof odd);
    if (rc != TPM_OK) {
        return rc;
    }
    /* The wrapped command: TPM_NV_ReadValue, no authorisation (the area's
     * gate is its PCRs). Its DATAw is everything after the 10-byte header,
     * since it has no handles and no authorisation trailer. */
    be32put(wc, 0);
    wc[0] = 0x00;
    wc[1] = (uint8_t)TAG_RQU;
    be32put(wc + 2, sizeof wc);
    be32put(wc + 6, ORD_NV_READ);
    be32put(wc + 10, index);
    be32put(wc + 14, 0);
    be32put(wc + 18, (uint32_t)len);
    be32put(ordw, ORD_NV_READ);
    be32put(ordet, ORD_EXECUTETRANSPORT);
    be32put(wsz, sizeof wc);

    /* H1 = SHA-1(ORDw || DATAw in the clear); inParamDigest = SHA-1(ORDet ||
     * wrappedCmdSize || H1). */
    sha1_init(&h);
    sha1_update(&h, ordw, 4);
    sha1_update(&h, wc + 10, sizeof wc - 10u);
    sha1_final(&h, h1);
    sha1_init(&h);
    sha1_update(&h, ordet, 4);
    sha1_update(&h, wsz, 4);
    sha1_update(&h, h1, 20);
    sha1_final(&h, digest);
    auth_value(t->secret, digest, t->nonce_even, odd, cont, mac);
    transport_xor(t, odd, "in", wc + 10, sizeof wc - 10u);

    tpmw_init(&w, d->cmd, sizeof d->cmd);
    tpmw_u16(&w, TAG_RQU_AUTH1);
    tpmw_u32(&w, 0);
    tpmw_u32(&w, ORD_EXECUTETRANSPORT);
    tpmw_u32(&w, sizeof wc);
    tpmw_bytes(&w, wc, sizeof wc);
    tpmw_u32(&w, t->handle);
    tpmw_bytes(&w, odd, sizeof odd);
    tpmw_u8(&w, cont);
    tpmw_bytes(&w, mac, sizeof mac);
    rc = tpm_transact(d, w.len, &rl);
    if (rc != TPM_OK) {
        return rc;
    }
    if (d->rc != 0u) {
        return TPM_ERR_RC;
    }

    /* currentTicks, locality, wrappedRspSize, wrappedRsp, then the
     * transport's trailer. */
    tpmr_init(&r, d->rsp, rl);
    (void)tpmr_bytes(&r, NULL, 10);
    (void)tpmr_bytes(&r, NULL, 8 + 4);
    wrl = tpmr_u32(&r);
    wr = tpmr_bytes(&r, NULL, wrl);
    tr = tpmr_bytes(&r, NULL, AUTH_TRAILER_RSP);
    if (r.err || r.off != rl || wrl < 10u || tpm_be32(wr + 2) != wrl) {
        return TPM_ERR_PROTO;
    }
    /* The new nonceEven keys the response's mask; decrypt, then check the
     * HMAC, which covers the plaintext through H2. */
    memcpy(t->nonce_even, tr, 20);
    {
        uint8_t *wrm = d->rsp + (size_t)(wr - d->rsp);
        uint8_t rcw[4], rc0[4] = { 0, 0, 0, 0 };
        uint8_t h2[20];

        transport_xor(t, odd, "out", wrm + 10, wrl - 10u);
        memcpy(rcw, wrm + 6, 4);
        sha1_init(&h);
        sha1_update(&h, rcw, 4);
        sha1_update(&h, ordw, 4);
        sha1_update(&h, wrm + 10, wrl - 10u);
        sha1_final(&h, h2);
        /* outParamDigest = SHA-1(RCet || ORDet || currentTicks || locality
         * || wrappedRspSize || H2). */
        sha1_init(&h);
        sha1_update(&h, rc0, 4);
        sha1_update(&h, ordet, 4);
        sha1_update(&h, d->rsp + 10, 8 + 4 + 4);
        sha1_update(&h, h2, 20);
        sha1_final(&h, digest);
        auth_value(t->secret, digest, t->nonce_even, odd, tr[20], mac);
        if (!crypto_ct_eq(mac, tr + 21, 20)) {
            crypto_wipe(d->rsp, sizeof d->rsp);
            return TPM_ERR_AUTH;
        }
        d->rc = tpm_be32(rcw);
        if (d->rc != 0u) {
            d->ord = ORD_NV_READ;
            rc = TPM_ERR_RC;
        }
        rc = nv_read_result(d, rc, wrm + 10, wrl - 10u, out, len);
        crypto_wipe(d->rsp, sizeof d->rsp);
    }
    crypto_wipe(t, sizeof *t);
    return rc;
}

/* ---- the two whole flows ---- */

int tpm12_seed_read(tpm_dev *d, uint32_t index,
                    const uint8_t *srk_auth, const tpm12_pubkey *srk,
                    uint8_t *seed, size_t len)
{
    static const uint8_t well_known[TPM12_DIGEST_LEN] = { 0 };
    tpm12_transport t;
    int rc;

    if (d == NULL || srk == NULL || seed == NULL) {
        return TPM_ERR_ARG;
    }
    rc = tpm12_transport_open(d, srk_auth != NULL ? srk_auth : well_known,
                              srk, &t);
    if (rc != TPM_OK) {
        return rc;
    }
    /* The session ends with the read whatever it answers (continue clear);
     * a transport error ends it too (Part 3 Section 24.2). */
    return tpm12_nv_read_transport(d, &t, index, seed, len);
}

/* The part of an enrollment after the PCR values are known. */
static int enroll_values(tpm_dev *d, const uint8_t owner_auth[20],
                         uint32_t index, uint32_t pcr_mask,
                         const uint8_t (*values)[TPM12_DIGEST_LEN],
                         const uint8_t *seed, size_t len, tpm12_pubkey *srk)
{
    uint8_t digest[TPM12_DIGEST_LEN];
    int rc;

    rc = tpm12_composite_digest(pcr_mask, values, digest);
    if (rc == TPM_OK) {
        rc = tpm12_srk_pub(d, owner_auth, srk);
    }
    if (rc == TPM_OK) {
        rc = tpm12_nv_define(d, owner_auth, index, (uint32_t)len, pcr_mask,
                             digest);
    }
    if (rc == TPM_OK) {
        rc = tpm12_nv_write(d, owner_auth, index, seed, len);
        if (rc != TPM_OK) {
            (void)tpm12_nv_undefine(d, owner_auth, index);
        }
    }
    return rc;
}

static int enroll_args_ok(const tpm_dev *d, const uint8_t *owner_auth,
                          const uint8_t *seed, size_t len,
                          const tpm12_pubkey *srk, uint32_t pcr_mask)
{
    return d != NULL && owner_auth != NULL && seed != NULL && srk != NULL &&
           len != 0u && len <= TPM12_NV_MAX && pcr_mask != 0u &&
           mask_ok(pcr_mask);
}

/* NV must be locked, or the PCR gate does not hold (tpm12.h). */
static int nv_locked(tpm_dev *d)
{
    int owned = 0, locked = 0;
    int rc = tpm12_flags(d, &owned, &locked);

    if (rc != TPM_OK) {
        return rc;
    }
    return locked ? TPM_OK : TPM_ERR_NVLOCK;
}

int tpm12_seed_enroll(tpm_dev *d, const uint8_t owner_auth[TPM12_DIGEST_LEN],
                      uint32_t index, uint32_t pcr_mask,
                      const uint8_t *seed, size_t len, tpm12_pubkey *srk)
{
    uint8_t vals[TPM12_PCR_MAX][TPM12_DIGEST_LEN];
    unsigned int i, n = 0;
    int rc;

    if (!enroll_args_ok(d, owner_auth, seed, len, srk, pcr_mask)) {
        return TPM_ERR_ARG;
    }
    rc = nv_locked(d);
    if (rc != TPM_OK) {
        return rc;
    }
    for (i = 0; i < TPM12_PCR_MAX; i++) {
        if ((pcr_mask >> i) & 1u) {
            rc = tpm12_pcr_read(d, i, vals[n++]);
            if (rc != TPM_OK) {
                return rc;
            }
        }
    }
    return enroll_values(d, owner_auth, index, pcr_mask,
                         (const uint8_t (*)[TPM12_DIGEST_LEN])vals, seed,
                         len, srk);
}

int tpm12_seed_enroll_values(tpm_dev *d,
                             const uint8_t owner_auth[TPM12_DIGEST_LEN],
                             uint32_t index, uint32_t pcr_mask,
                             const uint8_t (*values)[TPM12_DIGEST_LEN],
                             const uint8_t *seed, size_t len,
                             tpm12_pubkey *srk)
{
    int rc;

    if (!enroll_args_ok(d, owner_auth, seed, len, srk, pcr_mask) ||
        values == NULL) {
        return TPM_ERR_ARG;
    }
    rc = nv_locked(d);
    if (rc != TPM_OK) {
        return rc;
    }
    return enroll_values(d, owner_auth, index, pcr_mask, values, seed, len,
                         srk);
}

const char *tpm12_ord_name(uint32_t ord)
{
    switch (ord) {
    case ORD_CONTINUESELFTEST:   return "ContinueSelfTest";
    case ORD_OIAP:               return "OIAP";
    case ORD_OSAP:               return "OSAP";
    case ORD_GETRANDOM:          return "GetRandom";
    case ORD_PCRREAD:            return "PcrRead";
    case ORD_OWNERREADINTPUB:    return "OwnerReadInternalPub";
    case ORD_NV_DEFINE:          return "NV_DefineSpace";
    case ORD_NV_WRITE:           return "NV_WriteValue";
    case ORD_NV_READ:            return "NV_ReadValue";
    case ORD_ESTABLISHTRANSPORT: return "EstablishTransport";
    case ORD_EXECUTETRANSPORT:   return "ExecuteTransport";
    default:                     return NULL;
    }
}

int tpm12_continue_self_test(tpm_dev *d)
{
    if (d == NULL) {
        return TPM_ERR_ARG;
    }
    return run12(d, ORD_CONTINUESELFTEST, NULL, 0, NULL, 0, NULL, 0, NULL,
                 NULL);
}

int tpm12_rc_retry(uint32_t rc)
{
    return rc == TPM12_RC_RETRY || rc == TPM12_RC_NEEDS_SELFTEST ||
           rc == TPM12_RC_DOING_SELFTEST;
}
