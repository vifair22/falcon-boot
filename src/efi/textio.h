/* airies-boot: Simple Text Input/Output protocols (UEFI spec 12.3, 12.4).
 * Only the members we call get real prototypes; the rest are typed void*
 * placeholders that exist purely to keep struct offsets ABI-correct.
 */

#ifndef AIRIES_UEFI_TEXTIO_H
#define AIRIES_UEFI_TEXTIO_H

#include <efi/types.h>

typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

/* Scan codes we care about (spec Table 12.4). */
#define EFI_SCAN_NULL  0x0000
#define EFI_SCAN_UP    0x0001
#define EFI_SCAN_DOWN  0x0002
#define EFI_SCAN_ESC   0x0017

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
                               BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
                                       EFI_INPUT_KEY *Key);
    EFI_EVENT WaitForKey;
};

typedef struct {
    INT32   MaxMode;
    INT32   Mode;
    INT32   Attribute;
    INT32   CursorColumn;
    INT32   CursorRow;
    BOOLEAN CursorVisible;
} EFI_SIMPLE_TEXT_OUTPUT_MODE;

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                               BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      CHAR16 *String);
    void *TestString;
    EFI_STATUS (EFIAPI *QueryMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                   UINTN ModeNumber,
                                   UINTN *Columns,
                                   UINTN *Rows);
    void *SetMode;
    EFI_STATUS (EFIAPI *SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      UINTN Attribute);
    EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    EFI_STATUS (EFIAPI *SetCursorPosition)(
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
        UINTN Column,
        UINTN Row);
    EFI_STATUS (EFIAPI *EnableCursor)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      BOOLEAN Visible);
    EFI_SIMPLE_TEXT_OUTPUT_MODE *Mode;
};

/* Text attributes (spec 12.4 SetAttribute). */
#define EFI_BLACK        0x00
#define EFI_BLUE         0x01
#define EFI_GREEN        0x02
#define EFI_CYAN         0x03
#define EFI_RED          0x04
#define EFI_MAGENTA      0x05
#define EFI_BROWN        0x06
#define EFI_LIGHTGRAY    0x07
#define EFI_DARKGRAY     0x08
#define EFI_LIGHTBLUE    0x09
#define EFI_LIGHTGREEN   0x0A
#define EFI_LIGHTCYAN    0x0B
#define EFI_LIGHTRED     0x0C
#define EFI_LIGHTMAGENTA 0x0D
#define EFI_YELLOW       0x0E
#define EFI_WHITE        0x0F

#define EFI_TEXT_ATTR(fg, bg) ((UINTN)((fg) | ((bg) << 4)))

#endif /* AIRIES_UEFI_TEXTIO_H */
