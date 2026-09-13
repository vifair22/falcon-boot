/* airies-boot: UEFI status codes (the subset we act on).
 * Values are from UEFI spec Appendix D. Error codes set the high bit of
 * EFI_STATUS; EFI_ERROR() tests it via the sign of INTN.
 */

#ifndef AIRIES_UEFI_STATUS_H
#define AIRIES_UEFI_STATUS_H

#include <efi/types.h>

#define EFI_SUCCESS ((EFI_STATUS)0)

#define EFI_ERR(n) ((EFI_STATUS)(0x8000000000000000ULL | (UINT64)(n)))

#define EFI_LOAD_ERROR          EFI_ERR(1)
#define EFI_INVALID_PARAMETER   EFI_ERR(2)
#define EFI_UNSUPPORTED         EFI_ERR(3)
#define EFI_BAD_BUFFER_SIZE     EFI_ERR(4)
#define EFI_BUFFER_TOO_SMALL    EFI_ERR(5)
#define EFI_NOT_READY           EFI_ERR(6)
#define EFI_DEVICE_ERROR        EFI_ERR(7)
#define EFI_WRITE_PROTECTED     EFI_ERR(8)
#define EFI_OUT_OF_RESOURCES    EFI_ERR(9)
#define EFI_NO_MEDIA            EFI_ERR(12)
#define EFI_MEDIA_CHANGED       EFI_ERR(13)
#define EFI_NOT_FOUND           EFI_ERR(14)
#define EFI_ACCESS_DENIED       EFI_ERR(15)
#define EFI_TIMEOUT             EFI_ERR(18)
#define EFI_ABORTED             EFI_ERR(21)
#define EFI_SECURITY_VIOLATION  EFI_ERR(26)

#define EFI_ERROR(status) (((INTN)(status)) < 0)

#endif /* AIRIES_UEFI_STATUS_H */
