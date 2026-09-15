#include <efi/loadops.h>
#include <efi/shimguid.h>

#include <sb/sb.h>

/* 50614485-081b-4b7c-8b54-2abeffdf2fb3: falcon-boot's own vendor GUID for EFI
 * variables. Ours alone, so nothing we write can collide with a platform or
 * shim variable, and so a variable dumper shows at a glance which of these
 * belong to this loader. */
#define FALCON_VENDOR_GUID \
    { 0x50614485, 0x081b, 0x4b7c, \
      { 0x8b, 0x54, 0x2a, 0xbe, 0xff, 0xdf, 0x2f, 0xb3 } }

/* Non-volatile so it survives the appliance's reboot, and runtime-accessible
 * so the booted OS can set it too (DESIGN Section 6.3.1).
 *
 * Runtime access was added deliberately and is not a widening of what this
 * variable can cause. efivarfs exposes only runtime-accessible variables, so
 * without it a chooser running in the personal OS cannot ask for a next boot
 * at all, and the loader is the sole writer of a marker only the loader reads.
 *
 * It grants nothing, because the value has never been trusted: these bytes are
 * raw and untrusted on the way back in, and what they name still has to survive
 * validation against the manifest before a single byte of it is acted on. A
 * root process that sets this can therefore only select among entries that
 * already pass validation, which is exactly the authority UEFI's own BootNext
 * hands to root, and BootNext is runtime-writable by design. What it must NOT
 * become is a channel that skips that validation; if a future change ever acts
 * on this value without checking it, this attribute is the thing that turns
 * that bug into a local privilege escalation. */
#define FALCON_NEXT_BOOT_ATTRS (EFI_VARIABLE_NON_VOLATILE | \
                                EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                                EFI_VARIABLE_RUNTIME_ACCESS)

void resolve_load_ops(EFI_BOOT_SERVICES *bs, load_ops *ops)
{
    EFI_GUID sil_guid = SHIM_IMAGE_LOADER_GUID;
    SHIM_IMAGE_LOADER *sil = (SHIM_IMAGE_LOADER *)0;

    ops->LoadImage = bs->LoadImage;
    ops->StartImage = bs->StartImage;
    ops->UnloadImage = bs->UnloadImage;
    ops->via_shim = FALSE;
    ops->table_unhooked = FALSE;

    if (bs->LocateProtocol(&sil_guid, (void *)0, (void **)&sil) != EFI_SUCCESS)
        return;
    /* A protocol that is present but hollow is a shim we do not understand;
     * the firmware's loader is the safe answer, not a null call. */
    if (sil == (SHIM_IMAGE_LOADER *)0 || sil->LoadImage == (void *)0 ||
        sil->StartImage == (void *)0 || sil->UnloadImage == (void *)0)
        return;

    /* Both sides of this comparison are the same declared type, so it is the
     * plain pointer equality it looks like. Equal means shim patched the
     * table and the two calls are one function; unequal means shim is here
     * but did not patch, which it does not do unless it is enforcing. */
    ops->table_unhooked = (sil->LoadImage != bs->LoadImage) ? TRUE : FALSE;

    ops->LoadImage = sil->LoadImage;
    ops->StartImage = sil->StartImage;
    ops->UnloadImage = sil->UnloadImage;
    ops->via_shim = TRUE;
}

BOOLEAN load_ops_inner_load_blocked(EFI_SYSTEM_TABLE *st, const load_ops *ops)
{
    sb_state state;

    if (st == (EFI_SYSTEM_TABLE *)0 || ops == (const load_ops *)0)
        return FALSE;
    if (!ops->via_shim || !ops->table_unhooked)
        return FALSE;

    /* The read is for wording only, which is the whole of what sb.h permits.
     * SB_TRI_UNKNOWN is deliberately not treated as "on": the loader would
     * rather say nothing than accuse a machine of a state it could not
     * confirm, and the launch is identical either way. */
    if (sb_state_read(st->RuntimeServices, &state) != SB_OK)
        return FALSE;
    return (state.secure_boot == SB_TRI_YES) ? TRUE : FALSE;
}

