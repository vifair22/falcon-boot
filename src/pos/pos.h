/* airies-boot: the personal-OS boot path (docs/DESIGN.md Section 5).
 *
 * Purpose. One menu entry's worth of work: prompt for the passphrase, unlock
 * p3 (LUKS2, pbkdf2 keyslot), mount the read-only ext4 inside it, read the
 * Unified Kernel Image off that filesystem, and launch it through the
 * shim-verified load path of Section 7.6.
 *
 * Where this sits. It is the module Section 9 calls `loader` for the UKI
 * half: glue over src/luks (unlock), src/ext4 (the filesystem inside) and
 * the shim load path. It writes no crypto and no filesystem code of its own,
 * and it is the only module in the loader that ever holds the passphrase.
 *
 * Security posture, because this path looks superficially like the thing the
 * ISO path forbids. The UKI is handed to LoadImage as a memory BUFFER rather
 * than as a device path, and that is correct here and only here:
 *
 *   - A LUKS volume has no firmware-readable device path. That is the whole
 *     point of encrypting it; there is nothing for the firmware to open.
 *   - Verification is still entirely the platform's. Under Secure Boot the
 *     load goes through shim's image loader, which verifies the bytes in the
 *     buffer against db + MOK + SBAT before it will run them, and refuses
 *     with EFI_SECURITY_VIOLATION when they do not verify (measured, see
 *     docs/notes/shim-spike.md Section 4.2). With Secure Boot off the
 *     firmware's own LoadImage runs it unverified, which is what the platform
 *     means by Secure Boot being off.
 *   - The rule the ISO path follows ("name the image, never hand over bytes",
 *     src/iso/iso.c build_load_path) exists for a different reason: a device
 *     path is what gives the ISO's payload a LoadedImage->DeviceHandle
 *     pointing at the medium it came from, so a distro bootloader can find
 *     its own configuration. It is not a security rule, and applying it here
 *     would not make this path safer, only impossible.
 *
 * So: do not "fix" this into a device-path load, and do not "simplify" the
 * ISO path into a buffer load. Neither change is the other's improvement.
 * DESIGN Section 5 step 4 specifies this shape.
 *
 * Not in this module (DESIGN Section 11 step 6): the passphrase handoff to
 * the initramfs. Today the typed passphrase unlocks p3 and is then wiped;
 * the synthetic credential filesystem that carries it on to systemd-cryptsetup
 * for p4 is the next step, and is the only reason the passphrase would
 * outlive this call.
 */

#ifndef AIRIES_POS_H
#define AIRIES_POS_H

#include <efi/uefi.h>

/* Why an attempt ended, in words fit for the screen. Shaped like
 * iso_boot_result so main.c reports both paths the same way. */
typedef struct {
    const CHAR16 *stage;   /* what was being attempted */
    EFI_STATUS    status;  /* firmware status there, or the UKI's exit status */
    /* One actionable line, or NULL. Carried rather than printed here so the
     * wording for a refusal lives next to the code that knows which refusal
     * it was. */
    const CHAR16 *advice;
} pos_result;

/* Probes every Block I/O handle for a LUKS2 header and records the ones this
 * loader could actually unlock, i.e. those with at least one pbkdf2 keyslot
 * (DESIGN Section 3: p3 is created that way precisely so EFI-land can open
 * it; p4's argon2id slots are recognised and skipped). Costs one block read
 * and one header parse per handle, and never asks for a passphrase, so it is
 * safe to run as part of the startup scan.
 *
 * Returns how many unlockable volumes were found. Zero means the personal-OS
 * entry must not appear in the menu: there is nothing for it to open. */
UINTN pos_scan(EFI_SYSTEM_TABLE *st);

/* The entry's action, start to finish. Prompts, unlocks, mounts, reads and
 * launches; returns when the UKI exits or a stage fails.
 *
 * Returns EFI_SUCCESS when a UKI ran and returned, in which case
 * result->status is its own exit status. EFI_ABORTED means the user pressed
 * ESC and nothing was attempted. Anything else is the status that ended the
 * attempt, with result->stage naming where.
 *
 * Every exit path wipes the passphrase and the recovered volume key. */
EFI_STATUS pos_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                    pos_result *result);

#endif /* AIRIES_POS_H */
