/* falcon-boot: EFI_LOAD_FILE2_PROTOCOL (UEFI spec 13.2) and the Linux initrd
 * media device path.
 *
 * The Linux EFI stub (5.8 and later) asks for its initrd this way: it locates
 * a LoadFile2 instance on a handle whose device path is exactly one vendor
 * media node carrying LINUX_EFI_INITRD_MEDIA_GUID, then calls LoadFile twice,
 * once for the size and once for the bytes. The direct boot path
 * (falcon-efi/src/direct) is the provider.
 */

#ifndef FALCON_UEFI_LOADFILE2_H
#define FALCON_UEFI_LOADFILE2_H

#include <efi/types.h>
#include <efi/devpath.h>

#define EFI_LOAD_FILE2_PROTOCOL_GUID \
    { 0x4006c0c1, 0xfcb3, 0x403e, \
      { 0x99, 0x6d, 0x4a, 0x6c, 0x87, 0x24, 0xe0, 0x6d } }

/* 5568e427-68fc-4f3d-ac74-ca555231cc68 (linux include/linux/efi.h). */
#define LINUX_EFI_INITRD_MEDIA_GUID \
    { 0x5568e427, 0x68fc, 0x4f3d, \
      { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } }

typedef struct EFI_LOAD_FILE2_PROTOCOL EFI_LOAD_FILE2_PROTOCOL;

struct EFI_LOAD_FILE2_PROTOCOL {
    EFI_STATUS (EFIAPI *LoadFile)(EFI_LOAD_FILE2_PROTOCOL *This,
                                  EFI_DEVICE_PATH_PROTOCOL *FilePath,
                                  BOOLEAN BootPolicy, UINTN *BufferSize,
                                  void *Buffer);
};

#endif /* FALCON_UEFI_LOADFILE2_H */
