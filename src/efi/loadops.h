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
 * Nothing about enforcement changes: NVRAM is not the stick, this variable is
 * not read by anything but us, and what it names still has to survive
 * validation against the manifest before a single byte of it is acted on. */
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
 * Non-volatile plus boot-services access: it must survive the reboot the
 * appliance performs, and nothing after ExitBootServices has any business
 * reading it. Returns the firmware's status. */
EFI_STATUS loader_next_boot_set(EFI_SYSTEM_TABLE *st, const char *name);

#endif /* FALCON_EFI_LOADOPS_H */
