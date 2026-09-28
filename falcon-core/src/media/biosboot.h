/* falcon-boot: can falcon-bios boot this medium, and does it need a 64-bit
 * processor (docs/DESIGN.md Section 7.8)?
 *
 * The BIOS menu's two tags, and uefiboot.h's counterpart. The UEFI menu marks
 * a medium no UEFI machine can boot {BIOS ONLY}; this module answers the
 * questions the BIOS menu asks of the same rows, from the medium's own bytes,
 * before anything is chosen:
 *
 *   boots    whether any falcon-bios route can reach the medium. NO is the
 *            row's {EFI ONLY}. Two ways in, and NO only when NEITHER can:
 *
 *              1. Its own BIOS boot image, off the virtual CD: the El Torito
 *                 entry elt_bios_entry picks (media/eltorito.h). No
 *                 emulation and floppy emulation are both served (the
 *                 floppy as a memory disk). Hard-disk emulation is an
 *                 entry the virtual CD refuses today, but it IS a BIOS boot
 *                 image, and a medium that carries one is not called
 *                 EFI-only: that is UNKNOWN, not NO.
 *              2. A kernel, directly (DIR): an entry of the boot
 *                 configuration's CD reading whose kernel carries the x86
 *                 boot protocol's setup header ("HdrS" at 0x202). Any entry
 *                 counts, not just the default one: the rule is written to
 *                 miss a mark rather than make a false one. The tools beside
 *                 the OS (a memory tester, a .c32 module) are not asked,
 *                 in either question, since bootcfg_read never picks them
 *                 (bootcfg_entry_bootable).
 *
 *            Windows media has no kernel for the second way, so for the
 *            Windows family the first is the whole rule.
 *
 *   needs64  whether every way in needs long mode. YES is the row's
 *            {64-BIT ONLY}, shown only on a processor without it:
 *
 *              Linux   every entry of the CD configuration names a kernel
 *                      that could be read, every one is a bzImage whose
 *                      header is new enough to say its width (protocol
 *                      2.12, xloadflags), and every one says 64-bit. One
 *                      32-bit entry is NO, whatever the rest say: a
 *                      multi-arch medium boots on either processor. Any
 *                      kernel that could not be read or judged, with no
 *                      32-bit one beside it, is UNKNOWN.
 *              BSD     /boot/kernel/kernel is an ELF64 for x86-64 (YES) or
 *                      an ELF32 for i386 (NO), as falcon-bios/src/bsd reads
 *                      it.
 *              Windows UNKNOWN. The width is boot.wim's, which is too heavy
 *                      to read for a label; the Windows route refuses a
 *                      64-bit WinPE on such a processor after it is chosen.
 *
 * UNKNOWN is the answer whenever the medium does not PROVE it. The menu marks
 * NO (boots) and YES (needs64) and nothing else, because a false mark would
 * hide a medium that boots.
 *
 * A label, like the boot predictions: the loader refuses a marked row itself,
 * since nothing on it could run on this machine, and no signature or
 * enforcement decision is taken from it (DESIGN Section 7.3).
 *
 * EFI-free and allocation-free; every buffer is the caller's.
 */

#ifndef FALCON_BIOSBOOT_H
#define FALCON_BIOSBOOT_H

#include <stddef.h>
#include <stdint.h>

#include <fs/fs.h>
#include <media/bootcfg.h>
#include <media/bootfs.h>
#include <media/family.h>
#include <media/medium.h>

typedef enum {
    BIOSBOOT_UNKNOWN = 0,
    BIOSBOOT_YES,
    BIOSBOOT_NO
} biosboot_answer;

typedef struct {
    biosboot_answer boots;     /* NO: {EFI ONLY} */
    biosboot_answer needs64;   /* YES: {64-BIT ONLY}, where there is no LM */
} biosboot_result;

/* What a kernel's head says it is, for both questions. */
typedef enum {
    BIOSBOOT_KERNEL_UNKNOWN = 0, /* too short to tell */
    BIOSBOOT_KERNEL_NONE,        /* no setup header: DIR cannot start it */
    BIOSBOOT_KERNEL_OLD,         /* a setup header too old, or cut too short,
                                    to say its width */
    BIOSBOOT_KERNEL_32,
    BIOSBOOT_KERNEL_64
} biosboot_kernel;

/* How much of a kernel's head biosboot_kernel_kind is given: the two
 * sectors falcon-bios reads the setup header out of. */
#define BIOSBOOT_HEAD 1024u

/* The scratch biosboot_medium works in: tens of kilobytes, most of it the
 * parsed configuration, so the caller places it (static). */
typedef struct {
    bootfs_ctx     bctx;
    bootcfg_result cfg;
    fs_file        file;
    fs_dir         walk;
    fs_dirent      ent;
    uint8_t        head[BIOSBOOT_HEAD];
} biosboot_work;

/* The kernel half, on the first len bytes of a kernel file of file_len
 * bytes. A file shorter than a setup header's two sectors is NONE, since
 * the direct route refuses it outright. */
biosboot_kernel biosboot_kernel_kind(const void *head, size_t len,
                                     uint64_t file_len);

/* The BSD kernel's width, from the first len bytes of its ELF file: YES for
 * an ELF64 x86-64 image, NO for an ELF32 i386 one, UNKNOWN otherwise. */
biosboot_answer biosboot_elf_is_64(const void *head, size_t len);

/* The first way in: YES when the medium's El Torito catalog has a BIOS
 * entry the virtual CD serves, NO when it has none (no catalog, or no entry
 * for a BIOS), UNKNOWN for a hard-disk emulation entry, a catalog that did
 * not read or was cut short. */
biosboot_answer biosboot_has_image(medium *m);

/* Both answers over an opened medium. fam is what family_detect made of it;
 * scratch is bootcfg_read's (256 KiB is ample, as for the direct route).
 * Reads the medium; changes nothing. *out is always filled. */
void biosboot_medium(medium *m, family_id fam, biosboot_work *w,
                     char *scratch, size_t scratch_len, biosboot_result *out);

#endif /* FALCON_BIOSBOOT_H */
