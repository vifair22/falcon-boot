/* falcon-boot: the station's saved event log record. See evsave.h. */

#include <tpm/evsave.h>

#include <lib/string.h>

#define MAGIC   "FEVL"

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return get16(p) | (get16(p + 2) << 16);
}

size_t evsave_size(size_t log_len)
{
    return EVSAVE_BODY + log_len;
}

static void put_hdr(uint8_t *out, uint32_t version, uint32_t dlen,
                    uint32_t log_len)
{
    out[0] = (uint8_t)MAGIC[0];
    out[1] = (uint8_t)MAGIC[1];
    out[2] = (uint8_t)MAGIC[2];
    out[3] = (uint8_t)MAGIC[3];
    put16(out + 4, version);
    put16(out + 6, EVSAVE_PCRS);
    put16(out + 8, dlen);
    put16(out + 10, 0);
    put32(out + 12, log_len);
}

int evsave_write(uint8_t *out, size_t cap,
                 const uint8_t pcr[EVSAVE_PCRS][EVSAVE_DIGEST_LEN],
                 const uint8_t *log, size_t log_len, size_t *out_len)
{
    if (out == NULL || pcr == NULL || log == NULL || out_len == NULL ||
        log_len == 0u || log_len > 0xFFFFFFFFu) {
        return EVSAVE_ERR_ARG;
    }
    if (cap < EVSAVE_BODY || log_len > cap - EVSAVE_BODY) {
        return EVSAVE_ERR_SPACE;
    }
    put_hdr(out, 1u, EVSAVE_DIGEST_LEN, (uint32_t)log_len);
    memcpy(out + EVSAVE_HDR, pcr, EVSAVE_PCR_BYTES);
    memcpy(out + EVSAVE_BODY, log, log_len);
    *out_len = EVSAVE_BODY + log_len;
    return EVSAVE_OK;
}

int evsave_write2(uint8_t *out, size_t cap,
                  const uint8_t pcr[EVSAVE_PCRS][EVSAVE_DIGEST_LEN2],
                  size_t *out_len)
{
    if (out == NULL || pcr == NULL || out_len == NULL) {
        return EVSAVE_ERR_ARG;
    }
    if (cap < EVSAVE_BODY2) {
        return EVSAVE_ERR_SPACE;
    }
    put_hdr(out, 2u, EVSAVE_DIGEST_LEN2, 0u);
    memcpy(out + EVSAVE_HDR, pcr, EVSAVE_BODY2 - EVSAVE_HDR);
    *out_len = EVSAVE_BODY2;
    return EVSAVE_OK;
}

int evsave_read(const uint8_t *buf, size_t len, evsave_view *v)
{
    uint32_t ver;
    uint32_t n;

    if (buf == NULL || v == NULL) {
        return EVSAVE_ERR_ARG;
    }
    if (len < EVSAVE_HDR || memcmp(buf, MAGIC, 4) != 0 ||
        get16(buf + 6) != EVSAVE_PCRS || get16(buf + 10) != 0u) {
        return EVSAVE_ERR_FORMAT;
    }
    ver = get16(buf + 4);
    n = get32(buf + 12);
    if (ver == 1u) {
        if (get16(buf + 8) != EVSAVE_DIGEST_LEN || len < EVSAVE_BODY ||
            n == 0u || n != len - EVSAVE_BODY) {
            return EVSAVE_ERR_FORMAT;
        }
        v->digest_len = EVSAVE_DIGEST_LEN;
        v->log = buf + EVSAVE_BODY;
        v->log_len = n;
    } else if (ver == 2u) {
        if (get16(buf + 8) != EVSAVE_DIGEST_LEN2 || n != 0u ||
            len != EVSAVE_BODY2) {
            return EVSAVE_ERR_FORMAT;
        }
        v->digest_len = EVSAVE_DIGEST_LEN2;
        v->log = NULL;
        v->log_len = 0;
    } else {
        return EVSAVE_ERR_FORMAT;
    }
    v->version = ver;
    v->pcr = buf + EVSAVE_HDR;
    return EVSAVE_OK;
}
