/* falcon-boot: the one shim interface the loader speaks (DESIGN Section
 * 7.6), kept in its own header so the shim-specific surface is exactly
 * one file wide and easy to audit.
 *
 * SHIM_IMAGE_LOADER is what shim >= 16.0 installs, unconditionally, alongside
 * SHIM_LOCK. Its LoadImage/StartImage verify against db + MOK + SBAT and do
 * the PE loading themselves, which is the only way an image signed with our
 * MOK (rather than with a key in db) can load while Secure Boot is on.
 *
 * When the protocol is absent (Secure Boot off, or no shim in the chain) the
 * loader falls back to the firmware's own LoadImage.
 *
 * GUID and member order are from rhboot/shim lib/guid.c:38 and
 * include/loader-proto.h at commit 72fd4b70e7562d25234204249462da97080ef56f,
 * and were confirmed at runtime by the spike.
 */

#ifndef FALCON_SHIMGUID_H
#define FALCON_SHIMGUID_H

#include <efi/uefi.h>

/* shim's verify protocol, which every distro's signed GRUB asks to check
 * the kernel it loads. Only ever located, never called, by this loader
 * (falcon-efi/src/iso/iso.c hides ours while an ISO's own chain runs). */
#define SHIM_LOCK_GUID \
    { 0x605dab50, 0xe046, 0x4300, \
      { 0xab, 0xb6, 0x3d, 0xd8, 0x10, 0xdd, 0x8b, 0x23 } }

#define SHIM_IMAGE_LOADER_GUID \
    { 0x1f492041, 0xfadb, 0x4e59, \
      { 0x9e, 0x57, 0x7c, 0xaf, 0xe7, 0x3a, 0x55, 0xab } }

/* Four members, call-compatible with the boot-services calls of the same
 * names (measured: under Secure Boot on 16.x the patched gBS->LoadImage is
 * pointer-identical to this LoadImage). Exit is declared for completeness of
 * the interface but is not called: an image we start exits through its own
 * boot-services table, not through ours. */
typedef struct {
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN BootPolicy,
                                   EFI_HANDLE ParentImageHandle,
                                   EFI_DEVICE_PATH_PROTOCOL *DevicePath,
                                   void *SourceBuffer,
                                   UINTN SourceSize,
                                   EFI_HANDLE *ImageHandle);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE ImageHandle,
                                    UINTN *ExitDataSize,
                                    CHAR16 **ExitData);
    EFI_STATUS (EFIAPI *Exit)(EFI_HANDLE ImageHandle,
                              EFI_STATUS ExitStatus,
                              UINTN ExitDataSize,
                              CHAR16 *ExitData);
    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE ImageHandle);
} SHIM_IMAGE_LOADER;

#endif /* FALCON_SHIMGUID_H */
