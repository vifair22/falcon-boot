/* falcon-boot: the shim-preferring image load path (DESIGN Section 7.6).
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

#ifndef FALCON_EFI_LOADOPS_H
#define FALCON_EFI_LOADOPS_H

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
    /* Shim's loader protocol is installed, but the boot-services table still
     * holds the firmware's LoadImage. Measured 2026-09-13 (notes Section 11):
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
 * a call through zero. Measured, and it is a #GP with RIP=0 (notes Section
 * 11.4). Pairing them is what makes that unreachable. */
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
 * refuses the unsigned inner kernel with EFI_ACCESS_DENIED. Measured as the
 * cause of the UAT failure; see notes Section 11.
 *
 * Three conditions, all read from the live platform rather than inferred
 * from policy: shim's loader is present, the boot-services table is not
 * shim's, and Secure Boot is on so the firmware's loader enforces. Deriving
 * it from the table rather than from MokSBState is deliberate -- it detects
 * the mechanism that breaks rather than one cause of it.
 *
 * This grants nothing and gates nothing. Every caller must launch exactly as
 * it would have; the return value may only change what the user is told.
 * Using it to refuse a boot would be the Section 7.3 regression, in the one
 * state where the user has explicitly asked the platform to stop refusing. */
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
 * Nothing about enforcement changes: NVRAM is not the stick, and what it names
 * still has to survive validation against the manifest before a single byte of
 * it is acted on. That last clause is load-bearing rather than reassuring: the
 * variable is runtime-accessible, so the booted OS can write it, and the
 * validation is the only thing standing between "root asked for this" and
 * "this ran". */
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
 * The bytes are raw and untrusted; bless_next_decode is what turns them into
 * something that may be compared against a path. */
UINTN loader_next_boot_take(EFI_SYSTEM_TABLE *st, void *buf, UINTN cap);

/* Sets the marker to an ASCII path, or clears it when name is NULL or empty.
 * Non-volatile so it survives the reboot the appliance performs, and
 * runtime-accessible so a chooser in the booted OS can set it too; see the
 * attribute definition in loadops.c for why that grants nothing. Returns the
 * firmware's status. */
EFI_STATUS loader_next_boot_set(EFI_SYSTEM_TABLE *st, const char *name);

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

/* ---- boot to the firmware's own settings UI (DESIGN Section 7.4) ----
 *
 * UEFI's own one-shot: OsIndications bit 0, EFI_OS_INDICATIONS_BOOT_TO_FW_UI,
 * asks the firmware to show its settings UI on the next boot instead of
 * booting anything. The firmware clears it, exactly as it clears BootNext.
 *
 * THIS CHANGES NO ENFORCEMENT STATE AND IS NOT A ROUTE AROUND ANY. It is the
 * opposite: Secure Boot can only be turned off by a person standing at the
 * machine in the firmware's own UI, and invariant 3 says that is the only
 * legitimate way. This row does not make that decision, it delivers the user
 * to the place where they can make it. What it removes is guessing which key
 * this vendor wants held at power-on - which differs on every machine the
 * stick visits, is the actual difficulty, and is not a security boundary.
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

#endif /* FALCON_EFI_LOADOPS_H */
