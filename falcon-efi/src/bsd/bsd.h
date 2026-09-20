/* falcon-boot: BSD install media, booted as its own memory disk
 * (docs/DESIGN.md Section 6.4; docs/notes/bsd-route.md; M7).
 *
 * A FreeBSD kernel loses the virtual CD at ExitBootServices exactly as a
 * Linux one does, and falcon-hook cannot follow it: the hook is an initramfs
 * and device-mapper mechanism and FreeBSD has neither. What FreeBSD's loader
 * has instead is a memory disk: it will load a file into RAM before it starts
 * the kernel, and the kernel will mount root from it. No kernel module, and
 * nothing that has to be rebuilt per release, which is what the GEOM-module
 * route Ventoy takes would cost (docs/notes/ventoy-survey.md).
 *
 * The file it loads is the ISO, read off the virtual CD the loader is already
 * serving: falcon-core/src/media/bsdmd.c adds one directory record naming the
 * medium as a file inside itself, and writes the four md_image settings over
 * the opening comments of the ISO's /boot/defaults/loader.conf. Both are byte
 * ranges served through the overlay (iso/overlay.h), so nothing on p2 is
 * written and no ISO9660 length changes.
 *
 * The cost is RAM: the image is copied into memory below 4 GB, and FreeBSD's
 * loader gives up with "Unable to expand staging area" when it will not fit.
 * bsd_boot checks that the memory is there before it serves anything, and
 * says so plainly when it is not, rather than letting the boot die halfway
 * through the copy with a message only FreeBSD's source explains.
 */

#ifndef FALCON_BSD_H
#define FALCON_BSD_H

#include <efi/uefi.h>
#include <fs/fs.h>
#include <bless/bless.h>

typedef struct {
    const CHAR16 *stage;    /* what was being attempted */
    EFI_STATUS    status;   /* firmware status there, or the loader's exit */
    /* Set when this medium is not one the route applies to, before anything
     * was served: the caller's cue to chainload instead. */
    UINT8         not_bsd;
    /* One line to add to the failure, or NULL. */
    const CHAR16 *advice;
} bsd_result;

/* Serves the ISO with the two edits in place and starts its own loader.
 *
 * vol is p2 and file the ISO on it. Returns the status of the started image,
 * or an error with *result filled in. */
EFI_STATUS bsd_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                    fs_vol *vol, const fs_dirent *file, bsd_result *result);

/* The same, starting the blessed loader the hit names instead of the
 * medium's own (roadmap M7 part 2).
 *
 * Blessing a BSD medium signs the medium's own loader.efi with the user's
 * MOK, because there is no Linux kernel and no initramfs for a UKI to wrap.
 * The medium is still published with the two edits, because that signed
 * loader reads its kernel and its memory disk from it.
 *
 * *bres carries the launch's own outcome, in the words boot_blessed already
 * prints. Returns what bless_boot returned. */
EFI_STATUS bsd_boot_blessed(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                            fs_vol *vol, const fs_dirent *file,
                            const bless_hit *hit, bless_result *bres,
                            bsd_result *result);

#endif /* FALCON_BSD_H */
