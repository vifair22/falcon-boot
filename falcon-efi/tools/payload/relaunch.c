/* The core rig's relaunch-uefi payload: a program a falcon-boot launches
 * that starts a second falcon-boot, the shape of the EFI shell run as a
 * blessed .efi and asked to start the loader. The second loader runs while
 * the first one's synthetic devices are still published, which is what the
 * case exists to exercise (DESIGN Section 5.1).
 *
 * It tries \EFI\BOOT\grubx64.efi on every Simple File System volume in
 * turn, by device path through the firmware's LoadImage, and starts the
 * first one that loads. A volume without the file (the first loader's own
 * credential filesystem among them) answers NOT_FOUND and is passed over.
 * "FALCON-RELAUNCH <status>" on the console says how it ended.
 *
 * Deliberately outside src/: this never ships in the loader. Linked alone,
 * like standin.c, so it copies bytes by hand.
 */

#include <efi/uefi.h>
#include <efi/devpath.h>
#include <efi/sfs.h>

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

static const CHAR16 target[] = L"\\EFI\\BOOT\\grubx64.efi";

static void copy_bytes(UINT8 *dst, const UINT8 *src, UINTN n)
{
    UINTN i;

    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

static void say_status(EFI_SYSTEM_TABLE *st, EFI_STATUS s)
{
    CHAR16 buf[20];
    UINTN i;

    buf[0] = L'0';
    buf[1] = L'x';
    for (i = 0; i < 16u; i++) {
        unsigned d = (unsigned)((s >> (60u - 4u * i)) & 0xFu);
        buf[2 + i] = (CHAR16)(d < 10u ? L'0' + d : L'a' + d - 10u);
    }
    buf[18] = L'\r';
    buf[19] = 0;
    st->ConOut->OutputString(st->ConOut, L"FALCON-RELAUNCH ");
    st->ConOut->OutputString(st->ConOut, buf);
    st->ConOut->OutputString(st->ConOut, L"\n");
}

/* The volume's own path with a file path node for target after it. */
static EFI_STATUS file_path(EFI_BOOT_SERVICES *bs,
                            const EFI_DEVICE_PATH_PROTOCOL *vol,
                            EFI_DEVICE_PATH_PROTOCOL **out)
{
    UINTN head = dp_total_len(vol) - sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN node_len = sizeof(EFI_DEVICE_PATH_PROTOCOL) + sizeof(target);
    UINT8 *buf = (UINT8 *)0;
    EFI_DEVICE_PATH_PROTOCOL *node;
    EFI_STATUS s;

    s = bs->AllocatePool(EfiLoaderData,
                         head + node_len + sizeof(EFI_DEVICE_PATH_PROTOCOL),
                         (void **)&buf);
    if (EFI_ERROR(s))
        return s;
    copy_bytes(buf, (const UINT8 *)vol, head);
    node = (EFI_DEVICE_PATH_PROTOCOL *)(void *)(buf + head);
    node->Type = MEDIA_DEVICE_PATH;
    node->SubType = MEDIA_FILEPATH_DP;
    dp_set_node_len(node, (UINT16)node_len);
    copy_bytes(buf + head + sizeof(*node), (const UINT8 *)target,
               sizeof(target));
    dp_set_end((EFI_DEVICE_PATH_PROTOCOL *)(void *)(buf + head + node_len));
    *out = (EFI_DEVICE_PATH_PROTOCOL *)(void *)buf;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    UINTN n = 0, i;
    EFI_STATUS s;

    st->ConOut->OutputString(st->ConOut, L"\r\nFALCON-RELAUNCH-START\r\n");
    s = bs->LocateHandleBuffer(ByProtocol, &sfs_guid, (void *)0, &n,
                               &handles);
    if (EFI_ERROR(s)) {
        say_status(st, s);
        return s;
    }
    s = EFI_NOT_FOUND;
    for (i = 0; i < n; i++) {
        EFI_DEVICE_PATH_PROTOCOL *vol = (EFI_DEVICE_PATH_PROTOCOL *)0;
        EFI_DEVICE_PATH_PROTOCOL *path = (EFI_DEVICE_PATH_PROTOCOL *)0;
        EFI_HANDLE child = (EFI_HANDLE)0;
        EFI_STATUS ls;

        if (bs->HandleProtocol(handles[i], &dp_guid, (void **)&vol) !=
                EFI_SUCCESS ||
            vol == (EFI_DEVICE_PATH_PROTOCOL *)0)
            continue;
        if (EFI_ERROR(file_path(bs, vol, &path)))
            continue;
        ls = bs->LoadImage(FALSE, image_handle, path, (void *)0, 0, &child);
        bs->FreePool(path);
        if (EFI_ERROR(ls))
            continue;
        st->ConOut->OutputString(st->ConOut,
                                 L"FALCON-RELAUNCH-LOADED\r\n");
        s = bs->StartImage(child, (UINTN *)0, (CHAR16 **)0);
        break;
    }
    bs->FreePool(handles);
    say_status(st, s);
    bs->Stall(2000000);
    return s;
}
