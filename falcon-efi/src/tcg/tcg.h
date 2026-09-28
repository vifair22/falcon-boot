/* falcon-boot: the TPM, reached through the firmware (DESIGN Section 13.15).
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

/* Opens the TPM of version `want` (TPM_VER_20 or TPM_VER_12) and seeds the
 * randomness. *dev is valid until tcg_close. */
tcg_status tcg_open(EFI_SYSTEM_TABLE *st, tpm_version want, tpm_dev **dev);

/* Wipes the command buffers and the randomness pool. */
void tcg_close(void);

#endif /* FALCON_TCG_H */