UINTN loader_next_boot_take(EFI_SYSTEM_TABLE *st, void *buf, UINTN cap)
{
    EFI_GUID vendor = FALCON_VENDOR_GUID;
    EFI_RUNTIME_SERVICES *rt;
    UINTN size = cap;
    EFI_STATUS status;

    if (st == (EFI_SYSTEM_TABLE *)0 || buf == (void *)0 || cap == 0)
        return 0;
    rt = st->RuntimeServices;
    if (rt == (EFI_RUNTIME_SERVICES *)0 || rt->GetVariable == (void *)0 ||
        rt->SetVariable == (void *)0)
        return 0;

    /* The spec's parameters are non-const because the firmware is declared to
     * take them so, not because it writes through them; the uintptr_t round
     * trip is what keeps -Wcast-qual off a wart that is the spec's. */
    status = rt->GetVariable((CHAR16 *)(uintptr_t)LOADER_NEXT_BOOT_NAME,
                             &vendor, (UINT32 *)0, &size, buf);

    /* DELETE FIRST. Everything after this point may fail - the value may be
     * junk, the entry may not be on the stick, preparation may not have
     * happened - and every one of those failures has to end at the menu
     * rather than at another attempt to do the same thing. Deleting before a
     * single byte is believed is what makes a boot loop unrepresentable
     * rather than merely unlikely.
     *
     * Skipped only when the firmware says there is nothing there, so an
     * ordinary boot costs no NVRAM write. EFI_BUFFER_TOO_SMALL lands here
     * too: a value longer than a path is not one of ours, and it goes the
     * same way as any other value we will not act on. */
    if (status != EFI_NOT_FOUND)
        (void)rt->SetVariable((CHAR16 *)(uintptr_t)LOADER_NEXT_BOOT_NAME,
                              &vendor, FALCON_NEXT_BOOT_ATTRS, 0, (void *)0);

    if (EFI_ERROR(status))
        return 0;
    return size;
}

EFI_STATUS loader_next_boot_set(EFI_SYSTEM_TABLE *st, const char *name)
{
    EFI_GUID vendor = FALCON_VENDOR_GUID;
    EFI_RUNTIME_SERVICES *rt;
    UINTN len = 0;

    if (st == (EFI_SYSTEM_TABLE *)0)
        return EFI_INVALID_PARAMETER;
    rt = st->RuntimeServices;
    if (rt == (EFI_RUNTIME_SERVICES *)0 || rt->SetVariable == (void *)0)
        return EFI_UNSUPPORTED;

    /* The terminator travels with the value so the variable reads as a string
     * in any dumper. The reader accepts it either way (bless_next_decode). */
    if (name != (const char *)0) {
        while (name[len] != '\0')
            len++;
        if (len != 0)
            len++;
    }
    return rt->SetVariable((CHAR16 *)(uintptr_t)LOADER_NEXT_BOOT_NAME, &vendor,
                           FALCON_NEXT_BOOT_ATTRS, len,
                           (void *)(uintptr_t)name);
}

const CHAR16 *load_ops_inner_load_advice(void)
{
    /* Deliberately says what the platform did, not what this loader decided:
     * nothing here is ours to change, and a message that implied otherwise
     * would send the user looking for a setting in the wrong program. */
    return L"shim signature validation is disabled on this machine, and that "
           L"is what\r\nstops this image: shim leaves the firmware's own "
           L"LoadImage in place when it\r\nis not validating, and the "
           L"firmware refuses the kernel inside a UKI while\r\nSecure Boot is "
           L"still on. The loader cannot and must not work around that.\r\n"
           L"\r\nEither re-enable validation (mokutil --enable-validation "
           L"from Linux, then\r\nconfirm at MokManager on the next boot), "
           L"which is the state blessed images\r\nare for; or turn Secure "
           L"Boot off in firmware setup, where nothing verifies\r\nand "
           L"everything starts. Booting an ISO the ordinary way is "
           L"unaffected: that\r\nchainload is what disabling validation "
           L"bought, and it still works.";
}
