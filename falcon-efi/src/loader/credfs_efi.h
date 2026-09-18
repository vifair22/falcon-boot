/* falcon-boot: the File Protocol surface a provider has to implement, plus
 * the info structures it has to hand out (UEFI spec 13.5 and 13.6).
 *
 * Why this is not in falcon-efi/src/efi/sfs.h. That header exists for the ISO
 * path, which CONSUMES a filesystem the firmware published: it needs a handful
 * of members and leaves the rest as typed placeholders. This module PUBLISHES a
 * filesystem, so the firmware may call any member of the protocol, including
 * the four sfs.h stubs out. Rather than widening a header the rest of the
 * loader is happy with, the full-surface form lives beside its only
 * implementation.
 *
 * The two structs are the same UEFI protocol and are ABI-identical by
 * construction (spec 13.5, revision 1: eleven members in this order, no
 * async pair). The static assertion below is what keeps that true: the
 * pointer handed back through sfs.h's OpenVolume is one of ours.
 *
 * Naming and field order follow the specification, per
 * falcon-efi/src/efi/types.h.
 */

#ifndef FALCON_CREDFS_EFI_H
#define FALCON_CREDFS_EFI_H

#include <efi/sfs.h>
#include <efi/types.h>

/* Revision 1. Revision 2 adds OpenEx/ReadEx/WriteEx/FlushEx, which the
 * firmware is only entitled to call when we advertise it, so we do not. */
#define EFI_FILE_PROTOCOL_REVISION 0x00010000ULL
#define EFI_FILE_SYSTEM_PROTOCOL_REVISION 0x00010000ULL

/* Open modes (spec 13.5.2). sfs.h already carries EFI_FILE_MODE_READ, which
 * is the only one a read-only provider ever accepts; the other two are here
 * so the refusal can name what it refused. */
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

/* File attributes (spec 13.5.16). EFI_FILE_DIRECTORY comes from sfs.h. */
#define EFI_FILE_READ_ONLY 0x0000000000000001ULL
#define EFI_FILE_HIDDEN    0x0000000000000002ULL
#define EFI_FILE_SYSTEM    0x0000000000000004ULL
#define EFI_FILE_ARCHIVE   0x0000000000000020ULL

/* Warning codes (spec Appendix D). Not errors: the high bit is clear, so
 * EFI_ERROR() is false for them. Delete on read-only media is specified to
 * report exactly this. */
#define EFI_WARN_DELETE_FAILURE ((EFI_STATUS)2)

#define EFI_FILE_SYSTEM_INFO_ID_GUID \
    { 0x09576e93, 0x6d3f, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_SYSTEM_VOLUME_LABEL_ID_GUID \
    { 0xdb47d7d3, 0xfe81, 0x11d3, \
      { 0x9a, 0x35, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

/* Spec 8.3. Carried whole because EFI_FILE_INFO embeds three of them and a
 * short transcription would shift Attribute and FileName. */
typedef struct {
    UINT16 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
    UINT8  Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

/* Spec 13.5.16. Size is the size of the WHOLE record, the name included,
 * which is why the name is a flexible array and not a fixed buffer. */
typedef struct {
    UINT64   Size;
    UINT64   FileSize;
    UINT64   PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64   Attribute;
    CHAR16   FileName[];
} EFI_FILE_INFO;

/* Bytes of an EFI_FILE_INFO before the name. The spec's own SIZE_OF_ macro;
 * sizeof() would include any tail padding the compiler chose. */
#define SIZE_OF_EFI_FILE_INFO offsetof(EFI_FILE_INFO, FileName)

/* Spec 13.5.17. */
typedef struct {
    UINT64  Size;
    BOOLEAN ReadOnly;
    UINT64  VolumeSize;
    UINT64  FreeSpace;
    UINT32  BlockSize;
    CHAR16  VolumeLabel[];
} EFI_FILE_SYSTEM_INFO;

#define SIZE_OF_EFI_FILE_SYSTEM_INFO offsetof(EFI_FILE_SYSTEM_INFO, VolumeLabel)

/* Spec 13.5.18 defines EFI_FILE_SYSTEM_VOLUME_LABEL as a struct whose only
 * member is the flexible name array, which is a GCC extension rather than
 * C11 and which -Wpedantic rejects. It is transcribed as its own size
 * instead: the record IS the NUL-terminated label, and a struct that adds
 * nothing to that is not worth an extension. */
#define SIZE_OF_EFI_FILE_SYSTEM_VOLUME_LABEL ((UINTN)0)

typedef struct CREDFS_EFI_FILE_PROTOCOL CREDFS_EFI_FILE_PROTOCOL;

/* Spec 13.5, revision 1, in ABI order. */
struct CREDFS_EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(CREDFS_EFI_FILE_PROTOCOL *This,
                              CREDFS_EFI_FILE_PROTOCOL **NewHandle,
                              CHAR16 *FileName,
                              UINT64 OpenMode,
                              UINT64 Attributes);
    EFI_STATUS (EFIAPI *Close)(CREDFS_EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Delete)(CREDFS_EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Read)(CREDFS_EFI_FILE_PROTOCOL *This,
                              UINTN *BufferSize,
                              void *Buffer);
    EFI_STATUS (EFIAPI *Write)(CREDFS_EFI_FILE_PROTOCOL *This,
                               UINTN *BufferSize,
                               void *Buffer);
    EFI_STATUS (EFIAPI *GetPosition)(CREDFS_EFI_FILE_PROTOCOL *This,
                                     UINT64 *Position);
    EFI_STATUS (EFIAPI *SetPosition)(CREDFS_EFI_FILE_PROTOCOL *This,
                                     UINT64 Position);
    EFI_STATUS (EFIAPI *GetInfo)(CREDFS_EFI_FILE_PROTOCOL *This,
                                 EFI_GUID *InformationType,
                                 UINTN *BufferSize,
                                 void *Buffer);
    EFI_STATUS (EFIAPI *SetInfo)(CREDFS_EFI_FILE_PROTOCOL *This,
                                 EFI_GUID *InformationType,
                                 UINTN BufferSize,
                                 void *Buffer);
    EFI_STATUS (EFIAPI *Flush)(CREDFS_EFI_FILE_PROTOCOL *This);
};

/* The consumer's view and the provider's view must be the same object, or
 * OpenVolume hands the firmware a pointer into the wrong layout. */
_Static_assert(sizeof(CREDFS_EFI_FILE_PROTOCOL) == sizeof(EFI_FILE_PROTOCOL),
               "the File Protocol has one ABI; sfs.h and this must agree");

#endif /* FALCON_CREDFS_EFI_H */
