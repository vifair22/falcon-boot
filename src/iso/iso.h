/* airies-boot: virtual-media boot path (docs/DESIGN.md Section 6).
 *
 * Presents a file living on the exFAT partition as a virtual block device,
 * lets the firmware's own drivers parse it, and chainloads the ESP they
 * surface through the platform's verifying LoadImage. Nothing here verifies,
 * overrides or relaxes anything: the payload is loaded by device path so the
 * firmware (and shim behind it) makes the security decision, exactly as it
 * would for the same media plugged in directly (DESIGN Sections 2 and 7).
 *
 * Two media flavors, one publisher (DESIGN Section 6 stage 2): the flavor
 * decides what the firmware is asked to PARSE, never what it is allowed to
 * RUN. Everything past "an ESP surfaced" is the same code for both.
 *
 * Family detection and initrd injection (DESIGN Section 6.2) attach to this
 * module at step 4; the stages below are already separate for that reason.
 */

#ifndef AIRIES_ISO_H
#define AIRIES_ISO_H

#include <efi/uefi.h>
#include <family/family.h>
#include <fs/fs.h>

/* Which shape the virtual device is published as, chosen from the file's
 * extension by the scan (DESIGN Section 6 stage 2).
 *
 * CD is a .iso: 2048-byte blocks, which is the only block size for which the
 * firmware's partition driver goes looking for an El Torito boot catalog.
 * DISK is a .img: 512-byte sectors, so the firmware's GPT/MBR partition path
 * finds an ESP inside it, or its FAT driver binds straight to the whole
 * device for a partition-table-less superfloppy image.
 *
 * It is a parsing hint to the firmware and nothing else. Both flavors reach
 * the same LoadImage on the same terms; neither can make an image load that
 * the platform would otherwise refuse. */
typedef enum {
    ISO_MEDIA_CD = 0,
    ISO_MEDIA_DISK,
} iso_media_kind;

/* Why a boot attempt ended, in words fit for the menu's status line. */
typedef struct {
    const CHAR16 *stage;   /* what was being attempted */
    EFI_STATUS    status;  /* firmware status there, or the payload's exit */
    /* What the ISO's ESP looked like, once it surfaced; NULL when it never
     * did. Informational: it is reported, and nothing on the load path reads
     * it (DESIGN Section 6.2 templates have no consumer yet). */
    const family_desc *family;
} iso_boot_result;

/* Whether the medium's default loader carries an Authenticode signature at
 * all.
 *
 * PRESENCE, never validity. The loader does not and must not evaluate a
 * signature: whether an image may run is the platform's decision through the
 * shim-verified load path (DESIGN Sections 7.2 and 7.6). This exists so a
 * menu entry can say in advance which of the Section 7.2 rows it is likely to
 * land in, which is UX, and any label built from it has to read that way. */
typedef enum {
    ISO_SIG_UNKNOWN = 0,  /* not probed, or the PE headers made no sense */
    ISO_SIG_NO_EFI,       /* no ESP surfaced, or it has no default loader */
    ISO_SIG_ABSENT,       /* PE read, certificate table empty */
    ISO_SIG_PRESENT,      /* PE read, certificate table non-empty */
} iso_sig;

typedef struct {
    const family_desc *family;  /* never NULL after iso_survey_media */
    iso_sig            sig;
} iso_survey;

/* Boots the medium, returning when its payload exits or a stage fails. The
 * virtual device is always torn down before returning, so a failed attempt
 * leaves the loader able to try another entry.
 *
 * Returns EFI_SUCCESS when the payload ran and returned, in which case
 * result->status is the payload's own exit status. Any other return value is
 * the firmware status that ended the attempt, and result->stage names where. */
EFI_STATUS iso_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                    fs_vol *vol, const fs_dirent *file, iso_media_kind kind,
                    iso_boot_result *result);

/* Looks inside the medium without booting anything: builds the same virtual
 * device, lets the firmware surface the ESP, reads the two facts the menu
 * labels with, and tears the device down again. Nothing is loaded and nothing
 * is started, so this cannot run code off the medium.
 *
 * *out is always filled in, including on failure (family unknown, sig
 * NO_EFI/UNKNOWN), because a scan that cannot classify a medium still has to
 * show the entry. The return value is the firmware status for the caller that
 * wants it; a scan may ignore it. */
EFI_STATUS iso_survey_media(EFI_SYSTEM_TABLE *st, fs_vol *vol,
                            const fs_dirent *file, iso_media_kind kind,
                            iso_survey *out);

#endif /* AIRIES_ISO_H */
