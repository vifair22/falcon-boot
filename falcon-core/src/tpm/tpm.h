/* falcon-boot: talking to a TPM, 1.2 or 2.0 (docs/notes/argon2-totp.md
 * Section 2).
 *
 * Purpose. The station's TOTP seed lives in TPM NV storage, readable only
 * when the PCRs match a known-good boot, and travels to falcon encrypted.
 * This module is the command layer for that: it marshals commands, runs the
 * sessions, and checks what comes back. It has no TPM driver.
 *
 * Reasoning. One transport callback carries every command, so the same code
 * runs in the three places that need it: the UEFI station (through
 * EFI_TCG2_PROTOCOL SubmitCommand or EFI_TCG_PROTOCOL PassThroughToTpm),
 * mkstick's enrollment (a write and a read on /dev/tpmrm0 or /dev/tpm0), and
 * the host tests (swtpm's socket, or a recorded transcript). The callback is
 * handed a complete command and fills a response buffer; the framing on
 * either side (the TIS/CRB registers, the kernel's resource manager) is the
 * environment's.
 *
 * Randomness is a second callback. Nonces, the ECDH scalar, the OAEP seed and
 * the transport secret all come from it, and nothing here decides where
 * randomness comes from (EFI_RNG_PROTOCOL, the TPM's GetRandom, getrandom).
 * That also makes a whole exchange reproducible under test.
 *
 * Every buffer is the caller's: the command and response buffers live in the
 * tpm_dev the caller allocates (about 4 KB; the loader makes it static), and
 * there is no allocation anywhere.
 *
 * Interactions. tpm2.h and tpm12.h are the two command sets. mkstick and the
 * UEFI station are the consumers to come; today only the host tests link it.
 */

#ifndef FALCON_TPM_TPM_H
#define FALCON_TPM_TPM_H

#include <stdint.h>
#include <stddef.h>

/* The largest command or response exchanged. TPM 2.0 caps a response at
 * TPM_PT_MAX_RESPONSE_SIZE, 4096 on every part seen; nothing sent here comes
 * near half of it. */
#define TPM_BUF_MAX 2048u

/* Send cmd, receive into rsp (capacity rsp_cap), set *rsp_len. Return 0 on a
 * completed exchange whatever the TPM's response code, nonzero when nothing
 * usable came back. */
typedef int (*tpm_xfer_fn)(void *ctx, const uint8_t *cmd, size_t cmd_len,
                           uint8_t *rsp, size_t rsp_cap, size_t *rsp_len);

/* Fill out with n random bytes; nonzero on failure. */
typedef int (*tpm_rng_fn)(void *ctx, uint8_t *out, size_t n);

typedef struct {
    tpm_xfer_fn xfer;
    void       *xfer_ctx;
    tpm_rng_fn  rng;
    void       *rng_ctx;
    uint32_t    rc;             /* the last response code a TPM returned */
    uint8_t     cmd[TPM_BUF_MAX];
    uint8_t     rsp[TPM_BUF_MAX];
} tpm_dev;

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    TPM_OK          = 0,
    TPM_ERR_ARG     = -1,       /* a caller's argument */
    TPM_ERR_IO      = -2,       /* the transport failed */
    TPM_ERR_PROTO   = -3,       /* a response that does not parse */
    TPM_ERR_RC      = -4,       /* the TPM refused; dev->rc says how */
    TPM_ERR_AUTH    = -5,       /* a response's HMAC did not verify */
    TPM_ERR_RNG     = -6,       /* the randomness callback failed */
    TPM_ERR_KEY     = -7,       /* the TPM key is unusable or not the pinned one */
    TPM_ERR_PCR     = -8,       /* refused because the PCRs do not match */
    TPM_ERR_NOSPACE = -9,       /* a command or response too large */
    TPM_ERR_NVLOCK  = -10       /* TPM 1.2 NV is unlocked: no gate holds */
};

typedef enum {
    TPM_VER_NONE = 0,
    TPM_VER_12   = 12,
    TPM_VER_20   = 20
} tpm_version;

/* Which command set the TPM speaks: a TPM2_GetCapability, which a 1.2 TPM
 * answers with its own response tag. */
int tpm_detect(tpm_dev *d, tpm_version *ver);

#endif /* FALCON_TPM_TPM_H */
