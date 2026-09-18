/* falcon-boot: EFI_LOADED_IMAGE_PROTOCOL (UEFI spec 9.1).
 *
 * Wanted for one field: DeviceHandle, the volume this loader was read from,
 * which is p1. The boot-prediction code reads the stick's own MOK certificate
 * off it (DESIGN Section 7.8). The rest of the structure is declared so the
 * layout is right, and nothing else reads it.
 */

#ifndef FALCON_UEFI_LOADEDIMAGE_H
#define FALCON_UEFI_LOADEDIMAGE_H

#include <efi/types.h>
#include <efi/tables.h>

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, \
      { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    UINT32            Revision;
    EFI_HANDLE        ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE        DeviceHandle;
    void             *FilePath;
    void             *Reserved;
    UINT32            LoadOptionsSize;
    void             *LoadOptions;
    void             *ImageBase;
    UINT64            ImageSize;
    EFI_MEMORY_TYPE   ImageCodeType;
    EFI_MEMORY_TYPE   ImageDataType;
    void             *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

#endif /* FALCON_UEFI_LOADEDIMAGE_H */
