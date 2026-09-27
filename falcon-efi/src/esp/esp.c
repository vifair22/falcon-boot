/* Images on an ESP, loaded by device path. See esp.h. */

#include <esp/esp.h>

#include <discover/discover.h>
#include <efi/blockio.h>
#include <efi/devpath.h>
#include <efi/loadedimage.h>
#include <efi/sfs.h>
#include <lib/string.h>
#include <lib/ucs2.h>
#include <loader/loadops.h>

EFI_HANDLE esp_own(EFI_SYSTEM_TABLE *st, EFI_HANDLE image)
{
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = (EFI_LOADED_IMAGE_PROTOCOL *)0;

    if (st == (EFI_SYSTEM_TABLE *)0 || image == (EFI_HANDLE)0 ||
        st->BootServices->HandleProtocol(image, &li_guid, (void **)&li) !=
            EFI_SUCCESS ||
        li == (EFI_LOADED_IMAGE_PROTOCOL *)0)
        return (EFI_HANDLE)0;
    return li->DeviceHandle;
}

void esp_connect_all(EFI_SYSTEM_TABLE *st)
{
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN i;

    if (st == (EFI_SYSTEM_TABLE *)0)
        return;
    for (i = 0; i < discover_count(); i++) {
        discover_dev *d = discover_at(i);
        void *sfs = (void *)0;

        if (d == (discover_dev *)0 || !d->media_present ||
            !d->bio->Media->LogicalPartition)
            continue;
        if (st->BootServices->HandleProtocol(d->handle, &sfs_guid, &sfs) ==
            EFI_SUCCESS)
            continue;
        /* Not recursive: a partition has no children a filesystem driver
         * would make. A partition no driver binds (NTFS, LUKS2, ext4)
         * answers EFI_NOT_FOUND, which is the common, harmless case. */
        (void)st->BootServices->ConnectController(
            d->handle, (EFI_HANDLE *)0, (EFI_DEVICE_PATH_PROTOCOL *)0, FALSE);
    }
}

/* The file, opened read-only, or NULL. *root receives the volume's root,
 * which the caller closes after the file. */
