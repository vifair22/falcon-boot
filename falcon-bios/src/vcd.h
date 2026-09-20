/* falcon-bios: serving an ISO as a CD to its own bootloader. See vcd.c. */

#ifndef FALCON_BIOS_VCD_H
#define FALCON_BIOS_VCD_H

#include <bios.h>
#include <fs/fs.h>
#include <part/part.h>

/* The drive number the served medium answers on.
 *
 * 0xE0 is the first of the range El Torito reserves for CDs and is what a
 * BIOS that boots one from an ISO hands over in DL, which is what the loaded
 * program reads to find its own medium. */
#define VCD_DRIVE 0xE0u

/* Installs the resident handler over the ISO named by `iso` on the mounted
 * p2 volume, loads the medium's own boot image where El Torito says, and
 * hands the machine to it. Does not return on success.
 *
 * Returns nonzero with a message already printed when the medium cannot be
 * served: no BIOS boot image in its catalog, or an ISO in more pieces on p2
 * than the handler will scan. */
int vcd_chainload(bios_disk *d, fs_vol *p2, const gpt_part *p2part,
                  const fs_dirent *iso, fs_file *isofile);

/* Serves `len` bytes at `iso_off` from memory rather than from the disk when
 * the medium is next chainloaded. This is iso/overlay.h's job under UEFI and
 * is what makes a medium bootable that is not bootable as it lies on the
 * stick; media/bsdmd.c's two edits are what need it first.
 *
 * Whole CD sectors are what the handler can serve, so the sectors holding
 * the edit are read from the ISO, the edit is laid over them, and the result
 * is what is served. Call before vcd_chainload; vcd_overlay_reset forgets
 * everything staged so far.
 *
 * 0 on success, nonzero when the pool is full or the sectors would not
 * read. */
int vcd_overlay(fs_file *isofile, uint64_t iso_off, const void *bytes,
                uint32_t len);
void vcd_overlay_reset(void);

#endif /* FALCON_BIOS_VCD_H */
