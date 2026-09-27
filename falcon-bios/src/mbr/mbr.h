/* falcon-bios: a disk's own boot code, falcon jump's MBR route
 * (docs/DESIGN.md Section 14). See mbr.c. */

#ifndef FALCON_BIOS_MBR_H
#define FALCON_BIOS_MBR_H

#include <start/bios.h>
#include <result.h>

/* Whether this drive's LBA 0 ends in the boot signature, which is what a
 * BIOS itself asks before it will run a disk's boot code. Nonzero if so. */
int mbr_bootable(bios_disk *d);

/* Reads the drive's LBA 0 to 0x7C00 and starts it in real mode with DL set
 * to the drive, as a BIOS starts the disk it boots from. Returns only when
 * it did not get that far, with *r filled in. */
int mbr_boot(bios_disk *d, boot_result *r);

#endif /* FALCON_BIOS_MBR_H */
