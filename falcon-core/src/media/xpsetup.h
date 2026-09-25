/* falcon-core: Windows 2000/XP text-mode setup driver injection (DESIGN
 * Section 6.3, windows-route.md 7.4).
 *
 * 2000 and XP have no WinPE, so winjump cannot run: the loader instead injects
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
 *   [SourceDisksFiles]  <file> = 100,,,,,,4_,4,0,0,,1,4   (atapi.sys's own form)
 *
 * so text-mode setup loads <file> from i386 as a boot-start SCSI-class driver,
 * the way firadisk is loaded. Each new line is inserted right after its section
 * header, and the file's CRLF line ending is kept.
 *
 * `in` is the whole txtsetup.sif (in_len bytes). The grown file is written to
 * `out` (out_cap bytes) and its length returned in *out_len. `svc`, `file` and
 * `desc` are NUL-terminated ASCII. XPSETUP_ERR_NOSECTION if any of the three
 * sections is absent, XPSETUP_ERR_NOSPACE if out_cap cannot hold the result. */
int txtsetup_inject(const char *in, size_t in_len,
                    const char *svc, const char *file, const char *desc,
                    char *out, size_t out_cap, size_t *out_len);

#endif /* FALCON_XPSETUP_H */
