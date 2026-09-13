/* airies-boot: the shim-preferring image load path (DESIGN Section 7.6).
 *
 * One home for the resolver that three launch paths (ISO chainload, the
 * personal-OS UKI, blessed UKIs) previously carried as deliberate copies:
 * the third copy was the agreed promotion trigger, and this file is the
 * promotion.
 *
 * Preferring SHIM_IMAGE_LOADER over gBS->LoadImage is not about capability:
 * the spike measured the two to be the same function on shim 16.x under
 * Secure Boot, because shim patches the boot-services table in place. It is
 * about intent and durability: calling the protocol says which loader we
 * meant, keeps working if a future shim stops patching the table, and is
 * the interface upstream maintains.
 *
 * The fallback is not a weakening. With Secure Boot off there is nothing to
 * verify and the firmware's LoadImage is the honest loader; with Secure
 * Boot on and no shim in the chain the firmware's LoadImage is the
 * enforcing one. Either way the decision is the platform's, never ours.
 */

#ifndef AIRIES_EFI_LOADOPS_H
#define AIRIES_EFI_LOADOPS_H

#include <efi/uefi.h>

typedef struct {
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN, EFI_HANDLE,
                                   EFI_DEVICE_PATH_PROTOCOL *, void *, UINTN,
                                   EFI_HANDLE *);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE, UINTN *, CHAR16 **);
    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE);
    /* Which of the two the members came from; failure lines name it, which
     * is the only way an operator can tell the paths apart. */
    BOOLEAN via_shim;
} load_ops;

/* Resolved on every attempt rather than cached at startup: the loader has
 * no startup hook of its own to hang it off, and one LocateProtocol per
 * boot is not worth a cache that could go stale. */
void resolve_load_ops(EFI_BOOT_SERVICES *bs, load_ops *ops);

#endif /* AIRIES_EFI_LOADOPS_H */
