/* airies-boot: Simple File System and File protocols (UEFI spec 13.4, 13.5).
 *
 * The ISO path uses this header mostly for its GUID: the firmware's FAT
 * driver publishes Simple File System on the ISO's embedded ESP, and that
 * handle appearing is how we find the child to chainload from. The file
 * members carry real prototypes because family detection (DESIGN Section 6.2)
 * fingerprints that same filesystem next; everything else stays a placeholder.
 */

#ifndef AIRIES_UEFI_SFS_H
#define AIRIES_UEFI_SFS_H

#include <efi/types.h>

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_INFO_GUID \
    { 0x09576e92, 0x6d3f, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* Open modes and attributes (spec 13.5.2); we only ever read. */
#define EFI_FILE_MODE_READ  0x0000000000000001ULL
#define EFI_FILE_DIRECTORY  0x0000000000000010ULL

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *This,
                              EFI_FILE_PROTOCOL **NewHandle,
                              CHAR16 *FileName,
                              UINT64 OpenMode,
                              UINT64 Attributes);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
    void *Delete;
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This,
                              UINTN *BufferSize,
                              void *Buffer);
    void *Write;
    void *GetPosition;
    void *SetPosition;
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *This,
                                 EFI_GUID *InformationType,
                                 UINTN *BufferSize,
                                 void *Buffer);
    void *SetInfo;
    void *Flush;
    /* Revision 2 adds the async members; we never touch them. */
};

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                    EFI_FILE_PROTOCOL **Root);
};

#endif /* AIRIES_UEFI_SFS_H */
