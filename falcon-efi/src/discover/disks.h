/* falcon-boot: where a partition is, in falcon's names for disks
 * (docs/DESIGN.md Sections 3.2, 12.3).
 *
 * The station names another system's partition the way STATION.INI does:
 * the disk by its GPT DiskGUID or MBR signature, the partition by its
 * number. The firmware names it by device path, and a firmware boot entry
 * by a HARDDRIVE node that carries a partition GUID or a disk signature but
 * never a DiskGUID. This module joins the two: it reads the partition table
 * of every whole disk in the one inventory (discover.c) once, and answers
 * both questions from those tables.
 *
 * Nothing here decides what a partition is; that is the identity record's
 * job (Section 3.1). This is naming only, for rows and for matching an INI
 * section to what was found.
 */

#ifndef FALCON_DISKS_H
#define FALCON_DISKS_H

#include <efi/uefi.h>

#include <part/table.h>

typedef struct {
    part_disk_id disk;
    uint32_t     number;                 /* as Linux numbers it */
    uint64_t     start_lba;
    uint8_t      unique[GPT_GUID_LEN];   /* GPT: its PARTUUID; MBR: 0 */
} disks_part;

/* Reads the table of every whole disk in the inventory. Call after
 * discover_scan; again after another scan. Returns how many disks carried a
 * table this could read. */
UINTN disks_scan(EFI_SYSTEM_TABLE *st);

/* Where the partition handle h sits: its disk's id and its entry.
 * EFI_NOT_FOUND for a handle that is not a partition of a scanned disk. */
EFI_STATUS disks_part_of(EFI_SYSTEM_TABLE *st, EFI_HANDLE h, disks_part *out);

/* The partition a HARDDRIVE node names (a firmware boot entry's): sig_type
 * 2 is a GPT entry whose unique GUID is sig; 1 is the MBR disk whose
 * signature is sig[0..3], and its entry starting at `start`. */
EFI_STATUS disks_part_by_node(UINT8 sig_type, const UINT8 sig[16],
                              UINT64 start, disks_part *out);

#endif /* FALCON_DISKS_H */
