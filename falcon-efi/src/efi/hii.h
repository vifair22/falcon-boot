/* falcon-boot: EFI_HII_DATABASE_PROTOCOL (UEFI spec 34.8), the two members
 * the display calls to add its spinner glyphs to the firmware's fonts
 * (console/display.c), and the package formats that carry them (spec
 * 33.3.1 and 33.3.2). The rest of the protocol is opaque.
 */

#ifndef FALCON_UEFI_HII_H
#define FALCON_UEFI_HII_H

#include <efi/types.h>

#define EFI_HII_DATABASE_PROTOCOL_GUID \
    { 0xef9fc172, 0xa1b2, 0x4693, \
      { 0xb3, 0x27, 0x6d, 0x32, 0xfc, 0x41, 0x60, 0x42 } }

typedef void *EFI_HII_HANDLE;

/* A package's header is a 24-bit length and an 8-bit type in one LE32. */
#define EFI_HII_PACKAGE_SIMPLE_FONTS 0x07u
#define EFI_HII_PACKAGE_END          0xDFu

/* EFI_NARROW_GLYPH: the character, attributes, then 19 rows of 8 pixels,
 * the leftmost in the top bit. 22 bytes, unaligned in the package. */
#define EFI_GLYPH_HEIGHT       19u
#define EFI_NARROW_GLYPH_SIZE  22u

typedef struct EFI_HII_DATABASE_PROTOCOL EFI_HII_DATABASE_PROTOCOL;

struct EFI_HII_DATABASE_PROTOCOL {
    /* PackageList is an EFI_HII_PACKAGE_LIST_HEADER (a GUID and an LE32
     * length) followed by its packages. */
    EFI_STATUS (EFIAPI *NewPackageList)(EFI_HII_DATABASE_PROTOCOL *This,
                                        const void *PackageList,
                                        EFI_HANDLE DriverHandle,
                                        EFI_HII_HANDLE *Handle);
    EFI_STATUS (EFIAPI *RemovePackageList)(EFI_HII_DATABASE_PROTOCOL *This,
                                           EFI_HII_HANDLE Handle);
    void *UpdatePackageList;
    void *ListPackageLists;
    void *ExportPackageLists;
    void *RegisterPackageNotify;
    void *UnregisterPackageNotify;
    void *FindKeyboardLayouts;
    void *GetKeyboardLayout;
    void *SetKeyboardLayout;
    void *GetPackageListHandle;
};

#endif /* FALCON_UEFI_HII_H */
