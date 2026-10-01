/* falcon-boot: virtual-media boot path (docs/DESIGN.md Section 6).
 *
 * Presents a file living on the exFAT partition as a virtual block device,
 * lets the firmware's own drivers parse it, and chainloads the ESP they
 * surface through the platform's verifying LoadImage, by device path, as for
 * the same media plugged in directly (DESIGN Sections 2 and 7).
 *
 * Two media flavors, one publisher (DESIGN Section 6 stage 2): the flavor
 * decides what the firmware is asked to PARSE, never what it is allowed to
 * RUN. Everything past "an ESP surfaced" is the same code for both.
 */

#ifndef FALCON_ISO_H
#define FALCON_ISO_H

#include <efi/uefi.h>
#include <media/family.h>
#include <media/uefiboot.h>
#include <trust/trust.h>
#include <fs/fs.h>
#include <iso/overlay.h>

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
    /* The boot prediction for the default loader (DESIGN Section 7.8), or
     * TRUST_NO_VERDICT. A label, like everything else in this struct. */
    trust_result       trust;
    /* Whether this machine could boot the medium at all
     * (falcon-core/src/media/uefiboot.h). Asked only of a CD with no default
     * loader (sig NO_EFI); UEFIBOOT_YES or UNKNOWN for everything else, and
     * NO is the {BIOS ONLY} row. */
    uefiboot_answer    uefi;
} iso_survey;

/* Signature presence read out of the head of a PE image: data directory 4,
 * the certificate table, and whether its size field is nonzero.
 *
 * PRESENCE, never validity, exactly as iso_sig says. Exported because a bare
 * EFI payload on p2 (DESIGN Section 7.2) is labelled with the same fact about
 * a file the loader reads itself rather than about an ISO's default loader,
 * and two implementations of "is there a certificate table" could only be two
 * answers to it.
 *
 * buf is the first len bytes of the file; a kilobyte covers every PE header
 * layout that matters. Returns ISO_SIG_UNKNOWN for anything that does not
 * parse as a PE, which includes a read too short to decide. */
iso_sig iso_pe_signature(const void *buf, UINTN len);

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

/* The same boot, with a modified view of the medium (DESIGN Section 6.3 and
 * falcon-efi/src/iso/overlay.h): named byte ranges answered from sidecar files
 * on p2 instead of from the image, and optionally virtual sectors past its end.
 * iso_boot is this with ovl NULL, and is not a separate path.
 *
 * The overlay is an ARGUMENT rather than a setter on a device, which is what
 * makes "installed on an already-published device" unrepresentable: the device
 * does not exist yet when this is called, and it is torn down before this
 * returns. There is no handle to hang a second set on.
 *
 * ovl is copied, so the caller's struct need not outlive the call. The fs_file
 * objects it names must: they are read on every block the firmware asks for,
 * they must already be open, and neither they nor the volume they were opened
 * on may go away until this returns. They must also be distinct from the image
 * itself, which is refused.
 *
 * The set is validated in full before anything is published and any failure
 * refuses the boot outright (see iso_overlay_check). It is untrusted data off
 * a writable partition; a partially honoured byte map would be indistinguish-
 * able from a correct one at the point where it matters.
 *
 * The overlay serves bytes; the image is verified as served. The payload is
 * named to the platform's LoadImage by device path (DESIGN Sections 7.2 and
 * 7.6), so a payload whose bytes were substituted is verified as any other
 * is. */
EFI_STATUS iso_boot_overlaid(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                             fs_vol *vol, const fs_dirent *file,
                             iso_media_kind kind, const iso_overlay *ovl,
                             iso_boot_result *result);

/* The device path of \EFI\BOOT\BOOTX64.EFI on the medium's boot volume,
 * from the pool; the caller frees it. Valid only while the medium is
 * published.
 *
 * It exists for one caller: a blessed BSD medium starts a signed copy of the
 * medium's own loader, held on p2, and that loader has to believe it came
 * from the medium, because the medium is where it reads its kernel and its
 * memory disk from. The bytes are named to the platform as this path while
 * being handed over as a buffer (DESIGN Section 6.4). */
EFI_STATUS iso_boot_path(EFI_BOOT_SERVICES *bs, EFI_HANDLE esp_handle,
                         EFI_DEVICE_PATH_PROTOCOL **out);

/* What runs while the medium is published, when it is not the medium's own
 * \EFI\BOOT\BOOTX64.EFI. esp_handle is the medium's boot volume, for
 * iso_boot_path above. Sets result->stage and result->status the way the
 * chainload does. */
typedef EFI_STATUS (*iso_served_fn)(void *ctx, EFI_SYSTEM_TABLE *st,
                                    EFI_HANDLE parent_image,
                                    EFI_HANDLE esp_handle,
                                    iso_boot_result *result);

/* The same publish, with the caller deciding what to start.
 *
 * It exists for one case: a blessed BSD medium (DESIGN Section 6.4), where
 * the image the platform verifies is a signed copy of the ISO's own loader
 * held on p2, and the medium still has to be published beside it, because
 * that is where that loader reads its kernel and its memory disk from. The
 * callback runs with the device published and the firmware's drivers bound to
 * it, and everything is torn down afterwards exactly as it is for a
 * chainload.
 *
 * What the callback starts goes through the shim-verified load path (DESIGN
 * Section 7.6); this module never sees the image. */
EFI_STATUS iso_boot_overlaid_served(EFI_SYSTEM_TABLE *st,
                                    EFI_HANDLE parent_image, fs_vol *vol,
                                    const fs_dirent *file,
                                    iso_media_kind kind,
                                    const iso_overlay *ovl,
                                    iso_served_fn served, void *ctx,
                                    iso_boot_result *result);

/* Looks inside the medium without booting anything: opens the image file on
 * vol with falcon-core (media/medium.c), with no device published and no
 * firmware driver bound, and reads what the menu labels with: the family,
 * whether the default loader is signed, whether a UEFI machine can boot it,
 * and the boot prediction. Nothing is loaded and nothing is started, so this
 * cannot run code off the medium.
 *
 * *out is always filled in, including on failure (family unknown, sig
 * NO_EFI/UNKNOWN), because a scan that cannot classify a medium still has to
 * show the entry. The return value is the status for the caller that wants
 * it; a scan may ignore it. */
EFI_STATUS iso_survey_media(EFI_SYSTEM_TABLE *st, fs_vol *vol,
                            const fs_dirent *file, iso_media_kind kind,
                            const trust_stores *stores, iso_survey *out);

#endif /* FALCON_ISO_H */
