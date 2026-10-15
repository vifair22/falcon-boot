/* falcon-boot: the shim-preferring image load path (DESIGN Section 7.6).
 *
 * The one resolver every launch path (ISO chainload, the personal-OS UKI,
 * blessed UKIs) takes its loader from.
 *
 * Preferring SHIM_IMAGE_LOADER over gBS->LoadImage is not about capability:
 * the spike measured the two to be the same function on shim 16.x under
 * Secure Boot, because shim patches the boot-services table in place. It is
 * about intent and durability: calling the protocol says which loader we
 * meant, keeps working if a future shim stops patching the table, and is
 * the interface upstream maintains.
 *
 * The fallback is the firmware's LoadImage: with Secure Boot off it verifies
 * nothing, and with Secure Boot on and no shim in the chain it enforces db.
 */

#ifndef FALCON_EFI_LOADOPS_H
#define FALCON_EFI_LOADOPS_H

#include <efi/uefi.h>
#include <loader/bli.h>

/* 50614485-081b-4b7c-8b54-2abeffdf2fb3: falcon-boot's own vendor GUID for EFI
 * variables. Ours alone, so nothing we write can collide with a platform or
 * shim variable, and so a variable dumper shows at a glance which of these
 * belong to this loader. */
#define FALCON_VENDOR_GUID \
    { 0x50614485, 0x081b, 0x4b7c, \
      { 0x8b, 0x54, 0x2a, 0xbe, 0xff, 0xdf, 0x2f, 0xb3 } }

typedef struct {
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN, EFI_HANDLE,
                                   EFI_DEVICE_PATH_PROTOCOL *, void *, UINTN,
                                   EFI_HANDLE *);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE, UINTN *, CHAR16 **);
    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE);
    /* Which of the two the members came from; failure lines name it, which
     * is the only way an operator can tell the paths apart. */
    BOOLEAN via_shim;
    /* Shim's loader protocol is installed, but the boot-services table still
     * holds the firmware's LoadImage (DESIGN Section 7.6):
     * shim installs SHIM_LOCK and SHIM_IMAGE_LOADER unconditionally and
     * patches the table only under secure_mode(), so this is the state shim
     * is in whenever validation is off -- either because Secure Boot is off,
     * or because MokSBState turned it off with Secure Boot still on.
     *
     * Harmless in the first case and the whole of the blessed-boot failure in
     * the second; load_ops_inner_load_blocked() is the predicate that tells
     * them apart, and this flag alone must not be read as a problem. */
    BOOLEAN table_unhooked;
} load_ops;

/* Resolved on every attempt rather than cached at startup: the loader has
 * no startup hook of its own to hang it off, and one LocateProtocol per
 * boot is not worth a cache that could go stale.
 *
 * The three members must be used as a set. Loading with one loader and
 * unloading or starting with the other is not merely untidy: shim's
 * shim_unload_image() and shim_start_image() forward a handle they do not
 * recognise to a saved system_unload_image / system_start_image, and shim
 * fills those in only from hook_system_services(), i.e. only under
 * secure_mode(). With validation off they are still NULL and the forward is
 * a call through zero, a #GP with RIP=0 (DESIGN Section 7.6). Pairing them is
 * what makes that unreachable. */
void resolve_load_ops(EFI_BOOT_SERVICES *bs, load_ops *ops);

/* TRUE when an image we launch will be unable to load an inner PE of its own
 * through gBS->LoadImage, even though our own load of it succeeded.
 *
 * That is not a hypothetical shape: it is what every UKI does. sd-stub holds
 * the kernel in memory as one of its own PE sections and hands it to
 * gBS->LoadImage as a source buffer, taking that path whenever the SecureBoot
 * variable is set and shim's loader protocol is present (systemd 261
 * src/boot/linux.c). In the validation-disabled state those two conditions
 * hold while gBS->LoadImage is the firmware's, which knows db only and
 * refuses the unsigned inner kernel with EFI_ACCESS_DENIED (DESIGN Section
 * 7.6).
 *
 * Three conditions, all read from the live platform rather than inferred
 * from policy: shim's loader is present, the boot-services table is not
 * shim's, and Secure Boot is on so the firmware's loader enforces. Deriving
 * it from the table rather than from MokSBState is deliberate -- it detects
 * the mechanism that breaks rather than one cause of it.
 *
 * Every caller launches exactly as it would without it; the return value
 * changes only what the user is told (DESIGN Section 7.3). */
BOOLEAN load_ops_inner_load_blocked(EFI_SYSTEM_TABLE *st, const load_ops *ops);

