/* falcon-esp-jump: the UEFI station's \EFI\BOOT\BOOTX64.EFI (docs/DESIGN.md
 * Section 12.2).
 *
 * A station without shim boots through this image whichever way the
 * firmware gets there: the `falcon` entry names it, and with no entry the
 * firmware's removable-media path is it. So the measured chain is the same
 * on both (12.15). It starts \EFI\falcon\falconx64.efi off the volume it
 * was read from, through the firmware's LoadImage, which is the verifying
 * one with nothing in front of it (Section 7.6).
 */

#include <efi/uefi.h>
#include <efi/devpath.h>
#include <efi/loadedimage.h>
#include <lib/string.h>
#include <lib/ucs2.h>

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

static const CHAR16 k_loader[] = L"\\EFI\\falcon\\falconx64.efi";

/* Long enough to read the line before the firmware moves on. */
#define FAIL_PAUSE_US 5000000u

static void say(EFI_SYSTEM_TABLE *st, const CHAR16 *s)
{
    st->ConOut->OutputString(st->ConOut, (CHAR16 *)(UINTN)s);
}

static void say_hex(EFI_SYSTEM_TABLE *st, UINT64 v)
{
    CHAR16 buf[19];
    UINTN i;

    buf[0] = L'0';
    buf[1] = L'x';
    for (i = 0; i < 16u; i++)
        buf[2u + i] = L"0123456789ABCDEF"[(v >> (60u - 4u * i)) & 0xFu];
    buf[18] = 0;
    say(st, buf);
}

/* The volume's device path with `path` appended as a FILEPATH node. */
static EFI_STATUS file_path(EFI_BOOT_SERVICES *bs, EFI_HANDLE volume,
                            const CHAR16 *path,
                            EFI_DEVICE_PATH_PROTOCOL **out)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *vol = (EFI_DEVICE_PATH_PROTOCOL *)0;
    UINTN end_size = sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN name_bytes = (ucs2_len(path) + 1u) * sizeof(CHAR16);
    UINTN node_len = end_size + name_bytes;
    UINTN vol_len;
    UINTN prefix;
    UINT8 *buf = (UINT8 *)0;
    EFI_DEVICE_PATH_PROTOCOL *node;
    EFI_STATUS status;

    *out = (EFI_DEVICE_PATH_PROTOCOL *)0;
    status = bs->HandleProtocol(volume, &dp_guid, (void **)&vol);
    if (EFI_ERROR(status))
        return status;
    vol_len = dp_total_len(vol);
    if (vol_len < end_size)
        return EFI_INVALID_PARAMETER;
    prefix = vol_len - end_size;
    status = bs->AllocatePool(EfiLoaderData, prefix + node_len + end_size,
                              (void **)&buf);
    if (EFI_ERROR(status))
        return status;
    memcpy(buf, vol, prefix);
    node = (EFI_DEVICE_PATH_PROTOCOL *)(void *)(buf + prefix);
    node->Type = MEDIA_DEVICE_PATH;
    node->SubType = MEDIA_FILEPATH_DP;
    dp_set_node_len(node, (UINT16)node_len);
    memcpy(buf + prefix + end_size, path, name_bytes);
    dp_set_end((EFI_DEVICE_PATH_PROTOCOL *)(void *)(buf + prefix + node_len));
    *out = (EFI_DEVICE_PATH_PROTOCOL *)(void *)buf;
    return EFI_SUCCESS;
}

static EFI_STATUS fail(EFI_SYSTEM_TABLE *st, EFI_STATUS status)
{
    say(st, L"falcon-esp-jump: ");
    say(st, k_loader);
    say(st, L": status ");
    say_hex(st, (UINT64)status);
    say(st, L"\r\n");
    st->BootServices->Stall(FAIL_PAUSE_US);
    return status;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_LOADED_IMAGE_PROTOCOL *li = (EFI_LOADED_IMAGE_PROTOCOL *)0;
    EFI_DEVICE_PATH_PROTOCOL *path = (EFI_DEVICE_PATH_PROTOCOL *)0;
    EFI_HANDLE child = (EFI_HANDLE)0;
    EFI_STATUS status;

    status = bs->HandleProtocol(image_handle, &li_guid, (void **)&li);
    if (EFI_ERROR(status) || li == (EFI_LOADED_IMAGE_PROTOCOL *)0 ||
        li->DeviceHandle == (EFI_HANDLE)0)
        return fail(st, EFI_ERROR(status) ? status : EFI_NOT_FOUND);
    status = file_path(bs, li->DeviceHandle, k_loader, &path);
    if (EFI_ERROR(status))
        return fail(st, status);
    status = bs->LoadImage(FALSE, image_handle, path, (void *)0, 0, &child);
    bs->FreePool(path);
    if (EFI_ERROR(status)) {
        /* A refused image is still loaded (spec 7.4). */
        if (status == EFI_SECURITY_VIOLATION && child != (EFI_HANDLE)0)
            bs->UnloadImage(child);
        return fail(st, status);
    }
    return bs->StartImage(child, (UINTN *)0, (CHAR16 **)0);
}
