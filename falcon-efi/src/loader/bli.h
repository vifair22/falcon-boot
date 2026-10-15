/* falcon-boot: systemd's Boot Loader Interface, the timing half
 * (docs/DESIGN.md Section 5.5; https://systemd.io/BOOT_LOADER_INTERFACE/).
 *
 * The variables `systemd-analyze` and `bootctl` read to split a boot into
 * firmware, loader and kernel, written as systemd-boot writes them
 * (src/boot/boot.c, export-vars.c): UTF-16 with the terminator, volatile,
 * boot-services and runtime access, under the Loader vendor GUID
 * 4a67b082-0a4c-41cf-b6c7-440b29bb8c4f. FalconTimeWaitUSec, the time spent
 * waiting on the keyboard, FalconTimeKdfUSec, the keyslot derivations of
 * the unlock that led to the launch, FalconKdfInfo, that unlock in one
 * line, and FalconReadInfo, what the firmware's reads cost on the way, are
 * falcon's own and under falcon's GUID (loadops.h).
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
#define BLI_KDF_NAME L"FalconTimeKdfUSec"
#define BLI_KDF_INFO_NAME L"FalconKdfInfo"
#define BLI_READ_INFO_NAME L"FalconReadInfo"

/* An unlock's keyslot derivations, as pos.c measured them. */
typedef enum {
    BLI_KDF_NONE = 0,   /* no keyslot was tried */
    BLI_KDF_ARGON2ID,
    BLI_KDF_PBKDF2
} bli_kdf_kind;

typedef struct {
    bli_kdf_kind kind;        /* of the slot that opened, or the last tried */
    const char  *level;       /* argon2id's block function (crypto_simd_name) */
    UINT32       procs;       /* processors that ran lanes */
    UINT32       lanes;       /* argon2id: the slot's */
    UINT32       memory_kib;  /* argon2id */
    UINT32       passes;      /* argon2id */
    UINT32       iterations;  /* pbkdf2 */
    UINT64       usec;        /* the derivations, on the TSC clock */
    UINT32       mhz;         /* the effective core clock, 0 unknown */
} bli_kdf;

/* The longest line bli_kdf_info writes, terminator included. */
#define BLI_KDF_INFO_MAX 160u

/* The unlock in one line, ASCII, into out (cap bytes, terminator
 * included); FalconKdfInfo's value and the tail of pos.c's serial line:
 *   argon2id <level> <P> processors <L> lanes <M> KiB t=<T> <ms> ms
 *   pbkdf2 <P> processors <I> iterations <ms> ms
 *   none <P> processors <ms> ms
 * each followed by " <MHz> MHz" when the clock is known. Returns the
 * length, or 0 with out empty when cap is too small. */
UINTN bli_kdf_info(const bli_kdf *k, char *out, UINTN cap);

/* The firmware's reads (discover/biolow.h) in the three stretches of a boot:
 * startup, until the menu is up; menu, while it is up and the background
 * probe reads; boot, from the route's start to the launch, the unlock and
 * the UKI among it. A loader with no menu has its reads in startup and
 * boot. */
typedef struct {
    UINT64 calls;
    UINT64 bytes;
    UINT64 usec;
} bli_read_phase;

typedef struct {
    bli_read_phase startup;
    bli_read_phase menu;
    bli_read_phase boot;
} bli_reads;

/* The longest line bli_read_info writes, terminator included. */
#define BLI_READ_INFO_MAX 200u

/* The reads in one line, ASCII, into out (cap bytes, terminator included);
 * FalconReadInfo's value:
 *   startup <C> calls <K> KiB <ms> ms, menu <...>, boot <...> <R> MB/s
 * K rounded up, R the boot stretch's bytes per microsecond to two places,
 * left out when the stretch took no time. Returns the length, or 0 with out
 * empty when cap is too small. */
UINTN bli_read_info(const bli_reads *r, char *out, UINTN cap);

/* At startup: LoaderTimeInitUSec (skipped when usec is 0, as systemd-boot
 * skips it), LoaderInfo (info, "falcon-boot <version>"),
 * LoaderFirmwareInfo ("<FirmwareVendor> <major>.<minor, 2 digits>" of
 * FirmwareRevision) and LoaderFirmwareType ("UEFI <major>.<minor>" of the
 * system table's revision). Returns EFI_SUCCESS, or the first failure. */
EFI_STATUS bli_set_init(EFI_SYSTEM_TABLE *st, UINT64 usec,
                        const CHAR16 *info);

/* Immediately before a UKI is started: LoaderTimeExecUSec and
 * FalconTimeWaitUSec, and with kdf (the unlock that led to this launch;
 * NULL for none) FalconTimeKdfUSec and FalconKdfInfo. All are skipped when
 * usec is 0 (no clock this boot); the wait is written when it is 0 and the
 * clock is not. Returns EFI_SUCCESS, or the first failure. */
EFI_STATUS bli_set_exec(EFI_SYSTEM_TABLE *st, UINT64 usec, UINT64 wait_usec,
                        const bli_kdf *kdf);

/* With bli_set_exec, before the same launch: FalconReadInfo. */
EFI_STATUS bli_set_reads(EFI_SYSTEM_TABLE *st, const bli_reads *r);

/* When a started UKI returns: deletes the five, so an image booted later
 * this boot by a route that sets none does not inherit them. */
void bli_clear_exec(EFI_SYSTEM_TABLE *st);

#endif /* FALCON_EFI_BLI_H */
