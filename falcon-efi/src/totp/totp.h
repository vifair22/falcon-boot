/* falcon-boot: the UEFI station's TOTP check (DESIGN Section 12.15).
 *
 * Purpose. When STATION.INI has a [totp] section, the station asks the TPM
 * for the seed it holds sealed to the PCRs of a known-good boot, and shows
 * the RFC 6238 code for the firmware clock's time under the banner and
 * again above the passphrase prompt. The user compares it with an
 * authenticator before typing. When the TPM will not release the seed the
 * same places say why, in one or two lines, and the station boots on. A
 * label, like the boot predictions: nothing here enforces or signs.
 *
 * The seed's lifetime. It is read once, before the menu, and kept in this
 * module's static memory until another program is about to run: the ESP
 * and MEM routes' handover, the UKI's StartImage, and before a reset
 * (NXT, firmware setup) or the return to firmware. totp_wipe is called at
 * each. Reading it once is not only cheaper than a round of TPM commands
 * every thirty seconds: it is the only way it works, since anything the
 * station loads extends PCR 4 whether or not it then runs, and a second
 * read after that would be refused as a changed boot. After the wipe the
 * lines say the code is shown again after a restart.
 *
 * Enrollment's half. On a TPM 1.2 behind EFI_TCG_PROTOCOL, totp_save_log
 * saves edk2's event log and the PCRs with it on every boot, [totp] or not:
 * the boot enrollment needs it from is the one before there is a [totp]
 * (tpm/evsave.h says why Linux cannot read it itself).
 *
 * Interactions. tcg/ for the TPM, falcon-core's tpm/ for the unseal and
 * crypto/otp for the code; the menu's status box and the passphrase dialog
 * (tui/tui.h, through pos.c's hook) are where it is drawn.
 */

#ifndef FALCON_TOTP_H
#define FALCON_TOTP_H

#include <efi/uefi.h>
#include <tui/tui.h>
#include <media/stationini.h>

/* Saves the PCRs for enrollment (FalconEvLog, tpm/evsave.h): the SHA-256
 * bank on a TPM 2.0, the SHA-1 PCRs and the firmware's log on a 1.2 behind
 * EFI_TCG_PROTOCOL; nothing without a TPM. Says in FalconEvLogStatus what
 * it saved, or where it stopped. Before anything is loaded. */
void totp_save_log(EFI_SYSTEM_TABLE *st);

/* Reads the seed as cfg says (nothing when cfg is not present). */
void totp_load(EFI_SYSTEM_TABLE *st, const stationini_totp *cfg);

/* Is there anything to show: a [totp] section was loaded. */
BOOLEAN totp_configured(void);

/* The lines for now, for the menu's status box and the passphrase
 * dialog, both of which ask again every second; ctx is the
 * EFI_SYSTEM_TABLE. None when not configured. */
void totp_status(void *ctx, tui_status *out);

/* Wipes the seed; the lines then say the code is gone until a restart. */
void totp_wipe(void);

#endif /* FALCON_TOTP_H */
