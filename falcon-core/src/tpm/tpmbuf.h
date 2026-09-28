/* falcon-boot: big-endian marshalling for TPM commands, shared by the 1.2 and
 * 2.0 command sets. Internal to falcon-core/src/tpm.
 *
 * Both writer and reader keep a sticky error: after the first overrun every
 * later call is a no-op and the error survives to the one check at the end,
 * so a command builder is a straight run of puts with no test between them,
 * and a response parser cannot read past what the TPM sent. */

#ifndef FALCON_TPM_TPMBUF_H
#define FALCON_TPM_TPMBUF_H

#include <stdint.h>
#include <stddef.h>

#include <tpm/tpm.h>

typedef struct {
    uint8_t *p;
    size_t   cap;
    size_t   len;
    int      err;
} tpmw;

typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         off;
    int            err;
} tpmr;

void tpmw_init(tpmw *w, uint8_t *buf, size_t cap);
void tpmw_u8(tpmw *w, uint8_t v);
void tpmw_u16(tpmw *w, uint16_t v);
void tpmw_u32(tpmw *w, uint32_t v);
void tpmw_bytes(tpmw *w, const void *b, size_t n);
/* A TPM2B: u16 size, then the bytes. */
void tpmw_2b(tpmw *w, const void *b, size_t n);
/* Patches a u32 at off (a command's size field, a TPM 2.0 authorization
 * area's size). */
void tpmw_patch32(tpmw *w, size_t off, uint32_t v);

void tpmr_init(tpmr *r, const uint8_t *buf, size_t len);
uint8_t  tpmr_u8(tpmr *r);
uint16_t tpmr_u16(tpmr *r);
uint32_t tpmr_u32(tpmr *r);
uint64_t tpmr_u64(tpmr *r);
/* Copies n bytes out, or points at them when out is NULL (returns the
 * pointer, NULL on overrun). */
const uint8_t *tpmr_bytes(tpmr *r, void *out, size_t n);
/* A TPM2B into out (capacity cap); its size to *n. */
void tpmr_2b(tpmr *r, void *out, size_t cap, uint16_t *n);

uint32_t tpm_be32(const uint8_t *p);
void     tpm_put_be32(uint8_t *p, uint32_t v);

/* Sends d->cmd[0..cmd_len), receives into d->rsp, checks the response header
 * (length agrees, at least 10 bytes) and records its code in d->rc. Returns
 * TPM_OK with *rsp_len set whatever the code; the caller judges d->rc. */
int tpm_transact(tpm_dev *d, size_t cmd_len, size_t *rsp_len);

/* Random bytes through the caller's callback. */
int tpm_random(tpm_dev *d, uint8_t *out, size_t n);

#endif /* FALCON_TPM_TPMBUF_H */
