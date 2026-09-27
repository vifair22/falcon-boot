/* falcon-boot: the firmware half of telling partitions apart
 * (docs/DESIGN.md Sections 3.1, 9).
 *
 * What a partition IS comes from its identity record (discover.c), never
 * from its table entry, so all this file keeps of the firmware's partition
 * plumbing is the device path: which disk a partition handle sits on, and
 * where on that disk it starts. Both come from the handle's HARDDRIVE node,
 * which the firmware's partition driver publishes for MBR and GPT
 * partitions alike, so neither answer depends on the table's kind.
 *
 * Split from discover.c so the device-path walking has one home; it is the
 * part of the module that cannot be host-tested.
 */

#ifndef FALCON_PARTEFI_H
#define FALCON_PARTEFI_H

#include <efi/uefi.h>

/* Do two handles sit on the same disk? Both must be partitions (a HARDDRIVE
 * node in their device path), and the paths before those nodes must be
 * byte-identical. Anything that cannot be compared (no device path, no
 * HARDDRIVE node) answers FALSE. */
BOOLEAN part_same_disk(EFI_SYSTEM_TABLE *st, EFI_HANDLE a, EFI_HANDLE b);

/* The partition's first LBA on its disk, in the disk's logical blocks: the
 * HARDDRIVE node's PartitionStart, whichever table the partition is in (an
 * MBR logical partition's is absolute too, spec 10.3.5.1). EFI_NOT_FOUND
 * for a handle that is not a partition. */
EFI_STATUS part_start_lba(EFI_SYSTEM_TABLE *st, EFI_HANDLE h, UINT64 *lba);

/* Is `part` a partition of the disk `disk`? disk's device path, less its
 * end node, must be exactly what comes before part's HARDDRIVE node. */
BOOLEAN part_on_disk(EFI_SYSTEM_TABLE *st, EFI_HANDLE disk, EFI_HANDLE part);

#endif /* FALCON_PARTEFI_H */
