/* falcon-boot: minimal UEFI base types.
 *
 * We target x86_64-w64-mingw32, whose native calling convention IS the
 * Microsoft x64 ABI that UEFI mandates, so EFIAPI expands to nothing. If this
 * code is ever built with a SysV-defaulting toolchain, EFIAPI must become
 * __attribute__((ms_abi)); the define is kept so that is a one-line change.
 *
 * Naming follows the UEFI specification (CamelCase types and fields) rather
 * than repo snake_case, so that any struct here can be diffed against the
 * spec text it was transcribed from. Field order inside protocol and table
 * structs is ABI: never reorder, never remove, only annotate.
 */

#ifndef FALCON_UEFI_TYPES_H
#define FALCON_UEFI_TYPES_H

#include <stdint.h>
#include <stddef.h>

#define EFIAPI

typedef uint8_t   BOOLEAN;
typedef int64_t   INTN;
typedef uint64_t  UINTN;
typedef int8_t    INT8;
typedef uint8_t   UINT8;
typedef int16_t   INT16;
typedef uint16_t  UINT16;
typedef int32_t   INT32;
typedef uint32_t  UINT32;
typedef int64_t   INT64;
typedef uint64_t  UINT64;
typedef char      CHAR8;
/* Matches wchar_t on the mingw target (with -fshort-wchar kept for safety),
 * so L"..." literals are CHAR16 strings without casts. */
typedef uint16_t  CHAR16;

typedef UINTN     EFI_STATUS;
typedef void     *EFI_HANDLE;
typedef void     *EFI_EVENT;
typedef UINT64    EFI_LBA;
typedef UINTN     EFI_TPL;

/* An event's notification function, and the two values CreateEvent takes
 * for the one kind of event the loader creates (UEFI 2.10 Section 7.1). */
typedef void (EFIAPI *EFI_EVENT_NOTIFY)(EFI_EVENT Event, void *Context);
#define EVT_SIGNAL_EXIT_BOOT_SERVICES 0x00000201u
#define TPL_CALLBACK                  ((EFI_TPL)8)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

#ifndef TRUE
#define TRUE  ((BOOLEAN)1)
#define FALSE ((BOOLEAN)0)
#endif

#endif /* FALCON_UEFI_TYPES_H */
