/* falcon-boot: TPM marshalling and transport. See tpm.h and tpmbuf.h. */

#include <tpm/tpm.h>
#include <tpm/tpmbuf.h>
#include <lib/string.h>

void tpmw_init(tpmw *w, uint8_t *buf, size_t cap)
{
    w->p = buf;
    w->cap = cap;
    w->len = 0;
    w->err = 0;
}

static int tpmw_room(tpmw *w, size_t n)
{
    if (w->err || n > w->cap - w->len) {
        w->err = 1;
        return 0;
    }
    return 1;
}

void tpmw_u8(tpmw *w, uint8_t v)
{
    if (tpmw_room(w, 1)) {
        w->p[w->len++] = v;
    }
}

void tpmw_u16(tpmw *w, uint16_t v)
{
    if (tpmw_room(w, 2)) {
        w->p[w->len++] = (uint8_t)(v >> 8);
        w->p[w->len++] = (uint8_t)v;
    }
}

void tpmw_u32(tpmw *w, uint32_t v)
{
    if (tpmw_room(w, 4)) {
        tpm_put_be32(w->p + w->len, v);
        w->len += 4;
    }
}

void tpmw_bytes(tpmw *w, const void *b, size_t n)
{
    if (n != 0 && tpmw_room(w, n)) {
        memcpy(w->p + w->len, b, n);
        w->len += n;
    }
}

void tpmw_2b(tpmw *w, const void *b, size_t n)
{
    if (n > 0xffffu) {
        w->err = 1;
        return;
    }
    tpmw_u16(w, (uint16_t)n);
    tpmw_bytes(w, b, n);
}

void tpmw_patch32(tpmw *w, size_t off, uint32_t v)
{
    if (!w->err && off + 4u <= w->len) {
        tpm_put_be32(w->p + off, v);
    }
}

void tpmr_init(tpmr *r, const uint8_t *buf, size_t len)
{
    r->p = buf;
    r->len = len;
    r->off = 0;
    r->err = 0;
}

static int tpmr_have(tpmr *r, size_t n)
{
    if (r->err || n > r->len - r->off) {
        r->err = 1;
        return 0;
    }
    return 1;
}

uint8_t tpmr_u8(tpmr *r)
{
    return tpmr_have(r, 1) ? r->p[r->off++] : 0u;
}

uint16_t tpmr_u16(tpmr *r)
{
    uint16_t v;

    if (!tpmr_have(r, 2)) {
        return 0;
    }
    v = (uint16_t)((r->p[r->off] << 8) | r->p[r->off + 1u]);
    r->off += 2;
    return v;
}

uint32_t tpmr_u32(tpmr *r)
{
    uint32_t v;

    if (!tpmr_have(r, 4)) {
        return 0;
    }
    v = tpm_be32(r->p + r->off);
    r->off += 4;
    return v;
}

uint64_t tpmr_u64(tpmr *r)
{
    uint64_t hi = tpmr_u32(r);

    return (hi << 32) | tpmr_u32(r);
}

const uint8_t *tpmr_bytes(tpmr *r, void *out, size_t n)
{
    const uint8_t *at;

    if (!tpmr_have(r, n)) {
        return NULL;
    }
    at = r->p + r->off;
    if (out != NULL && n != 0) {
        memcpy(out, at, n);
    }
    r->off += n;
    return at;
}

void tpmr_2b(tpmr *r, void *out, size_t cap, uint16_t *n)
{
    uint16_t sz = tpmr_u16(r);

    if (sz > cap) {
        r->err = 1;
        sz = 0;
    }
    (void)tpmr_bytes(r, out, sz);
    if (n != NULL) {
        *n = r->err ? 0 : sz;
    }
}

uint32_t tpm_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void tpm_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

int tpm_transact(tpm_dev *d, size_t cmd_len, size_t *rsp_len)
{
    size_t got = 0;

    if (d == NULL || d->xfer == NULL || cmd_len < 10u ||
        cmd_len > sizeof d->cmd) {
        return TPM_ERR_ARG;
    }
    /* Every command's header carries its own length; set it here once
     * rather than in each builder. */
    tpm_put_be32(d->cmd + 2, (uint32_t)cmd_len);
    d->ord = tpm_be32(d->cmd + 6);
    if (d->xfer(d->xfer_ctx, d->cmd, cmd_len, d->rsp, sizeof d->rsp,
                &got) != 0) {
        return TPM_ERR_IO;
    }
    if (got < 10u || got > sizeof d->rsp || tpm_be32(d->rsp + 2) != got) {
        return TPM_ERR_PROTO;
    }
    d->rc = tpm_be32(d->rsp + 6);
    *rsp_len = got;
    return TPM_OK;
}

int tpm_random(tpm_dev *d, uint8_t *out, size_t n)
{
    if (d->rng == NULL || d->rng(d->rng_ctx, out, n) != 0) {
        return TPM_ERR_RNG;
    }
    return TPM_OK;
}

int tpm_detect(tpm_dev *d, tpm_version *ver)
{
    tpmw w;
    size_t rl;
    int rc;
    uint16_t tag;

    if (d == NULL || ver == NULL) {
        return TPM_ERR_ARG;
    }
    *ver = TPM_VER_NONE;
    /* TPM2_GetCapability(TPM_CAP_TPM_PROPERTIES, TPM_PT_FAMILY_INDICATOR, 1).
     * A 1.2 TPM does not know tag 0x8001 and answers TPM_BADTAG under its
     * own response tag, 0x00C4; the tag, not the code, tells them apart. */
    tpmw_init(&w, d->cmd, sizeof d->cmd);
    tpmw_u16(&w, 0x8001u);
    tpmw_u32(&w, 0);
    tpmw_u32(&w, 0x0000017Au);
    tpmw_u32(&w, 0x00000006u);
    tpmw_u32(&w, 0x00000100u);
    tpmw_u32(&w, 1u);
    rc = tpm_transact(d, w.len, &rl);
    if (rc != TPM_OK) {
        return rc;
    }
    tag = (uint16_t)((d->rsp[0] << 8) | d->rsp[1]);
    if (tag == 0x8001u) {
        *ver = TPM_VER_20;
    } else if (tag == 0x00C4u) {
        *ver = TPM_VER_12;
    } else {
        return TPM_ERR_PROTO;
    }
    return TPM_OK;
}
