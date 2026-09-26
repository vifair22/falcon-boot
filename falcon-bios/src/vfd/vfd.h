/* falcon-bios: serving an El Torito floppy image as a memory disk. See vfd.c
 * and vfdres.S; the arithmetic is vfdcore.c.
 */

#ifndef FALCON_BIOS_VFD_H
#define FALCON_BIOS_VFD_H

#include <fs/fs.h>
#include <media/eltorito.h>

#include <result.h>

/* Loads the medium's floppy-emulation boot image into extended memory and
 * hands the machine to it on drive 0x00. Only returns on failure, with *r
 * filled in. `e` must be a floppy media type (1, 2 or 3). */
int vfd_serve(fs_file *isofile, const elt_entry *e, boot_result *r);

#endif /* FALCON_BIOS_VFD_H */
