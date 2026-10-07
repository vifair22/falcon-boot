/* falcon-boot: the TPM, reached through the firmware (DESIGN Section 12.15).
 *
 * Purpose. falcon-core/src/tpm is the command layer and has no transport;
 * this module is its transport under UEFI and its randomness. A TPM 2.0 is
 * reached through EFI_TCG2_PROTOCOL's SubmitCommand, a TPM 1.2 through
 * EFI_TCG_PROTOCOL's PassThroughToTpm: the firmware owns the TIS or CRB
 * registers, and falcon has no TPM driver.
 *
 * Randomness. The session's nonces and, on 2.0, the ECDH scalar that salts
 * it come from here. A TPM's GetRandom crosses the same bus as the seed, so
 * a bus interposer reads it, and randomness taken from it alone would hand
 * the interposer the session key. So the pool is SHA-256 over every source
 * there is: EFI_RNG_PROTOCOL when the firmware has one, RDRAND when the
 * processor has it, the TPM's GetRandom, the time-stamp counter and the
 * clock. Either of the first two keeps the salt from an interposer; with
 * neither, what is left is the counter's low bits, and the station says
 * nothing about it: the seed is still sealed to the PCRs, only its trip
 * across the bus is weaker. Output is SHA-256(pool || counter).
 *
 * The PCRs for enrollment. The station saves PCRs 0 to 15 on every boot
 * (tpm/evsave.h): tcg_evlog12 copies what edk2 has logged so far on a TPM
 * 1.2 reached through EFI_TCG_PROTOCOL and reads the PCRs after it;
 * tcg_pcrs20 reads the SHA-256 bank of a TPM 2.0, whose firmware's log
 * reaches Linux whole.
 *
 * Interactions. The station's TOTP check (totp/totp.c) is the one caller.
 * The tpm_dev is this module's, static (4 KB of command and response
 * buffers), and tcg_close wipes it with the pool.
 */

#ifndef FALCON_TCG_H
#define FALCON_TCG_H

#include <efi/uefi.h>
#include <tpm/tpm.h>

typedef enum {
    TCG_OK = 0,
    TCG_NO_PROTOCOL = 1,    /* the firmware offers no protocol for it */
    TCG_NOT_PRESENT = 2     /* the protocol is there, the TPM is not usable */
} tcg_status;

/* Where a PCR read for the saved record stopped, for the station's
 * FalconEvLogStatus (totp/totp.c): the step, and what answered there. */
typedef enum {
    TCG_STEP_NONE = 0,
    TCG_STEP_LOCATE,        /* no such protocol */
    TCG_STEP_CAPABILITY,    /* GetCapability or StatusCheck failed: efi */
    TCG_STEP_PRESENT,       /* the protocol says the TPM is not usable */
    TCG_STEP_LOG,           /* 1.2: the log is not where it says, or too big */
    TCG_STEP_PCR_READ       /* a PCR read failed: pcr, err, and rc or efi */
} tcg_step;

typedef struct {
    tcg_step   step;
    unsigned   pcr;
    int        err;         /* tpm.h's TPM_ERR_*, for TCG_STEP_PCR_READ */
    uint32_t   rc;          /* the TPM's response code, for TPM_ERR_RC */
    EFI_STATUS efi;         /* the protocol's status, where one failed */
} tcg_diag;

/* Opens the TPM of version `want` (TPM_VER_20 or TPM_VER_12) and seeds the
 * randomness. *dev is valid until tcg_close. */
tcg_status tcg_open(EFI_SYSTEM_TABLE *st, tpm_version want, tpm_dev **dev);

/* With EFI_TCG_PROTOCOL and no EFI_TCG2_PROTOCOL: the firmware's log as it
 * stands (*len bytes into log, at most cap) and PCRs 0-15, read after it.
 * TCG_NO_PROTOCOL when the TPM is not a 1.2 reached that way, or the log
 * does not fit. Opens and closes the transport itself. */
tcg_status tcg_evlog12(EFI_SYSTEM_TABLE *st, uint8_t *log, size_t cap,
                       size_t *len, uint8_t pcr[16][20], tcg_diag *why);

/* With EFI_TCG2_PROTOCOL: PCRs 0-15 in the SHA-256 bank. TCG_NO_PROTOCOL
 * when there is no such protocol, TCG_NOT_PRESENT when the TPM behind it
 * does not answer. Opens and closes the transport itself. Both say in
 * *why where they stopped. */
tcg_status tcg_pcrs20(EFI_SYSTEM_TABLE *st, uint8_t pcr[16][32],
                      tcg_diag *why);

/* Wipes the command buffers and the randomness pool. */
void tcg_close(void);

#endif /* FALCON_TCG_H */