static EFI_FILE_PROTOCOL *open_file(EFI_SYSTEM_TABLE *st, EFI_HANDLE handle,
                                    const CHAR16 *path,
                                    EFI_FILE_PROTOCOL **root)
{
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    EFI_FILE_PROTOCOL *f = (EFI_FILE_PROTOCOL *)0;

    *root = (EFI_FILE_PROTOCOL *)0;
    if (st == (EFI_SYSTEM_TABLE *)0 || handle == (EFI_HANDLE)0 ||
        path == (const CHAR16 *)0)
        return (EFI_FILE_PROTOCOL *)0;
    if (st->BootServices->HandleProtocol(handle, &sfs_guid, (void **)&sfs) !=
            EFI_SUCCESS ||
        sfs == (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0)
        return (EFI_FILE_PROTOCOL *)0;
    if (sfs->OpenVolume(sfs, root) != EFI_SUCCESS ||
        *root == (EFI_FILE_PROTOCOL *)0) {
        *root = (EFI_FILE_PROTOCOL *)0;
        return (EFI_FILE_PROTOCOL *)0;
    }
    /* The spec's parameter is non-const; the firmware does not write it. */
    if ((*root)->Open(*root, &f, (CHAR16 *)(uintptr_t)path,
                      EFI_FILE_MODE_READ, 0) != EFI_SUCCESS ||
        f == (EFI_FILE_PROTOCOL *)0) {
        (*root)->Close(*root);
        *root = (EFI_FILE_PROTOCOL *)0;
        return (EFI_FILE_PROTOCOL *)0;
    }
    return f;
}

static void close_file(EFI_FILE_PROTOCOL *f, EFI_FILE_PROTOCOL *root)
{
    if (f != (EFI_FILE_PROTOCOL *)0)
        f->Close(f);
    if (root != (EFI_FILE_PROTOCOL *)0)
        root->Close(root);
}

BOOLEAN esp_has_file(EFI_SYSTEM_TABLE *st, EFI_HANDLE handle,
                     const CHAR16 *path)
{
    EFI_FILE_PROTOCOL *root;
    EFI_FILE_PROTOCOL *f = open_file(st, handle, path, &root);

    if (f == (EFI_FILE_PROTOCOL *)0)
        return FALSE;
    close_file(f, root);
    return TRUE;
}

EFI_STATUS esp_read_file(EFI_SYSTEM_TABLE *st, EFI_HANDLE handle,
                         const CHAR16 *path, void *buf, UINTN cap,
                         UINTN *len)
{
    EFI_FILE_PROTOCOL *root;
    EFI_FILE_PROTOCOL *f;
    UINTN got = 0;
    EFI_STATUS status = EFI_SUCCESS;

    if (buf == (void *)0 || len == (UINTN *)0 || cap == 0)
        return EFI_INVALID_PARAMETER;
    *len = 0;
    f = open_file(st, handle, path, &root);
    if (f == (EFI_FILE_PROTOCOL *)0)
        return EFI_NOT_FOUND;

    /* Read until the file ends or the buffer is full; a byte more then
     * says whether it was all there. */
    while (got < cap) {
        UINTN n = cap - got;

        status = f->Read(f, &n, (UINT8 *)buf + got);
        if (EFI_ERROR(status) || n == 0)
            break;
        got += n;
    }
    if (!EFI_ERROR(status) && got == cap) {
        UINT8 more;
        UINTN n = 1;

        if (f->Read(f, &n, &more) == EFI_SUCCESS && n != 0)
            status = EFI_BUFFER_TOO_SMALL;
    }
    close_file(f, root);
    if (EFI_ERROR(status))
        return status;
    *len = got;
    return EFI_SUCCESS;
}

UINTN esp_find_all(EFI_SYSTEM_TABLE *st, const CHAR16 *path, esp_vol *out,
                   UINTN cap)
{
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    UINTN count = 0;
    UINTN n = 0;
    UINTN i;

    if (st == (EFI_SYSTEM_TABLE *)0 || out == (esp_vol *)0 || cap == 0)
        return 0;
    if (st->BootServices->LocateHandleBuffer(ByProtocol, &sfs_guid,
                                             (void *)0, &count, &handles) !=
        EFI_SUCCESS)
        return 0;
    for (i = 0; i < count && n < cap; i++) {
        if (!esp_has_file(st, handles[i], path))
            continue;
        out[n].handle = handles[i];
        out[n].has_part = (UINT8)(disks_part_of(st, handles[i],
                                                &out[n].part) ==
                                  EFI_SUCCESS);
        /* A volume that is not a partition of a disk (a firmware volume, a
         * RAM disk) is not somewhere a system is installed. */
        if (!out[n].has_part)
            continue;
        n++;
    }
    st->BootServices->FreePool(handles);
    return n;
}

/* The volume's device path with `path` appended as a FILEPATH node: what
 * LoadImage is given, so the started image's LoadedImage->DeviceHandle is
 * that volume and Windows Boot Manager finds its BCD beside it. */
static EFI_STATUS file_path(EFI_BOOT_SERVICES *bs, EFI_HANDLE handle,
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
    status = bs->HandleProtocol(handle, &dp_guid, (void **)&vol);
    if (EFI_ERROR(status))
        return status;
    vol_len = dp_total_len(vol);
    if (vol_len < end_size || node_len > 0xFFFFu)
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

EFI_STATUS esp_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent,
                    EFI_HANDLE handle, const CHAR16 *path,
                    void (*handover)(void), esp_result *result)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_DEVICE_PATH_PROTOCOL *load_path = (EFI_DEVICE_PATH_PROTOCOL *)0;
    EFI_HANDLE child = (EFI_HANDLE)0;
    CHAR16 *exit_data = (CHAR16 *)0;
    UINTN exit_size = 0;
    load_ops ops;
    EFI_STATUS status;

    result->stage = L"starting";
    result->status = EFI_SUCCESS;
    result->advice = (const CHAR16 *)0;

    result->stage = L"building the image's device path";
    status = file_path(bs, handle, path, &load_path);
    if (EFI_ERROR(status))
        return status;

    resolve_load_ops(bs, &ops);

    /* Shim present but not enforcing: the firmware loads the image, as the
     * ISO chainload does and for its reason (DESIGN Section 7.6). Windows
     * Boot Manager loads winload.efi through the firmware's LoadImage with
     * its own handle as the parent, and an image shim's loader created is
     * not one the firmware will take as a parent. With shim not enforcing
     * neither loader verifies anything the other would refuse, except that
     * the firmware still enforces db while Secure Boot is on and only
     * shim's validation is off; that refusal is answered with shim's loader
     * below, which is the user's MokSBState choice honoured. */
    status = EFI_NOT_FOUND;
    if (ops.via_shim && ops.table_unhooked) {
        result->stage = L"LoadImage (firmware)";
        status = bs->LoadImage(FALSE, parent, load_path, (void *)0, 0,
                               &child);
        if (status == EFI_SUCCESS) {
            ops.LoadImage = bs->LoadImage;
            ops.StartImage = bs->StartImage;
            ops.UnloadImage = bs->UnloadImage;
            ops.via_shim = FALSE;
        } else {
            if (status == EFI_SECURITY_VIOLATION && child != (EFI_HANDLE)0)
                bs->UnloadImage(child);
            child = (EFI_HANDLE)0;
        }
    }
    if (status != EFI_SUCCESS) {
        result->stage = ops.via_shim ? L"LoadImage (shim)"
                                     : L"LoadImage (firmware)";
        status = ops.LoadImage(FALSE, parent, load_path, (void *)0, 0,
                               &child);
    }
    bs->FreePool(load_path);
    if (EFI_ERROR(status)) {
        /* A refusal under Secure Boot leaves a loaded image behind (spec
         * 7.4), unloaded by the loader that made it. */
        if (status == EFI_SECURITY_VIOLATION && child != (EFI_HANDLE)0)
            ops.UnloadImage(child);
        if (status == EFI_SECURITY_VIOLATION)
            result->advice =
                L"the image is not signed with a key this machine trusts.";
        result->status = status;
        return status;
    }

    if (handover != (void (*)(void))0)
        handover();

    result->stage = L"StartImage";
    status = ops.StartImage(child, &exit_size, &exit_data);
    if (exit_data != (CHAR16 *)0)
        bs->FreePool(exit_data);
    /* The firmware arms a watchdog around a started image. */
    bs->SetWatchdogTimer(0, 0, 0, (CHAR16 *)0);

    result->stage = L"image exit";
    result->status = status;
    return EFI_SUCCESS;
}
