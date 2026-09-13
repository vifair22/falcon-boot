/* airies-boot: ISO boot path (docs/DESIGN.md Section 6).
 *
 * Presents an ISO file living on the exFAT partition as a virtual CD-ROM,
 * lets the firmware's own partition driver parse El Torito and mount the
 * embedded ESP, and chainloads that ESP's \EFI\BOOT\BOOTX64.EFI through the
 * platform's verifying LoadImage. Nothing here verifies, overrides or relaxes
 * anything: the payload is loaded by device path so the firmware (and shim
 * behind it) makes the security decision, exactly as it would for the same
 * media plugged in directly (DESIGN Sections 2 and 7).
 *
 * Family detection and initrd injection (DESIGN Section 6.2) attach to this
 * module at step 4; the stages below are already separate for that reason.
 */

#ifndef AIRIES_ISO_H
#define AIRIES_ISO_H

#include <efi/uefi.h>
#include <exfat/exfat.h>

/* Why a boot attempt ended, in words fit for the menu's status line. */
typedef struct {
    const CHAR16 *stage;   /* what was being attempted */
    EFI_STATUS    status;  /* firmware status there, or the payload's exit */
} iso_boot_result;

/* Boots the ISO, returning when its payload exits or a stage fails. The
 * virtual device is always torn down before returning, so a failed attempt
 * leaves the loader able to try another entry.
 *
 * Returns EFI_SUCCESS when the payload ran and returned, in which case
 * result->status is the payload's own exit status. Any other return value is
 * the firmware status that ended the attempt, and result->stage names where. */
EFI_STATUS iso_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                    exfat_vol *vol, const exfat_dirent *file,
                    iso_boot_result *result);

#endif /* AIRIES_ISO_H */
