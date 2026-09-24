/* falcon-boot: the firmware half of partition identification
 * (docs/DESIGN.md Section 3).
 *
 * Answers one question about a Block I/O handle: what does its GPT entry say
 * it is - type GUID and partition name, since p2's identity needs both
 * (part.h). Two routes, tried in that order:
 *
 *   1. EFI_PARTITION_INFO_PROTOCOL (UEFI 2.7+). The firmware already parsed
 *      the GPT to publish the handle and will simply hand us the entry.
 *   2. The handle's HARDDRIVE device path node gives the partition's starting
 *      LBA and names its parent disk (the path with that node removed); we then
 *      parse the parent's GPT ourselves through falcon-core/src/part/part.c and
 *      look the starting LBA up. This is what keeps older firmware working, and
 *      the QEMU rig exercises it, since OVMF's vvfat-backed disks and the loose
 *      image rigs do not present a GPT at all.
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

/* What one handle's GPT entry says it is: the two fields part_match_role
 * needs, and nothing else off the entry. */
typedef struct {
    uint8_t  type[GPT_GUID_LEN];
    uint16_t name[GPT_NAME_LEN];   /* NUL-padded, not necessarily terminated */
} part_ident;

/* Fills *out and returns PART_OK, or leaves it untouched and returns one of
 * the errors above. */
int part_identify(EFI_SYSTEM_TABLE *st, EFI_HANDLE h, part_ident *out);

/* The type GUID alone, for the inventory row falcon-efi/src/discover/discover.c
 * keeps; identity questions go through part_match_role, never through this. */
int part_type_guid(EFI_SYSTEM_TABLE *st, EFI_HANDLE h, EFI_GUID *out);

/* Is this handle our p2 / p3, by the rule in part.h? A handle whose entry
 * cannot be established is not ours, so both answer FALSE for it. */
BOOLEAN part_is_falcon_iso(EFI_SYSTEM_TABLE *st, EFI_HANDLE h);
BOOLEAN part_is_falcon_shelf(EFI_SYSTEM_TABLE *st, EFI_HANDLE h);

/* Is any partition on any disk identified for this role? Enumerates every
 * Block I/O handle, so call it once per scan rather than per handle; the
 * answer is cached (see part_scan_reset). */
BOOLEAN part_role_present(EFI_SYSTEM_TABLE *st, part_role role);

/* THE scan filter, and the one call a scan loop needs. DESIGN Section 3's
 * rule in a single test:
 *
 *   - if any partition identified for this role exists anywhere, accept only
 *     those, so an internal disk's exFAT or LUKS2 volume can never be
 *     mistaken for the stick's;
 *   - if none exists anywhere, accept everything, which is what keeps the
 *     QEMU rig images, any hand-made stick and any stick stamped by an
 *     older mkstick working. This graceful degradation is what makes a
 *     wrong identification guess survivable rather than fatal.
 *
 * The presence half is computed on the first call and cached, so this stays
 * one enumeration per role however many handles the loop walks. */
BOOLEAN part_scan_accept(EFI_SYSTEM_TABLE *st, EFI_HANDLE h, part_role role);

/* Do two handles sit on the same disk? Both must be partitions (a HARDDRIVE
 * node in their device path), and the paths before those nodes must be
 * byte-identical, which is dp_is_parent's test applied to two children
 * rather than a parent and a child. Anything that cannot be compared (no
 * device path, no HARDDRIVE node) answers FALSE. */
BOOLEAN part_same_disk(EFI_SYSTEM_TABLE *st, EFI_HANDLE a, EFI_HANDLE b);

/* Drops the cached presence answers and the cached disk table. Call before a
 * rescan; media the firmware enumerated after our first look would otherwise
 * be judged against a stale picture of the machine. */
void part_scan_reset(void);

#endif /* FALCON_PARTEFI_H */
