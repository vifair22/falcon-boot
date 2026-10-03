/* falcon-boot: EFI_SERIAL_IO_PROTOCOL (UEFI spec 12.8), the members the
 * serial log (console/serial.c) calls. The rest are opaque. */

#ifndef FALCON_UEFI_SERIALIO_H
#define FALCON_UEFI_SERIALIO_H

#include <efi/types.h>

#define EFI_SERIAL_IO_PROTOCOL_GUID \
    { 0xbb25cf6f, 0xf1d4, 0x11d2, \
      { 0x9a, 0x0c, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0xfd } }

typedef struct EFI_SERIAL_IO_PROTOCOL EFI_SERIAL_IO_PROTOCOL;

struct EFI_SERIAL_IO_PROTOCOL {
    UINT32 Revision;
    void  *Reset;
    void  *SetAttributes;
    void  *SetControl;
    void  *GetControl;
    EFI_STATUS (EFIAPI *Write)(EFI_SERIAL_IO_PROTOCOL *This,
                               UINTN *BufferSize, void *Buffer);
    void  *Read;
    void  *Mode;
};

#endif /* FALCON_UEFI_SERIALIO_H */
