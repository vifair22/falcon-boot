/* falcon-boot: partition identification ABI (UEFI spec 13.18 and 10.3.5.1).
 *
 * Two firmware-facing pieces, both consumed and never produced by us:
 *
 *   EFI_PARTITION_INFO_PROTOCOL   the direct answer. Firmware that installs
 *                                 it hands us the partition's whole GPT entry,
 *                                 type GUID included, with no table parsing.
 *   HARDDRIVE_DEVICE_PATH         the fallback key. Older firmware installs no
 *                                 PartitionInfo, but every partition handle
 *                                 still carries a device path ending in this
 *                                 node, whose PartitionStart identifies the
 *                                 entry in the parent disk's GPT.
 *
 * Both are packed. Device path nodes are byte-packed by definition (spec
 * 10.2), and EDK2 declares the PartitionInfo structure under #pragma pack(1)
 * as well, so the union member offsets below are byte offsets with no
 * alignment padding anywhere. Getting that wrong reads a plausible-looking
 * GUID from the wrong offset, so: this is ABI, never reorder, never remove,
 * only annotate. Naming follows the specification, per the note in
 * efi/types.h.
 */

#ifndef FALCON_UEFI_PARTINFO_H
#define FALCON_UEFI_PARTINFO_H

#include <efi/types.h>
#include <efi/devpath.h>

#define EFI_PARTITION_INFO_PROTOCOL_GUID \
    { 0x8cf2f62c, 0xbc9b, 0x4821, \
      { 0x80, 0x8d, 0xec, 0x9e, 0xc4, 0x21, 0xa1, 0xa0 } }

/* Spelt with seven hex digits in the specification and in EDK2, which is not
 * a typo of ours to fix. */
#define EFI_PARTITION_INFO_PROTOCOL_REVISION 0x0001000

#define PARTITION_TYPE_OTHER 0x00
#define PARTITION_TYPE_MBR   0x01
#define PARTITION_TYPE_GPT   0x02

#pragma pack(1)

/* GPT partition entry (spec Table 5-6). Exactly 128 bytes; the same layout
 * src/disk/part.c decodes off the disk, which is why one set of falcon GUID
 * constants serves both routes. */
typedef struct {
    EFI_GUID PartitionTypeGUID;
    EFI_GUID UniquePartitionGUID;
    EFI_LBA  StartingLBA;
    EFI_LBA  EndingLBA;          /* inclusive */
    UINT64   Attributes;
    CHAR16   PartitionName[36];
} EFI_PARTITION_ENTRY;

/* Legacy MBR partition record (spec Table 5-2). Present only because it is
 * the other arm of the protocol's union and the union's size is ABI; the
 * loader never reads it. */
typedef struct {
    UINT8 BootIndicator;
    UINT8 StartHead;
    UINT8 StartSector;
    UINT8 StartTrack;
    UINT8 OSIndicator;
    UINT8 EndHead;
    UINT8 EndSector;
    UINT8 EndTrack;
    UINT8 StartingLBA[4];
    UINT8 SizeInLBA[4];
} MBR_PARTITION_RECORD;

typedef struct {
    UINT32 Revision;
    UINT32 Type;        /* PARTITION_TYPE_*; only GPT carries a type GUID */
    UINT8  System;      /* 1 = the firmware considers this an ESP */
    UINT8  Reserved[7];
    union {
        MBR_PARTITION_RECORD Mbr;
        EFI_PARTITION_ENTRY  Gpt;
    } Info;
} EFI_PARTITION_INFO_PROTOCOL;

/* Media device path, hard drive subtype (spec 10.3.5.1). PartitionStart and
 * PartitionSize are in the parent disk's logical blocks, so PartitionStart
 * compares directly against a GPT entry's StartingLBA.
 *
 * SignatureType says what Signature holds: 0 none, 1 a 32-bit MBR disk
 * signature, 2 a GUID (the partition's PARTUUID). Only the GUID case can
 * belong to a GPT disk. */
#define MEDIA_HARDDRIVE_DP 0x01

#define SIGNATURE_TYPE_MBR  0x01
#define SIGNATURE_TYPE_GUID 0x02

typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;
    UINT32                   PartitionNumber;   /* 1-based */
    UINT64                   PartitionStart;
    UINT64                   PartitionSize;
    UINT8                    Signature[16];
    UINT8                    MBRType;
    UINT8                    SignatureType;
} HARDDRIVE_DEVICE_PATH;

#pragma pack()

#endif /* FALCON_UEFI_PARTINFO_H */
