/* falcon-boot: the TPM 2.0 commands the TOTP seed needs
 * (docs/notes/argon2-totp.md Section 2).
 *
 * The seed is an NV index whose only read authorisation is a policy: a
 * PolicyPCR over the PCRs chosen at enrollment, bound to their values then
 * (TPMA_NV_POLICYREAD, authPolicy = tpm2_policy_pcr_digest). Writing it takes
 * the owner hierarchy's authorisation (TPMA_NV_OWNERWRITE), as defining and
 * removing it already do, so enrollment needs one secret and falcon none.
 *
 * The read runs in a policy session salted to a TPM key by ECDH on P-256,
 * with AES-128-CFB response encryption: the seed crosses the bus as
 * ciphertext under a key derived from a salt only the TPM and falcon hold.
 * The response HMAC is checked, so a response that was tampered with or
 * came from something other than the TPM that holds the salt is refused.
 *
 * The salt key is the one at TPM2_SRK_HANDLE when that is an ECC P-256
 * storage key, else a primary made from TPM2_SRK_TEMPLATE under the owner
 * hierarchy (deterministic: the same seed and template give the same key
 * every boot) and flushed afterwards. Its name can be pinned: enrollment
 * records it, and a read that finds another key refuses with TPM_ERR_KEY,
 * which is what stops an active interposer handing falcon its own key.
 *
 * Interactions. tpm.h for the transport; crypto/p256, kdf, cfb and hmac.
 */

#ifndef FALCON_TPM_TPM2_H
#define FALCON_TPM_TPM2_H

#include <stdint.h>
#include <stddef.h>

#include <tpm/tpm.h>

#define TPM2_DIGEST_LEN 32u             /* SHA-256, the only bank used */
#define TPM2_NAME_LEN   34u             /* nameAlg (2) + SHA-256 */
#define TPM2_NV_MAX     64u             /* the largest NV payload handled */

#define TPM2_RH_OWNER     0x40000001u
#define TPM2_SRK_HANDLE   0x81000001u   /* the conventional persistent SRK */

/* The TOTP seed's index, in the owner range 0x01800000-0x01BFFFFF (TCG
 * Handles Registry). tpm2-totp's default is 0x018094AF; this is apart
 * from it. */
#define TPM2_TOTP_INDEX   0x01800FA1u

/* PCRs are a bit mask, bit i for PCR i, 0 to 23. */
#define TPM2_PCR_MAX      24u

typedef struct {
    uint32_t handle;
    uint8_t  transient;                 /* made here, so flushed by close */
    uint8_t  x[32], y[32];              /* the public point */
    uint8_t  name[TPM2_NAME_LEN];
} tpm2_saltkey;

typedef struct {
    uint32_t handle;
    uint8_t  key[TPM2_DIGEST_LEN];      /* sessionKey */
    uint8_t  nonce_tpm[TPM2_DIGEST_LEN];
    uint8_t  nonce_caller[TPM2_DIGEST_LEN];
    uint16_t nonce_tpm_len;
} tpm2_session;

/* For the rig; the firmware does this on a real machine. */
int tpm2_startup(tpm_dev *d);

int tpm2_pcr_read(tpm_dev *d, unsigned int pcr, uint8_t out[TPM2_DIGEST_LEN]);
int tpm2_pcr_extend(tpm_dev *d, unsigned int pcr,
                    const uint8_t digest[TPM2_DIGEST_LEN]);

/* The PolicyPCR policy digest over the PCRs in mask with the given values,
 * one per set bit in ascending PCR order, starting from the empty policy. */
int tpm2_policy_pcr_digest(uint32_t mask,
                           const uint8_t (*values)[TPM2_DIGEST_LEN],
                           uint8_t out[TPM2_DIGEST_LEN]);

/* The salt key (see above). owner_auth may be empty. */
int tpm2_saltkey_open(tpm_dev *d, const uint8_t *owner_auth, size_t auth_len,
                      tpm2_saltkey *k);
int tpm2_saltkey_close(tpm_dev *d, tpm2_saltkey *k);

/* A session salted to k, AES-128-CFB, SHA-256; policy or HMAC. */
int tpm2_session_start(tpm_dev *d, const tpm2_saltkey *k, int policy,
                       tpm2_session *s);
int tpm2_policy_pcr(tpm_dev *d, const tpm2_session *s, uint32_t mask);
int tpm2_flush(tpm_dev *d, uint32_t handle);

int tpm2_nv_define(tpm_dev *d, const uint8_t *owner_auth, size_t auth_len,
                   uint32_t index, uint16_t size,
                   const uint8_t policy[TPM2_DIGEST_LEN]);
int tpm2_nv_undefine(tpm_dev *d, const uint8_t *owner_auth, size_t auth_len,
                     uint32_t index);
/* enc, when given, is an HMAC session that carries data encrypted. */
int tpm2_nv_write(tpm_dev *d, const uint8_t *owner_auth, size_t auth_len,
                  uint32_t index, const uint8_t *data, size_t len,
                  tpm2_session *enc);
/* The index's name (NV_ReadPublic), and its data size. */
int tpm2_nv_name(tpm_dev *d, uint32_t index, uint8_t name[TPM2_NAME_LEN],
                 uint16_t *size);
/* Reads len bytes under policy session s (already through PolicyPCR), with
 * the response encrypted. The session ends with the command. */
int tpm2_nv_read_policy(tpm_dev *d, tpm2_session *s, uint32_t index,
                        const uint8_t name[TPM2_NAME_LEN],
                        uint8_t *out, size_t len);

/* The whole unseal: salt key, session, PolicyPCR, encrypted read, cleanup.
 * pin, when given, is the salt key name recorded at enrollment.
 * TPM_ERR_PCR when the PCRs do not match. */
int tpm2_seed_read(tpm_dev *d, uint32_t index, uint32_t pcr_mask,
                   const uint8_t *pin, uint8_t *seed, size_t len);

/* The whole enrollment: reads the PCRs in mask, defines the index sealed
 * to them, writes the seed encrypted; the salt key's name to key_name. */
int tpm2_seed_enroll(tpm_dev *d, const uint8_t *owner_auth, size_t auth_len,
                     uint32_t index, uint32_t pcr_mask,
                     const uint8_t *seed, size_t len,
                     uint8_t key_name[TPM2_NAME_LEN]);

#endif /* FALCON_TPM_TPM2_H */
