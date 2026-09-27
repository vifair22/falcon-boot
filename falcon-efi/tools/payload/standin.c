/* A stand-in for an image the station starts off an ESP: Windows Boot
 * Manager (\EFI\Microsoft\Boot\bootmgfw.efi) and memtest86+
 * (\EFI\falcon\memtest.efi) in falcon-efi/tools/run-station-rig.sh, and the
 * removable-media path of the rig's second disk, which is what the
 * firmware starts after a BootNext.
 *
 * It says which file it was started as, which is the whole assertion: a
 * line on the console (and so on the rig's serial) reading
 * "FALCON-STANDIN <path>", taken from its own LoadedImage->FilePath. Then
 * it waits two seconds and returns, so the loader's "the image exited"
 * page is exercised too.
 *
 * Deliberately outside src/: this never ships in the loader.
 */

#include <efi/uefi.h>
#include <efi/loadedimage.h>

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

/* The FILEPATH nodes of the image's own path, printed in order. */
static void say_path(EFI_SYSTEM_TABLE *st, const UINT8 *p)
{
    unsigned n;

    for (n = 0; n < 32u; n++) {
        const EFI_DEVICE_PATH_PROTOCOL *node =
            (const EFI_DEVICE_PATH_PROTOCOL *)(const void *)p;
        UINT16 len = dp_node_len(node);

        if (len < sizeof(*node) || dp_is_end(node))
            return;
        if (node->Type == MEDIA_DEVICE_PATH &&
            node->SubType == MEDIA_FILEPATH_DP && len > sizeof(*node))
            st->ConOut->OutputString(st->ConOut,
                                     (CHAR16 *)(uintptr_t)(p + sizeof(*node)));
        p += len;
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = (EFI_LOADED_IMAGE_PROTOCOL *)0;

    st->ConOut->OutputString(st->ConOut, L"\r\nFALCON-STANDIN ");
    if (st->BootServices->HandleProtocol(image_handle, &li_guid,
                                         (void **)&li) == EFI_SUCCESS &&
        li != (EFI_LOADED_IMAGE_PROTOCOL *)0 && li->FilePath != (void *)0)
        say_path(st, (const UINT8 *)li->FilePath);
    st->ConOut->OutputString(st->ConOut, L"\r\n");
    st->BootServices->Stall(2000000);
    st->ConOut->OutputString(st->ConOut, L"FALCON-STANDIN-EXIT\r\n");
    return EFI_SUCCESS;
}
