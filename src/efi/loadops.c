#include <efi/loadops.h>
#include <efi/shimguid.h>

#include <sb/sb.h>

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
