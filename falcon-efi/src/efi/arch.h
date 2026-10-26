/* falcon-boot: the architecture's part of a UEFI file name.
 *
 * The removable-media loader, shim and its second stage are named by the
 * architecture they run on (UEFI 2.10 Section 3.5.1.1; shim's
 * DEFAULT_LOADER): BOOTX64.EFI and shimx64.efi on x86-64, BOOTAA64.EFI and
 * shimaa64.efi on AArch64. A name is written once, with the suffix spliced
 * in: L"\\EFI\\BOOT\\BOOT" EFI_ARCH_UP L".EFI". C11 joins a wide literal with
 * plain ones, so the same macros serve CHAR16 and char strings.
 * EFI_ARCH_MACHINE is the PE machine type this loader runs, the one the
 * boot predictions answer for; EFI_ARCH_OTHER_* name the other 64-bit
 * architecture, for a row only that one boots (uefiboot.h, UEFIBOOT_OTHER).
 */

#ifndef FALCON_EFI_ARCH_H
#define FALCON_EFI_ARCH_H

#if defined(__x86_64__)
#define EFI_ARCH_UP "X64"
#define EFI_ARCH_LOW "x64"
#define EFI_ARCH_MACHINE 0x8664u     /* PE_MACHINE_AMD64 */
#define EFI_ARCH_OTHER_TAG "ARM64"
#define EFI_ARCH_OTHER_NAME "an ARM64 (AArch64)"
#elif defined(__aarch64__)
#define EFI_ARCH_UP "AA64"
#define EFI_ARCH_LOW "aa64"
#define EFI_ARCH_MACHINE 0xaa64u     /* PE_MACHINE_ARM64 */
#define EFI_ARCH_OTHER_TAG "X64"
#define EFI_ARCH_OTHER_NAME "an x86-64"
#else
#error "efi/arch.h: no UEFI file names for this architecture"
#endif

#endif /* FALCON_EFI_ARCH_H */
