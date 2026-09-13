/* falcon-boot: the firmware half of partition identification
 * (docs/DESIGN.md Section 3).
 *
 * Answers one question about a Block I/O handle: which GPT partition type
 * GUID does it carry? Two routes, tried in that order:
 *
 *   1. EFI_PARTITION_INFO_PROTOCOL (UEFI 2.7+). The firmware already parsed
 *      the GPT to publish the handle and will simply hand us the entry.
 *   2. The handle's HARDDRIVE device path node gives the partition's starting
 *      LBA and names its parent disk (the path with that node removed); we
 *      then parse the parent's GPT ourselves through src/part/part.c and look
 *      the starting LBA up. This is what keeps older firmware working, and
 *      the QEMU rig exercises it, since OVMF's vvfat-backed disks and the
 *      loose image rigs do not present a GPT at all.
 *
 * Split from part.c so that everything testable off the firmware stays
 * testable: this file is the only part of the module that cannot be.
 *
 * The cost model, since this runs inside a scan loop: route 1 is a protocol
 * lookup and a memcpy. Route 2 costs one handle enumeration plus, for the
 * first partition of a given disk, two streaming passes over that disk's
 * 16 KiB entry array. The parse is cached for the most recently seen disk,
 * and firmware enumerates a disk's partitions consecutively, so in practice
 * a stick is parsed once per scan.
 */

#ifndef FALCON_PARTEFI_H
#define FALCON_PARTEFI_H

#include <efi/uefi.h>
#include <part/part.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    PART_OK            =  0,
    PART_NOT_PARTITION = -1, /* a whole disk, or a partition of something that
                                is not a GPT, so no type GUID can exist */
    PART_UNKNOWN       = -2  /* it is a GPT partition, but neither route could
                                establish its type: no PartitionInfo and the
                                parent disk's GPT was unreadable or refused */
};

/* Writes the handle's partition type GUID and returns PART_OK, or leaves
 * *out untouched and returns one of the errors above. */
int part_type_guid(EFI_SYSTEM_TABLE *st, EFI_HANDLE h, EFI_GUID *out);

/* Does this handle carry the falcon type GUID for p2 / p3? A handle whose
 * type cannot be established is not ours, so both answer FALSE for it. */
BOOLEAN part_is_falcon_iso(EFI_SYSTEM_TABLE *st, EFI_HANDLE h);
BOOLEAN part_is_falcon_shelf(EFI_SYSTEM_TABLE *st, EFI_HANDLE h);

/* Is any partition on any disk typed for this role? Enumerates every Block
 * I/O handle, so call it once per scan rather than per handle; the answer is
 * cached (see part_scan_reset). */
BOOLEAN part_role_present(EFI_SYSTEM_TABLE *st, part_role role);

/* THE scan filter, and the one call a scan loop needs. DESIGN Section 3's
 * rule in a single test:
 *
 *   - if any falcon-typed partition for this role exists anywhere, accept
 *     only those, so an internal disk's exFAT or LUKS2 volume can never be
 *     mistaken for the stick's;
 *   - if none exists anywhere, accept everything, which is what keeps the
 *     QEMU rig images and any hand-made stick working until mkstick
 *     (Section 8) stamps the types.
 *
 * The presence half is computed on the first call and cached, so this stays
 * one enumeration per role however many handles the loop walks. */
BOOLEAN part_scan_accept(EFI_SYSTEM_TABLE *st, EFI_HANDLE h, part_role role);

/* Drops the cached presence answers and the cached disk table. Call before a
 * rescan; media the firmware enumerated after our first look would otherwise
 * be judged against a stale picture of the machine. */
void part_scan_reset(void);

#endif /* FALCON_PARTEFI_H */
