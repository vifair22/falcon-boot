/* falcon-boot: the TPM 1.2 commands the TOTP seed needs
 * (docs/notes/argon2-totp.md Section 2).
 *
 * The seed is an NV area whose pcrInfoRead names the PCRs chosen at
 * enrollment and their composite then (digestAtRelease), with no read
 * authorisation of its own: the TPM releases it to anyone exactly when the
 * PCRs match. Writing takes the owner (TPM_NV_PER_OWNERWRITE), as defining
 * and removing already do.
 *
 * That gate holds only once NV is locked (TPM_PERMANENT_FLAGS nvLocked; the
 * PC Client specification has the manufacturer set it). With nvLocked clear
 * a 1.2 TPM skips every NV authorisation check, PCRs included, so
 * enrollment refuses such a TPM with TPM_ERR_NVLOCK.
 *
 * The read travels in a transport session with TPM_TRANSPORT_ENCRYPT and
 * TPM_ALG_MGF1 (TPM Main Part 3 Section 24): falcon picks the session secret,
 * encrypts it to the SRK with RSAES-OAEP, and the TPM XORs the wrapped
 * response with MGF1 of that secret and the session's nonces. MGF1 rather
 * than AES-CTR because it is the one every 1.2 TPM must implement. The
 * session is authorised with the SRK's usage secret, which is the
 * well-known all-zero value on every TPM set up the common way (Windows,
 * TrouSerS -z, swtpm_setup --srk-well-known).
 *
 * The SRK's public key cannot be read at boot without the owner (readSRKPub
 * is off after TakeOwnership), so enrollment reads it with the owner's
 * authorisation (TPM_OwnerReadInternalPub) and the station keeps it. That is
 * also the pin: the secret is only ever encrypted to the key enrollment saw.
 *
 * Interactions. tpm.h for the transport; crypto/rsa and hmac (SHA-1).
 */

#ifndef FALCON_TPM_TPM12_H
#define FALCON_TPM_TPM12_H

#include <stdint.h>
#include <stddef.h>

#include <tpm/tpm.h>

#define TPM12_DIGEST_LEN 20u
#define TPM12_PCR_MAX    24u
#define TPM12_NV_MAX     64u
#define TPM12_KH_SRK     0x40000000u

/* The TOTP seed's NV index: the U bit (0x20000000, not defined by the
 * TCG), purview 0, and the same low half as the 2.0 index. The D bit is
 * clear, so the owner can delete it. */
#define TPM12_TOTP_INDEX 0x20000FA1u

typedef struct {
    uint32_t handle;
    uint8_t  nonce_even[TPM12_DIGEST_LEN];
    uint8_t  secret[TPM12_DIGEST_LEN];     /* the transport's authData */
} tpm12_transport;

/* An RSA public key as TPM_PUBKEY carries it. */
typedef struct {
    uint8_t  mod[256];
    size_t   mod_len;
    uint8_t  exp[4];
    size_t   exp_len;
} tpm12_pubkey;

/* For the rig; the BIOS does this on a real machine. */
int tpm12_startup(tpm_dev *d);

int tpm12_pcr_read(tpm_dev *d, unsigned int pcr, uint8_t out[TPM12_DIGEST_LEN]);
/* n random bytes from the TPM (TPM_GetRandom), at most 20. In the clear,
 * as tpm2_get_random. */
int tpm12_get_random(tpm_dev *d, uint8_t *out, size_t n);

int tpm12_extend(tpm_dev *d, unsigned int pcr,
                 const uint8_t digest[TPM12_DIGEST_LEN]);

/* TPM_PERMANENT_FLAGS ownership and nvLocked. */
int tpm12_flags(tpm_dev *d, int *owned, int *nv_locked);

/* digestAtRelease: SHA-1 of the TPM_PCR_COMPOSITE of the PCRs in mask with
 * the given values, one per set bit in ascending order. */
int tpm12_composite_digest(uint32_t mask,
                           const uint8_t (*values)[TPM12_DIGEST_LEN],
                           uint8_t out[TPM12_DIGEST_LEN]);

/* The SRK's public key, read with the owner's authorisation. */
int tpm12_srk_pub(tpm_dev *d, const uint8_t owner_auth[TPM12_DIGEST_LEN],
                  tpm12_pubkey *pk);

int tpm12_nv_define(tpm_dev *d, const uint8_t owner_auth[TPM12_DIGEST_LEN],
                    uint32_t index, uint32_t size, uint32_t pcr_mask,
                    const uint8_t digest[TPM12_DIGEST_LEN]);
int tpm12_nv_undefine(tpm_dev *d, const uint8_t owner_auth[TPM12_DIGEST_LEN],
                      uint32_t index);
int tpm12_nv_write(tpm_dev *d, const uint8_t owner_auth[TPM12_DIGEST_LEN],
                   uint32_t index, const uint8_t *data, size_t len);
/* A read in the clear, no session: what the transport read replaces. */
int tpm12_nv_read(tpm_dev *d, uint32_t index, uint8_t *out, size_t len);

/* An encrypting transport session to the SRK. */
int tpm12_transport_open(tpm_dev *d, const uint8_t srk_auth[TPM12_DIGEST_LEN],
                         const tpm12_pubkey *srk, tpm12_transport *t);
/* NV_ReadValue wrapped in t; the session ends with it. TPM_ERR_PCR when the
 * PCRs do not match. */
int tpm12_nv_read_transport(tpm_dev *d, tpm12_transport *t, uint32_t index,
                            uint8_t *out, size_t len);

/* The command a refusal came from, by name (dev->ord): the steps of the
 * seed's read and enrollment. NULL for any other ordinal. */
const char *tpm12_ord_name(uint32_t ord);

/* The whole unseal. srk_auth NULL means the well-known value. */
int tpm12_seed_read(tpm_dev *d, uint32_t index,
                    const uint8_t *srk_auth, const tpm12_pubkey *srk,
                    uint8_t *seed, size_t len);

/* The whole enrollment: NV locked, the PCRs read, the area defined and
 * written, the SRK's public key to srk for the station to keep. */
int tpm12_seed_enroll(tpm_dev *d, const uint8_t owner_auth[TPM12_DIGEST_LEN],
                      uint32_t index, uint32_t pcr_mask,
                      const uint8_t *seed, size_t len, tpm12_pubkey *srk);

/* The same with the PCR values given rather than read (tpm2.h's
 * tpm2_seed_enroll_values has why). */
int tpm12_seed_enroll_values(tpm_dev *d,
                             const uint8_t owner_auth[TPM12_DIGEST_LEN],
                             uint32_t index, uint32_t pcr_mask,
                             const uint8_t (*values)[TPM12_DIGEST_LEN],
                             const uint8_t *seed, size_t len,
                             tpm12_pubkey *srk);

#endif /* FALCON_TPM_TPM12_H */
