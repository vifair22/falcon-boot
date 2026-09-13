#include <efi/loadops.h>
#include <efi/shimguid.h>

void resolve_load_ops(EFI_BOOT_SERVICES *bs, load_ops *ops)
{
    EFI_GUID sil_guid = SHIM_IMAGE_LOADER_GUID;
    SHIM_IMAGE_LOADER *sil = (SHIM_IMAGE_LOADER *)0;

    ops->LoadImage = bs->LoadImage;
    ops->StartImage = bs->StartImage;
    ops->UnloadImage = bs->UnloadImage;
    ops->via_shim = FALSE;

    if (bs->LocateProtocol(&sil_guid, (void *)0, (void **)&sil) != EFI_SUCCESS)
        return;
    /* A protocol that is present but hollow is a shim we do not understand;
     * the firmware's loader is the safe answer, not a null call. */
    if (sil == (SHIM_IMAGE_LOADER *)0 || sil->LoadImage == (void *)0 ||
        sil->StartImage == (void *)0 || sil->UnloadImage == (void *)0)
        return;

    ops->LoadImage = sil->LoadImage;
    ops->StartImage = sil->StartImage;
    ops->UnloadImage = sil->UnloadImage;
    ops->via_shim = TRUE;
}
