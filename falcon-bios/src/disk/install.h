/* falcon-bios: which partitions are this install's (docs/DESIGN.md Section
 * 3.1).
 *
 * Stage 2 found the `boot` partition on the disk it booted from and left
 * its first LBA and its marker sector in the gate (load3.c). Stage 3 starts
 * there: the boot partition's record gives the install ID, and every other
 * partition is taken only when its own record carries the same ID, on
 * whichever BIOS disk it is. A second falcon disk plugged in is never mixed
 * in, and nothing is decided by drive order.
 *
 * The stick's stage 3 and the station's link this; jump has no install of
 * its own and asks bootpart.h its one question directly.
 */

#ifndef FALCON_BIOS_INSTALL_H
#define FALCON_BIOS_INSTALL_H

#include <stdint.h>

#include <start/bios.h>
#include <part/table.h>
#include <ident/ident.h>

/* What stage 2 published (biosdefs.h, GATE_BOOTPART and GATE_MARKER). */
uint64_t install_boot_lba(void);
uint16_t install_marker_sector(void);

/* Reads the boot partition's record off the boot disk, and says on serial
 * which partition and install this is. 0 on success. */
int install_boot_record(bios_disk *boot, ident_rec *out);

/* The BIOS drives to look at for an install's other partitions: the boot
 * drive first, whatever its number (a USB stick emulated past the BDA's
 * count, audit B1), then every fixed disk the BDA counts from 0x80 up
 * (B2), each once. Returns how many were written to `out`. */
unsigned install_drives(uint8_t boot, uint8_t *out, unsigned max);

/* What this partition's own record says it is: a LUKS2 header's falcon
 * token, or a /.falcon file at the root of a volume the filesystem
 * registry mounts. 1 with *out filled when there is a record, 0 when there
 * is none or it will not decode. *encrypted says which of the two it was.
 * The boot partition's binary record is read by install_boot_record, not
 * here. */
int install_part_record(bios_disk *d, const part_entry *p, ident_rec *out,
                        int *encrypted);

/* Prints the install ID as 32 hex digits, on serial. */
void install_put_id(const ident_rec *r);

#endif /* FALCON_BIOS_INSTALL_H */
