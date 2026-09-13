/* airies-boot: Block I/O protocol (UEFI spec 13.9).
 *
 * We are on both sides of this protocol: we consume it to reach the exFAT
 * partition, and we produce an instance of it for the virtual CD (DESIGN
 * Section 6 stage 2). Producing one is why the write and reset members carry
 * real prototypes even though the loader never writes storage: the firmware
 * is entitled to call every member of an interface we publish, so all of them
 * must be real functions, and ours refuse writes.
 */

#ifndef AIRIES_UEFI_BLOCKIO_H
#define AIRIES_UEFI_BLOCKIO_H

#include <efi/types.h>

#define EFI_BLOCK_IO_PROTOCOL_GUID \
    { 0x964e5b21, 0x6459, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* Revision 1: the media struct ends at LastBlock. Publishing this revision is
 * what tells the firmware not to read the fields beyond it. */
#define EFI_BLOCK_IO_PROTOCOL_REVISION 0x00010000ULL

typedef struct {
    UINT32  MediaId;
    BOOLEAN RemovableMedia;
    BOOLEAN MediaPresent;
    BOOLEAN LogicalPartition;
    BOOLEAN ReadOnly;
    BOOLEAN WriteCaching;
    UINT32  BlockSize;
    UINT32  IoAlign;
    EFI_LBA LastBlock;
    /* Revision 2+ fields exist beyond this point. We neither read them on a
     * firmware instance nor publish them on ours, which is legal only
     * because our instance declares revision 1. */
} EFI_BLOCK_IO_MEDIA;

typedef struct EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO_PROTOCOL;

struct EFI_BLOCK_IO_PROTOCOL {
    UINT64              Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    EFI_STATUS (EFIAPI *Reset)(EFI_BLOCK_IO_PROTOCOL *This,
                               BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *ReadBlocks)(EFI_BLOCK_IO_PROTOCOL *This,
                                    UINT32 MediaId,
                                    EFI_LBA Lba,
                                    UINTN BufferSize,
                                    void *Buffer);
    EFI_STATUS (EFIAPI *WriteBlocks)(EFI_BLOCK_IO_PROTOCOL *This,
                                     UINT32 MediaId,
                                     EFI_LBA Lba,
                                     UINTN BufferSize,
                                     void *Buffer);
    EFI_STATUS (EFIAPI *FlushBlocks)(EFI_BLOCK_IO_PROTOCOL *This);
};

#endif /* AIRIES_UEFI_BLOCKIO_H */
