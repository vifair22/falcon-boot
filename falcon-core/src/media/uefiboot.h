/* falcon-boot: can a UEFI machine of this architecture boot this medium at
 * all (docs/DESIGN.md Sections 6.4 and 7.8)?
 *
 * The UEFI menu lists every image on p2, and a BIOS-only one (an isolinux
 * medium with a kernel that has no EFI stub, Windows XP install media)
 * otherwise looks like any other row until chosen, and fails while looking
 * for an EFI System Partition. This module answers the question the row's
 * {BIOS ONLY} tag states, from the medium's own bytes, before anything is
 * chosen.
 *
 * The machine is the loader's own architecture, PE_MACHINE_AMD64 or
 * PE_MACHINE_ARM64. Two ways the UEFI loader boots an ISO, and a medium is
 * BIOS-only only when NEITHER can reach it:
 *
 *   1. Its own UEFI loader (the chainload, D3a, D3b, Windows, BSD):
 *      \EFI\BOOT\BOOTX64.EFI or Windows' \bootmgr.efi on x86-64,
 *      \EFI\BOOT\BOOTAA64.EFI on AArch64, on any of its volumes. On x86-64
 *      an El Torito entry for platform 0xEF counts too, whatever it points
 *      at: a 0xEF entry holding only BOOTIA32.EFI is a 32-bit UEFI medium,
 *      not a BIOS one, and is left alone.
 *   2. Its kernel, directly (DIR): the kernel the boot configuration's
 *      default CD entry names, which the direct route hands to LoadImage and
 *      which therefore has to be a PE32+ of this architecture, i.e. carry an
 *      EFI stub. A bzImage with no PE header, or a PE for another processor,
 *      is NO.
 *
 * A medium whose only way in is the other architecture's (its removable-
 * media loader, or its kernel) is OTHER: an x86-64 ISO on an AArch64
 * machine, an ARM64 one on an x86-64 machine.
 *
 * Windows media has no kernel for the second route, so for the Windows
 * family the first is the whole rule.
 *
 * UNKNOWN is the answer whenever the medium does not PROVE it: no boot
 * configuration, a kernel that cannot be read, bytes that are neither a PE
 * nor a bzImage. The menu marks NO and nothing else, because a false mark
 * would hide a medium that boots.
 *
 * A label, like the boot predictions: the loader refuses a NO row itself,
 * since there is nothing on it to hand to the platform, but no signature or
 * enforcement decision is taken from it (DESIGN Section 7.3).
 *
 * EFI-free and allocation-free; every buffer is the caller's.
 */

#ifndef FALCON_UEFIBOOT_H
#define FALCON_UEFIBOOT_H

#include <stddef.h>
#include <stdint.h>

#include <fs/fs.h>
#include <media/bootcfg.h>
#include <media/bootfs.h>
#include <media/family.h>
#include <media/medium.h>

typedef enum {
    UEFIBOOT_UNKNOWN = 0,
    UEFIBOOT_YES,
    UEFIBOOT_NO,
    /* Only the other 64-bit architecture's machine can: its loader, or its
     * kernel, and nothing for this one. The {X64 ONLY} or {ARM64 ONLY}
     * row. */
    UEFIBOOT_OTHER
} uefiboot_answer;

/* How much of a kernel's head uefiboot_kernel is given: the setup header
 * ends before 0x300 and every x86 kernel's PE header sits in its first
 * sector or two. */
#define UEFIBOOT_HEAD 4096u

/* The scratch uefiboot_medium works in: a few tens of kilobytes, most of it
 * the parsed configuration, so the caller places it (static, or pool). */
typedef struct {
    bootfs_ctx     bctx;
    bootcfg_result cfg;
    fs_file        kernel;
    fs_dir         walk;
    fs_dirent      ent;
    uint8_t        head[UEFIBOOT_HEAD];
} uefiboot_work;

/* The kernel half: whether this machine's UEFI LoadImage could take the file
 * whose first len bytes are head.
 *
 *   YES      a PE32+ for this machine (an EFI-stub kernel, or any EFI image)
 *   OTHER    a PE32+ for the other one
 *   NO       a PE for anything else, or a bzImage ("HdrS" at 0x202) with no
 *            PE header at all
 *   UNKNOWN  anything else, including an "MZ" whose PE header is past the
 *            window or does not parse
 */
uefiboot_answer uefiboot_kernel(const void *head, size_t len,
                                uint16_t machine);

/* Rule 1 alone: YES when the medium carries this machine's UEFI boot image,
 * OTHER when only the other architecture's, NO when neither. */
uefiboot_answer uefiboot_has_image(medium *m, uefiboot_work *w,
                                   uint16_t machine);

/* The whole rule over an opened medium. fam is what family_detect made of
 * it; scratch is bootcfg_read's (256 KiB is ample, as for the direct route).
 * Reads the medium; changes nothing. */
uefiboot_answer uefiboot_medium(medium *m, family_id fam, uefiboot_work *w,
                                char *scratch, size_t scratch_len,
                                uint16_t machine);

#endif /* FALCON_UEFIBOOT_H */
