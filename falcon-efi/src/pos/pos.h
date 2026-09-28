/* falcon-boot: the personal-OS boot path (docs/DESIGN.md Section 5).
 *
 * Purpose. One menu entry's worth of work: prompt for the passphrase, unlock
 * p3 (LUKS2, an argon2id or pbkdf2 keyslot), mount the read-only ext4 inside it, read the
 * Unified Kernel Image off that filesystem, and launch it through the
 * shim-verified load path of Section 7.6.
 *
 * Where this sits. It is the module Section 9 calls `loader` for the UKI half:
 * glue over falcon-core/src/luks (unlock), falcon-core/src/fs/ext4.c (the
 * filesystem inside) and the shim load path. It writes no crypto and no
 * filesystem code of its own, and it is the only module in the loader that ever
 * holds the passphrase.
 *
 * How the image is handed to LoadImage, and why that changed. A LUKS volume
 * has no firmware-readable device path: that is the point of encrypting it,
 * and it is why this path began life handing LoadImage a memory BUFFER while
 * the ISO path named a device path. The buffer shape verified honestly (shim
 * checks the bytes it is given against db + MOK + SBAT either way), but it
 * left the started image's LoadedImage->DeviceHandle NULL, and sd-stub finds
 * its companion credentials by scanning the directory next to the image on
 * that device handle. No device, no handoff.
 *
 * So this module now builds one: falcon-efi/src/loader/credfs.c publishes the
 * UKI bytes and the credential as a synthetic read-only filesystem in RAM, and
 * the UKI is loaded BY DEVICE PATH off it (DESIGN Sections 5 step 4 and 5.1).
 * The buffer call survives only as the fallback for when that filesystem cannot
 * be published, and it is reported on screen when it is taken, because a silent
 * fallback is a personal OS that asks for the passphrase twice.
 *
 * None of this is a security change, in either direction:
 *
 *   - Verification stays entirely the platform's. Both shapes go through the
 *     shim-verified load path of Section 7.6, which refuses an image that does
 *     not verify with EFI_SECURITY_VIOLATION (measured, docs/notes/
 *     shim-spike.md Section 4.2) and permits everything when Secure Boot is
 *     off. This module installs no security protocol and overrides nothing.
 *   - The credential is DATA handed to an already-verified image, never code
 *     and never a verification decision. Publishing a filesystem changes where
 *     bytes come from, not what is allowed to run.
 *
 * This module is the only one in the loader that ever holds the passphrase,
 * and now the only one that hands it on. Lifetime: the typed buffer is wiped
 * the moment the unlock succeeds, having first been copied into the buffer
 * credfs serves; credfs_teardown wipes that copy on every path out of the
 * launch, successful or not.
 */

#ifndef FALCON_POS_H
#define FALCON_POS_H

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

/* Reads the shelf-role rows of the one device inventory
 * (falcon-efi/src/discover/discover.c, DESIGN Section 9), which are this
 * install's shelves by record (Section 3.1), and records the ones this
 * loader could actually boot from:
 *
 *   - an encrypted shelf, i.e. a LUKS2 volume with at least one argon2id
 *     or pbkdf2 keyslot (DESIGN Section 3). Its contents are unreadable
 *     here, so the header is all that is judged, and whether an argon2id
 *     slot's memory can be had is found out at the unlock;
 *   - a plaintext shelf (Section 3's --p3-plain), which IS readable here, and
 *     is therefore recorded only when a UKI is actually on it.
 *
 * Costs a header parse or a short directory walk per row, and never asks for
 * a passphrase, so it is safe to run as part of the startup scan. Call
 * discover_scan first: this reads its inventory rather than enumerating.
 *
 * Returns how many bootable shelves were found. Zero means the personal-OS
 * entry must not appear in the menu: there is nothing for it to open. */
UINTN pos_scan(EFI_SYSTEM_TABLE *st);

/* What the menu entry should say, which depends on what the scan found: an
 * encrypted shelf prompts for a passphrase and a plaintext one does not.
 * Meaningful only after pos_scan. */
const CHAR16 *pos_entry_label(void);

/* Which image on the shelf an attempt is for.
 *
 * ASK is the personal-OS entry: the shelf's images are offered and the user
 * picks, because on a stick built to DESIGN Section 7.5 the second one is the
 * signing appliance and choosing for them would be choosing wrong half the
 * time. APPLIANCE is the Windows auto-prep path of Section 6.3.1, where the
 * user has already said which thing they want and a chooser would only be a
 * second way to get it wrong; the appliance image is identified by name (see
 * SHELF_APPLIANCE_PREFIX in falcon-core/src/media/shelf.h). */
typedef enum {
    POS_PICK_ASK       = 0,
    POS_PICK_APPLIANCE = 1
} pos_pick;

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

/* The same attempt, for a named image rather than a chosen one. pos_boot is
 * this with POS_PICK_ASK and is not a separate path.
 *
 * EFI_NOT_FOUND with an actionable result->advice is what a shelf carrying no
 * appliance image answers, which includes every --p3-plain stick (DESIGN
 * Section 3: no LUKS on p3 means no on-stick private key and so no signing
 * appliance there at all). */
EFI_STATUS pos_boot_pick(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                         pos_pick want, pos_result *result);

/* ---- the station's rows (DESIGN Section 13.3) ----
 *
 * The station lists a plaintext shelf's UKIs one row each, since their
 * names can be read without a passphrase, and an encrypted shelf as one row
 * that prompts and then offers its chooser. Either may be asked for one
 * image by file name (a STATION.INI `uki` entry, a default, next boot). */

/* Longest UKI file name a row carries, ASCII, terminator included. */
#define POS_FILE_MAX 128u
/* A row's label, in UTF-16 units, terminator included. */
#define POS_LABEL_MAX 128u

typedef struct {
    CHAR16 label[POS_LABEL_MAX];   /* shelf.c's, as the chooser says it */
    char   file[POS_FILE_MAX];     /* its file name on the shelf */
    UINTN  shelf;                  /* for pos_boot_file */
} pos_image;

/* The shelf argument that means "the encrypted shelf, after the prompt". */
#define POS_ENCRYPTED ((UINTN)-1)

/* The UKIs on this install's plaintext shelves, labelled and in the order
 * the chooser would give them (media/shelf.c). Meaningful after pos_scan.
 * Returns how many were written, at most cap. */
UINTN pos_plain_images(EFI_SYSTEM_TABLE *st, pos_image *out, UINTN cap);

/* Is there an encrypted shelf to prompt for? Meaningful after pos_scan. */
BOOLEAN pos_has_encrypted(void);

/* pos_boot for one shelf and one image: `shelf` is a pos_image's, or
 * POS_ENCRYPTED; `file` is the image's file name, matched without regard
 * to case, or NULL for the chooser. EFI_NOT_FOUND with result->advice when
 * the shelf holds no such file. */
EFI_STATUS pos_boot_file(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                         UINTN shelf, const char *file, pos_result *result);

/* Every launch also carries the install's identity, as IDENT_CRED beside
 * the passphrase: the boot partition's record (discover_install) in its text
 * form. The signing appliance opens only partitions carrying that ID (DESIGN
 * Section 3.1); any other image ignores the file. Not a secret. */

/* A second credential for the next launch: the bless list the menu built
 * (falcon-core/src/manifest/blesslist.h), published beside the passphrase as
 * BLESSLIST_CRED in the image's drop-in directory. data must stay valid until
 * the launch returns; NULL or len 0 clears it. Not a secret, and not wiped. */
void pos_set_bless_list(const void *data, UINTN len);

#endif /* FALCON_POS_H */