/* The explanation and the two ways out, shared verbatim by every launch site
 * that can hit it, because the cause and the remedy do not differ by entry
 * type. Wrapped at 78 columns for the spec's guaranteed 80x25 mode. */
const CHAR16 *load_ops_inner_load_advice(void);

/* ---- the next-boot marker (DESIGN Section 6.3.1) ----
 *
 * One non-volatile EFI variable, FalconNextBoot, under our own vendor GUID.
 * The loader sets it before launching the signing appliance to prepare media,
 * and reads it on the next boot to know which entry the user was waiting for.
 * It lives here because DESIGN Section 9 gives the whole lifecycle to the
 * `loader` module: one component both sets and clears it, and it is the only
 * place in this loader that writes an EFI variable at all.
 *
 * Why a variable and not a file on the stick: every filesystem driver in
 * EFI-land is read-only by design, so the loader can read a marker on p2 but
 * can never clear one, and a one-shot that cannot be cleared is not a one-shot.
 * The delete is legal here because all of this runs before ExitBootServices,
 * which is exactly what Section 5.1's rejected volatile-variable design did
 * not have.
 *
 * The variable is runtime-accessible, so the booted OS can write it, and what
 * it names must be validated against the manifest before any of it is acted
 * on. */
#define LOADER_NEXT_BOOT_NAME L"FalconNextBoot"

/* Reads the marker and DELETES it, in that order, unconditionally.
 *
 * That order is the whole boot-loop defence and is not an optimisation to
 * reverse: if preparation failed, or the value was never ours, or anything at
 * all goes wrong below this call, the marker is already gone and the next boot
 * draws the menu. Validating first and deleting after would make every
 * failure that could not boot the entry repeat forever.
 *
 * Returns the byte count written to buf (0 when there was no marker, or when
 * it is longer than cap, which for a value this short means it is not ours).
 * The bytes are raw and untrusted; nextboot_decode is what turns them into
 * something that may be compared against a path. */
UINTN loader_next_boot_take(EFI_SYSTEM_TABLE *st, void *buf, UINTN cap);

/* Sets the marker to an ASCII path, or clears it when name is NULL or empty.
 * Non-volatile so it survives the reboot the appliance performs, and
 * runtime-accessible so a chooser in the booted OS can set it too (see the
 * marker above). Returns the firmware's status. */
EFI_STATUS loader_next_boot_set(EFI_SYSTEM_TABLE *st, const char *name);

/* ---- the appliance's reboot comes back here (DESIGN Section 6.3.1) ----
 *
 * The appliance reboots with the kernel's RB_AUTOBOOT, which follows the
 * firmware's boot order, and a stick reached through a one-time boot pick is
 * not in it: the reboot would land on the internal disk and the marker set
 * above would sit unread until the stick next booted, then fire. BootNext
 * pointed at BootCurrent sends that one reboot back to the entry this boot
 * came through.
 *
 * loader_boot_next_self returns EFI_NOT_FOUND when there is no BootCurrent,
 * or when the option it names no longer exists (a transient one-time pick);
 * nothing is written then. loader_boot_next_clear undoes it for a launch
 * that came back without rebooting, so an unrelated reboot later does not
 * return here. The firmware deletes BootNext itself when it acts on it. */
EFI_STATUS loader_boot_next_self(EFI_SYSTEM_TABLE *st);
void loader_boot_next_clear(EFI_SYSTEM_TABLE *st);

/* BootNext set to a named option, for the stationary build's NXT route
 * (DESIGN Section 12.4): the firmware boots Boot<option> on the next start
 * as it would from its own boot menu. The same write as
 * loader_boot_next_self, with the option chosen by the user rather than
 * read from BootCurrent. EFI_NOT_FOUND when Boot<option> does not exist;
 * nothing is written then. */
EFI_STATUS loader_boot_next_set(EFI_SYSTEM_TABLE *st, UINT16 option);

/* ---- the ISO extent map (DESIGN Section 6.4) ----
 *
 * FalconIsoMap, the variable falcon-hook reads from efivarfs to rebuild the
 * ISO as a block device (falcon-core/src/media/extmap.h has the layout). Set
 * immediately before a direct boot starts the kernel and deleted if the
 * kernel comes back, so a later boot of anything else never sees a stale map.
 *
 * Volatile: it describes where one file sits on p2 at the moment of one boot,
 * and nothing about it should outlive a power cycle. Runtime-accessible,
 * because efivarfs shows the booted kernel only runtime variables, and that
 * reader is the whole point. The hook checks what it reads against the disk it
 * finds (partition GUID, start, block size), so a value some other writer put
 * there maps nothing the hook would not refuse. */
