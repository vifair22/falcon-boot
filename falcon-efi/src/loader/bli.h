/* falcon-boot: systemd's Boot Loader Interface, the timing half
 * (docs/DESIGN.md Section 5.5; https://systemd.io/BOOT_LOADER_INTERFACE/).
 *
 * The variables `systemd-analyze` and `bootctl` read to split a boot into
 * firmware, loader and kernel, written as systemd-boot writes them
 * (src/boot/boot.c, export-vars.c): UTF-16 with the terminator, volatile,
 * boot-services and runtime access, under the Loader vendor GUID
 * 4a67b082-0a4c-41cf-b6c7-440b29bb8c4f. FalconTimeWaitUSec, the time spent
 * waiting on the keyboard, is falcon's own and under falcon's GUID
 * (loadops.h).
 *
 * The setters take values, not the clock, so the host suite drives them
 * through a fake SetVariable. loader_time_init and loader_time_exec
 * (loadops.h) read the clock and call them. A variable that cannot be set
 * is left unset: nothing here can fail a boot.
 */

#ifndef FALCON_EFI_BLI_H
#define FALCON_EFI_BLI_H

#include <efi/uefi.h>

/* 4a67b082-0a4c-41cf-b6c7-440b29bb8c4f. */
#define BLI_LOADER_GUID \
    { 0x4a67b082, 0x0a4c, 0x41cf, \
      { 0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f } }

/* Volatile: each describes one boot. */
#define BLI_ATTRS (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

#define BLI_WAIT_NAME L"FalconTimeWaitUSec"

/* At startup: LoaderTimeInitUSec (skipped when usec is 0, as systemd-boot
 * skips it), LoaderInfo (info, "falcon-boot <version>"),
 * LoaderFirmwareInfo ("<FirmwareVendor> <major>.<minor, 2 digits>" of
 * FirmwareRevision) and LoaderFirmwareType ("UEFI <major>.<minor>" of the
 * system table's revision). Returns EFI_SUCCESS, or the first failure. */
EFI_STATUS bli_set_init(EFI_SYSTEM_TABLE *st, UINT64 usec,
                        const CHAR16 *info);

/* Immediately before a UKI is started: LoaderTimeExecUSec and
 * FalconTimeWaitUSec. Both are skipped when usec is 0 (no clock this
 * boot); the wait is written when it is 0 and the clock is not. Returns
 * EFI_SUCCESS, or the first failure. */
EFI_STATUS bli_set_exec(EFI_SYSTEM_TABLE *st, UINT64 usec, UINT64 wait_usec);

#endif /* FALCON_EFI_BLI_H */
