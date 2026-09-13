/* Virtual CD over an exFAT-backed ISO file, plus the chainload of the ESP
 * the firmware finds inside it. See src/iso/iso.h for the contract and
 * docs/DESIGN.md Section 6 for why the firmware, not us, does the parsing.
 */

#include <iso/iso.h>
#include <efi/blockio.h>
#include <efi/devpath.h>
#include <efi/sfs.h>
#include <lib/string.h>

/* El Torito lives on 2048-byte media; the firmware's partition driver only
 * looks for a boot catalog when the block size says CD-ROM. */
#define ISO_BLOCK_SIZE 2048u

/* Identifies our synthetic media in device paths. A vendor-defined node is
 * the spec's answer for a device with no real bus behind it. */
#define AIRIES_ISO_MEDIA_GUID \
    { 0x887579f6, 0x4287, 0x4bc4, \
      { 0x99, 0x92, 0x15, 0xa3, 0xb5, 0x61, 0xce, 0x63 } }

/* Removable-media default path: every El Torito ESP that expects to be
 * booted by firmware puts its loader here (UEFI spec 3.5.1.1). */
static const CHAR16 boot_file_path[] = L"\\EFI\\BOOT\\BOOTX64.EFI";

typedef struct {
    VENDOR_DEVICE_PATH       vendor;
    EFI_DEVICE_PATH_PROTOCOL end;
} iso_device_path;

/* Device path nodes are byte-packed; a compiler that padded this struct
 * would publish a path the firmware cannot walk. */
_Static_assert(sizeof(iso_device_path) ==
                   sizeof(VENDOR_DEVICE_PATH) +
                       sizeof(EFI_DEVICE_PATH_PROTOCOL),
               "device path nodes must pack without padding");

typedef struct {
    /* First member on purpose: the interface pointer the firmware hands back
     * to our callbacks is the address of the instance. */
    EFI_BLOCK_IO_PROTOCOL bio;
    EFI_BLOCK_IO_MEDIA    media;
    iso_device_path       path;
    exfat_file            file;
    EFI_HANDLE            handle;
    UINT8                 in_use;
    UINT8                 connected;
} iso_device;

/* One ISO boots at a time, and the instance must outlive every firmware
 * driver that binds to it, so it is static rather than pool-allocated. */
static iso_device g_dev;
static UINT32 g_next_media_id = 1;

static iso_device *device_of(EFI_BLOCK_IO_PROTOCOL *self)
{
    return (iso_device *)(void *)self;
}

/* ---- the published Block I/O instance ---- */