EFI_STATUS loader_isomap_set(EFI_SYSTEM_TABLE *st, const void *map, UINTN len);
void loader_isomap_clear(EFI_SYSTEM_TABLE *st);

/* ---- the station's saved event log (DESIGN Section 12.15) ----
 *
 * FalconEvLog, under our vendor GUID: the PCRs as the station found them,
 * and on a TPM 1.2 edk2's event log (falcon-core/src/tpm/evsave.h has the
 * record), which `station totp enroll` reads from efivarfs. Volatile, for
 * the same reason as FalconIsoMap: it describes one boot, and a record left
 * from an earlier boot would be sealed to as if it were this one's. Written
 * by the station on every boot with a TPM; enrollment checks it against the
 * TPM before it seals to anything. */
EFI_STATUS loader_evlog_set(EFI_SYSTEM_TABLE *st, const void *rec, UINTN len);

/* FalconEvLogStatus beside it (tpm/evsave.h): len bytes of ASCII, the same
 * attributes. */
EFI_STATUS loader_evlog_status_set(EFI_SYSTEM_TABLE *st, const char *text,
                                   UINTN len);

/* FalconDiscoverStatus: how the loader found its own boot partition, or
 * where that stopped (discover_install_detail), len bytes of ASCII, the
 * same attributes. Written by both UEFI loaders on every boot. */
EFI_STATUS loader_discover_status_set(EFI_SYSTEM_TABLE *st, const char *text,
                                      UINTN len);


/* ---- boot to the firmware's own settings UI (DESIGN Section 7.4) ----
 *
 * UEFI's own one-shot: OsIndications bit 0, EFI_OS_INDICATIONS_BOOT_TO_FW_UI,
 * asks the firmware to show its settings UI on the next boot instead of
 * booting anything. The firmware clears it, exactly as it clears BootNext.
 *
 * The row delivers the user to the firmware's own UI, where Secure Boot is
 * changed, without guessing which key this vendor wants held at power-on.
 *
 * It earns its place because of one specific dead end: when validation is
 * disabled with Secure Boot still on, a UKI cannot boot
 * (load_ops_inner_load_blocked), and the advice we print says "go to firmware
 * setup" to a user who has no way to get there from where they are standing. */

/* TRUE when the firmware advertises EFI_OS_INDICATIONS_BOOT_TO_FW_UI in
 * OsIndicationsSupported. Asked before the row is drawn rather than after it
 * is picked: a menu entry that reboots into nothing is worse than no entry. */
BOOLEAN fw_setup_available(EFI_SYSTEM_TABLE *st);

/* Sets the bit and resets the machine. Does not return on success.
 *
 * Read-modify-write, because the other bits of OsIndications are pending
 * capsule-update requests and clobbering one would silently drop somebody
 * else's work. Returns FALSE when the variable could not be written or the
 * reset did not happen, having already said why on screen. */
BOOLEAN loader_boot_to_fw_setup(EFI_SYSTEM_TABLE *st);

/* ---- boot timing (DESIGN Section 5.5; loader/bli.h) ----
 *
 * loader_time_init is efi_main's: t0 is the counter as efi_main's first
 * statement read it, and info the LoaderInfo string. It decides the
 * counter's rate (a calibration happens here, after t0 was taken) and sets
 * the startup variables. loader_time_exec is the UKI launches' (pos.c,
 * bless.c), called immediately before StartImage: the counter, then
 * LoaderTimeExecUSec and FalconTimeWaitUSec, and with kdf, the unlock that
 * led to this launch (NULL for none), FalconTimeKdfUSec and FalconKdfInfo,
 * and FalconReadInfo (loader_reads_mark below). Both say what they set on serial (console/serial.h) and carry on
 * whatever the firmware answers. */
void loader_time_init(EFI_SYSTEM_TABLE *st, UINT64 t0, const CHAR16 *info);
void loader_time_exec(EFI_SYSTEM_TABLE *st, const bli_kdf *kdf);

/* Where a boot's reads change stretch (bli_reads): the menu is up, and a
 * route has started. The menu mark is taken once, the first time; the boot
 * mark every time, so a route tried again after one that came back counts
 * from its own start. loader_time_init starts the count's clock, and
 * loader_time_exec writes FalconReadInfo from the marks: with no menu mark
 * startup runs to the boot mark, and with neither everything is startup. */
typedef enum {
    LOADER_READS_MENU,
    LOADER_READS_BOOT
} loader_reads_at;

void loader_reads_mark(loader_reads_at at);

#endif /* FALCON_EFI_LOADOPS_H */
