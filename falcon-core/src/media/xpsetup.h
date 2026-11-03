/* falcon-core: Windows XP text-mode setup driver injection (DESIGN
 * Section 6.3).
 *
 * XP has no WinPE, so winjump cannot run: the loader instead injects
 * falcon-vdisk as a boot-start driver into text-mode setup by editing the
 * served ISO's i386/txtsetup.sif. This file holds the EFI-free, host-tested
 * text edit; the loader does the ISO 9660 record and serving edits around it.
 */

#ifndef FALCON_XPSETUP_H
#define FALCON_XPSETUP_H

#include <stddef.h>

enum {
    XPSETUP_OK            =  0,
    XPSETUP_ERR_BAD_ARG   = -1,
    XPSETUP_ERR_NOSPACE   = -2,  /* out buffer too small */
    XPSETUP_ERR_NOSECTION = -3   /* a section the edit needs is missing */
};

/* The three sections the injection adds a line to, and the line it adds:
 *
 *   [SCSI.Load]         <svc> = <file>,4
 *   [SCSI]             <svc> = "<desc>"
 *   [SourceDisksFiles]  <file> = <the section's first entry's value>
 *
 * so text-mode setup loads <file> from i386 as a boot-start SCSI-class driver,
 * the way firadisk is loaded. The [SCSI] lines go right after their headers;
 * the [SourceDisksFiles] line follows the section's first real entry (e.g.
 * atapi.sys) and COPIES its value, because the source-disk id and field layout
 * differ per Windows, and a wrong id
 * is rejected as a corrupt [SourceDisksNames] entry. The file's CRLF ending is
 * kept.
 *
 * `in` is the whole txtsetup.sif (in_len bytes). The grown file is written to
 * `out` (out_cap bytes) and its length returned in *out_len. `svc`, `file` and
 * `desc` are NUL-terminated ASCII. XPSETUP_ERR_NOSECTION if any of the three
 * sections is absent, XPSETUP_ERR_NOSPACE if out_cap cannot hold the result. */
int txtsetup_inject(const char *in, size_t in_len,
                    const char *svc, const char *file, const char *desc,
                    char *out, size_t out_cap, size_t *out_len);

/* Adds `AddReg = <inf>,AddReg` to [HiveInfs.Fresh] of the sif in buf (*len bytes,
 * cap at most), in place: setup merges that INF into the new system's registry.
 * Setup also wants the file in [SourceDisksFiles], so it gets a line with the
 * value of `like`'s, which txtsetup_inject has already added.
 * XPSETUP_ERR_NOSECTION if either is absent. */
int txtsetup_add_hive(char *buf, size_t *len, size_t cap, const char *inf,
                      const char *like);

/* 1 if the sif is Windows 2000's, whose [SetupData] says MinorVersion = 0: that
 * media is not supported (DESIGN Section 6.3). */
int txtsetup_is_win2000(const char *buf, size_t len);

/* Moves the USB controller, hub and storage rows from [InputDevicesSupport] to
 * [BootBusExtenders], and from .Load to .Load, in place. Setup records devices of
 * the first kind as still to be installed, which it does from the CD, so a USB
 * stick holding that CD would stay out of reach; those of the second it leaves
 * running. XPSETUP_ERR_NOSECTION if a section is absent. */
int txtsetup_usb_boot_start(char *buf, size_t *len, size_t cap);

#endif /* FALCON_XPSETUP_H */