static EFI_STATUS EFIAPI iso_reset(EFI_BLOCK_IO_PROTOCOL *self,
                                   BOOLEAN extended)
{
    (void)self;
    (void)extended;
    /* Nothing to reset: the backing store is a file we hold open. */
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI iso_read_blocks(EFI_BLOCK_IO_PROTOCOL *self,
                                         UINT32 media_id, EFI_LBA lba,
                                         UINTN size, void *buffer)
{
    iso_device *dev = device_of(self);
    uint64_t off;
    uint64_t avail;

    if (media_id != dev->media.MediaId)
        return EFI_MEDIA_CHANGED;
    if (size == 0)
        return EFI_SUCCESS;
    if (buffer == (void *)0)
        return EFI_INVALID_PARAMETER;
    if (size % ISO_BLOCK_SIZE != 0)
        return EFI_BAD_BUFFER_SIZE;
    if (lba > dev->media.LastBlock)
        return EFI_INVALID_PARAMETER;

    off = (uint64_t)lba * ISO_BLOCK_SIZE;
    if (size / ISO_BLOCK_SIZE - 1 > dev->media.LastBlock - lba)
        return EFI_INVALID_PARAMETER;

    /* An ISO whose length is not a whole number of 2048-byte blocks still
     * advertises that last partial block, so serve the bytes that exist and
     * zero the rest rather than failing the read. */
    avail = dev->file.size - off;
    if (avail > (uint64_t)size)
        avail = (uint64_t)size;
    if (exfat_file_read(&dev->file, off, avail, buffer) != EXFAT_OK)
        return EFI_DEVICE_ERROR;
    if (avail < (uint64_t)size)
        memset((UINT8 *)buffer + avail, 0, (size_t)((uint64_t)size - avail));
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI iso_write_blocks(EFI_BLOCK_IO_PROTOCOL *self,
                                          UINT32 media_id, EFI_LBA lba,
                                          UINTN size, void *buffer)
{
    (void)self;
    (void)media_id;
    (void)lba;
    (void)size;
    (void)buffer;
    /* The media advertises ReadOnly, so a well-behaved driver never gets
     * here; answering honestly beats a null pointer for one that does. */
    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI iso_flush_blocks(EFI_BLOCK_IO_PROTOCOL *self)
{
    (void)self;
    return EFI_SUCCESS;
}

/* ---- device lifecycle ---- */

static void device_init(iso_device *dev, const exfat_dirent *file)
{
    EFI_GUID vendor_guid = AIRIES_ISO_MEDIA_GUID;

    dev->media.MediaId = g_next_media_id++;
    dev->media.RemovableMedia = TRUE;
    dev->media.MediaPresent = TRUE;
    dev->media.LogicalPartition = FALSE;
    dev->media.ReadOnly = TRUE;
    dev->media.WriteCaching = FALSE;
    dev->media.BlockSize = ISO_BLOCK_SIZE;
    dev->media.IoAlign = 0;
    dev->media.LastBlock = file->size / ISO_BLOCK_SIZE - 1;

    dev->bio.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
    dev->bio.Media = &dev->media;
    dev->bio.Reset = iso_reset;
    dev->bio.ReadBlocks = iso_read_blocks;
    dev->bio.WriteBlocks = iso_write_blocks;
    dev->bio.FlushBlocks = iso_flush_blocks;

    dev->path.vendor.Header.Type = MEDIA_DEVICE_PATH;
    dev->path.vendor.Header.SubType = MEDIA_VENDOR_DP;
    dp_set_node_len(&dev->path.vendor.Header,
                    (UINT16)sizeof(dev->path.vendor));
    dev->path.vendor.Guid = vendor_guid;
    dp_set_end(&dev->path.end);

    dev->handle = (EFI_HANDLE)0;
    dev->connected = 0;
}

static EFI_STATUS device_teardown(EFI_BOOT_SERVICES *bs, iso_device *dev)
{
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_STATUS status;

    if (!dev->in_use)
        return EFI_SUCCESS;

    if (dev->connected) {
        /* Children first. The firmware's disk, partition and FAT drivers
         * hold our interfaces open; pulling the interfaces out from under
         * them is how a virtual device turns into a hung machine. */
        bs->DisconnectController(dev->handle, (EFI_HANDLE)0, (EFI_HANDLE)0);
        dev->connected = 0;
    }

    status = bs->UninstallMultipleProtocolInterfaces(
        dev->handle,
        &bio_guid, &dev->bio,
        &dp_guid, &dev->path,
        (void *)0);
    if (EFI_ERROR(status)) {
        /* A driver we could not stop still points at this instance. It is
         * static so it stays valid, but it must never be handed to a second
         * ISO: leaving in_use set is what refuses that. */
        return status;
    }

    dev->handle = (EFI_HANDLE)0;
    dev->in_use = 0;
    return EFI_SUCCESS;
}

/* ---- finding the ESP the firmware mounted inside the ISO ---- */

static EFI_STATUS sfs_handles(EFI_BOOT_SERVICES *bs, EFI_HANDLE **buf,
                              UINTN *count)
{
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_STATUS status;

    *buf = (EFI_HANDLE *)0;
    *count = 0;
    status = bs->LocateHandleBuffer(ByProtocol, &sfs_guid, (void *)0,
                                    count, buf);
    if (status == EFI_NOT_FOUND) {
        *buf = (EFI_HANDLE *)0;
        *count = 0;
        return EFI_SUCCESS;
    }
    return status;
}

static BOOLEAN handle_in(EFI_HANDLE h, EFI_HANDLE *set, UINTN n)
{
    UINTN i;

    for (i = 0; i < n; i++) {
        if (set[i] == h)
            return TRUE;
    }
    return FALSE;
}

/* Does this handle's device path start with our virtual CD's vendor node? */
static BOOLEAN is_our_child(EFI_BOOT_SERVICES *bs, iso_device *dev,
                            EFI_HANDLE h)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *dp;

    if (bs->HandleProtocol(h, &dp_guid, (void **)&dp) != EFI_SUCCESS)
        return FALSE;
    if (dp_total_len(dp) <= sizeof(dev->path.vendor))
        return FALSE;
    return (BOOLEAN)(memcmp(dp, &dev->path.vendor,
                            sizeof(dev->path.vendor)) == 0);
}

/* The ISO's ESP is whichever Simple File System handle appeared while we were
 * connecting drivers. Comparing handle sets is firmer than matching device
 * paths alone, because it does not depend on how the firmware's partition
 * driver chooses to name its children; the device path is then used only to
 * break ties if several handles appeared at once. */
static EFI_STATUS find_iso_esp(EFI_BOOT_SERVICES *bs, iso_device *dev,
                               EFI_HANDLE *before, UINTN before_n,
                               EFI_HANDLE *out)
{
    EFI_HANDLE *after = (EFI_HANDLE *)0;
    EFI_HANDLE first_new = (EFI_HANDLE)0;
    EFI_HANDLE matched = (EFI_HANDLE)0;
    UINTN after_n = 0;
    UINTN i;
    EFI_STATUS status;

    status = sfs_handles(bs, &after, &after_n);
    if (EFI_ERROR(status))
        return status;

    for (i = 0; i < after_n; i++) {
        if (handle_in(after[i], before, before_n))
            continue;
        if (first_new == (EFI_HANDLE)0)
            first_new = after[i];
        if (matched == (EFI_HANDLE)0 && is_our_child(bs, dev, after[i]))
            matched = after[i];
    }

    if (after != (EFI_HANDLE *)0)
        bs->FreePool(after);

    *out = (matched != (EFI_HANDLE)0) ? matched : first_new;
    return (*out != (EFI_HANDLE)0) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

/* ---- chainload ---- */

/* Appends the removable-media loader path to the ESP's own device path.
 * LoadImage is given a path, never a buffer: that is what makes the child's
 * LoadedImage->DeviceHandle the ISO's ESP (so a distro bootloader finds its
 * own configuration) and what keeps the security decision the platform's. */
static EFI_STATUS build_load_path(EFI_BOOT_SERVICES *bs,
                                  EFI_DEVICE_PATH_PROTOCOL *esp_path,
                                  EFI_DEVICE_PATH_PROTOCOL **out)
{
    UINTN end_size = sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN esp_len = dp_total_len(esp_path);
    UINTN node_len = end_size + sizeof(boot_file_path);
    UINTN prefix;
    UINT8 *buf = (UINT8 *)0;
    EFI_DEVICE_PATH_PROTOCOL *node;
    EFI_STATUS status;

    if (esp_len < end_size)
        return EFI_INVALID_PARAMETER;
    prefix = esp_len - end_size; /* everything but the ESP path's end node */

    status = bs->AllocatePool(EfiLoaderData, prefix + node_len + end_size,
                              (void **)&buf);
    if (EFI_ERROR(status))
        return status;

    memcpy(buf, esp_path, prefix);

    node = (EFI_DEVICE_PATH_PROTOCOL *)(buf + prefix);
    node->Type = MEDIA_DEVICE_PATH;
    node->SubType = MEDIA_FILEPATH_DP;
    dp_set_node_len(node, (UINT16)node_len);
    memcpy(buf + prefix + end_size, boot_file_path, sizeof(boot_file_path));

    dp_set_end((EFI_DEVICE_PATH_PROTOCOL *)(buf + prefix + node_len));

    *out = (EFI_DEVICE_PATH_PROTOCOL *)buf;
    return EFI_SUCCESS;
}

static EFI_STATUS chainload(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                            EFI_HANDLE esp_handle, iso_boot_result *result)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *esp_path;
    EFI_DEVICE_PATH_PROTOCOL *load_path = (EFI_DEVICE_PATH_PROTOCOL *)0;
    EFI_HANDLE child = (EFI_HANDLE)0;
    CHAR16 *exit_data = (CHAR16 *)0;
    UINTN exit_size = 0;
    EFI_STATUS status;

    result->stage = L"reading the ISO ESP's device path";
    status = bs->HandleProtocol(esp_handle, &dp_guid, (void **)&esp_path);
    if (EFI_ERROR(status))
        return status;

    result->stage = L"building the payload's device path";
    status = build_load_path(bs, esp_path, &load_path);
    if (EFI_ERROR(status))
        return status;

    result->stage = L"LoadImage of \\EFI\\BOOT\\BOOTX64.EFI";
    status = bs->LoadImage(FALSE, parent_image, load_path, (void *)0, 0,
                           &child);
    bs->FreePool(load_path);
    if (EFI_ERROR(status)) {
        /* A refusal under Secure Boot is the one failure that still leaves a
         * loaded image behind (spec 7.4): the platform read and relocated it,
         * then declined to run it. Unloading is the caller's job, and not
         * doing it leaks the image on every refused entry. */
        if (status == EFI_SECURITY_VIOLATION && child != (EFI_HANDLE)0)
            bs->UnloadImage(child);
        return status;
    }

    result->stage = L"StartImage";
    status = bs->StartImage(child, &exit_size, &exit_data);
    /* StartImage unloads an application that returns, so the child handle is
     * gone either way; only its exit data is ours to release. */
    if (exit_data != (CHAR16 *)0)
        bs->FreePool(exit_data);
    /* The firmware arms a watchdog around a started image; disarm it again
     * so a long stay in the menu afterwards cannot reset the machine. */
    bs->SetWatchdogTimer(0, 0, 0, (CHAR16 *)0);

    result->stage = L"payload exit";
    result->status = status;
    return EFI_SUCCESS;
}

/* ---- entry point ---- */

EFI_STATUS iso_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                    exfat_vol *vol, const exfat_dirent *file,
                    iso_boot_result *result)
{
    EFI_BOOT_SERVICES *bs;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_HANDLE *before = (EFI_HANDLE *)0;
    EFI_HANDLE esp_handle = (EFI_HANDLE)0;
    UINTN before_n = 0;
    EFI_STATUS status;
    EFI_STATUS teardown;

    if (st == (void *)0 || vol == (void *)0 || file == (void *)0 ||
        result == (void *)0)
        return EFI_INVALID_PARAMETER;

    bs = st->BootServices;
    result->stage = L"preparing the virtual CD";
    result->status = EFI_SUCCESS;

    if (g_dev.in_use) {
        result->stage = L"previous virtual CD still held by a driver";
        result->status = EFI_ACCESS_DENIED;
        return EFI_ACCESS_DENIED;
    }
    if (file->size < ISO_BLOCK_SIZE) {
        result->stage = L"ISO is too small to be CD media";
        result->status = EFI_UNSUPPORTED;
        return EFI_UNSUPPORTED;
    }
    if (exfat_file_open(vol, &g_dev.file, file) != EXFAT_OK) {
        result->stage = L"opening the ISO on the exFAT volume";
        result->status = EFI_DEVICE_ERROR;
        return EFI_DEVICE_ERROR;
    }

    device_init(&g_dev, file);

    result->stage = L"publishing the virtual CD";
    status = bs->InstallMultipleProtocolInterfaces(
        &g_dev.handle,
        &bio_guid, &g_dev.bio,
        &dp_guid, &g_dev.path,
        (void *)0);
    if (EFI_ERROR(status)) {
        result->status = status;
        return status;
    }
    g_dev.in_use = 1;

    /* Snapshot before connecting: the ISO's ESP is the filesystem that is
     * not here yet. */
    result->stage = L"enumerating filesystems";
    status = sfs_handles(bs, &before, &before_n);
    if (EFI_ERROR(status))
        goto out;

    /* The firmware's own partition driver parses El Torito and its FAT
     * driver mounts the embedded ESP; we write no ISO9660 reader for this
     * (DESIGN Section 6 stage 3). */
    result->stage = L"ConnectController on the virtual CD";
    /* Set before the call: a partial bind still leaves drivers attached, and
     * teardown must disconnect them. */
    g_dev.connected = 1;
    status = bs->ConnectController(g_dev.handle, (EFI_HANDLE *)0,
                                   (EFI_DEVICE_PATH_PROTOCOL *)0, TRUE);
    if (EFI_ERROR(status))
        goto out;

    result->stage = L"finding the ISO's EFI System Partition";
    status = find_iso_esp(bs, &g_dev, before, before_n, &esp_handle);
    if (EFI_ERROR(status))
        goto out;

    status = chainload(st, parent_image, esp_handle, result);

out:
    if (before != (EFI_HANDLE *)0)
        bs->FreePool(before);
    if (EFI_ERROR(status))
        result->status = status;

    teardown = device_teardown(bs, &g_dev);
    if (EFI_ERROR(teardown)) {
        /* Worth overriding a payload's own exit status: from here on the
         * loader refuses further ISO boots, and the user needs to know. */
        result->stage = L"releasing the virtual CD";
        result->status = teardown;
        return teardown;
    }
    return status;
}
