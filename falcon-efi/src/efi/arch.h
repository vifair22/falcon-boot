/* falcon-boot: the architecture's part of a UEFI file name.
 *
 * The removable-media loader, shim and its second stage are named by the
 * architecture they run on (UEFI 2.10 Section 3.5.1.1; shim's
 * DEFAULT_LOADER): BOOTX64.EFI and shimx64.efi on x86-64, BOOTAA64.EFI and
 * shimaa64.efi on AArch64. A name is written once, with the suffix spliced
 * in: L"\\EFI\\BOOT\\BOOT" EFI_ARCH_UP L".EFI". C11 joins a wide literal with
 * plain ones, so the same macros serve CHAR16 and char strings.
 */

#ifndef FALCON_EFI_ARCH_H
#define FALCON_EFI_ARCH_H

#if defined(__x86_64__)
#define EFI_ARCH_UP "X64"
#define EFI_ARCH_LOW "x64"
#elif defined(__aarch64__)
#define EFI_ARCH_UP "AA64"
#define EFI_ARCH_LOW "aa64"
#else
#error "efi/arch.h: no UEFI file names for this architecture"
#endif

#endif /* FALCON_EFI_ARCH_H */
