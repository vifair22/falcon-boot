/* Standalone spike: publish the SAME virtual CD-ROM that
 * falcon-efi/src/iso/iso.c publishes, connect drivers to it, and report exactly
 * which filesystems the firmware surfaced on it.
 *
 * The media descriptor and device path here are copied verbatim from
 * falcon-efi/src/iso/iso.c device_init(). The only difference is the backing
 * store: the loader reads the ISO out of a file on exFAT, this reads it out of
 * a parent Block I/O handle (the ISO attached to QEMU as a raw disk). Nothing
 * the firmware's partition/filesystem drivers can see depends on that
 * difference, so the binding behaviour this measures is the loader's binding
 * behaviour.
 *
 * Deliberately outside src/: this never ships.
 */

#include <efi/uefi.h>
#include <efi/blockio.h>
#include <efi/devpath.h>
#include <efi/sfs.h>

#define ISO_CD_BLOCK_SIZE 2048u

#define FALCON_ISO_MEDIA_GUID \
    { 0x887579f6, 0x4287, 0x4bc4, \
      { 0x99, 0x92, 0x15, 0xa3, 0xb5, 0x61, 0xce, 0x63 } }

#define EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID \
    { 0x8b843e20, 0x8132, 0x4852, \
      { 0x90, 0xcc, 0x55, 0x1a, 0x4e, 0x4a, 0x7f, 0x1c } }

#define EFI_DISK_IO_PROTOCOL_GUID \
    { 0xce345171, 0xba0b, 0x11d2, \
      { 0x8e, 0x4f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_PARTITION_INFO_PROTOCOL_GUID \
    { 0x8cf2f62c, 0xbc9b, 0x4821, \
      { 0x80, 0x8d, 0xec, 0x9e, 0xc4, 0x21, 0xa1, 0xa0 } }

typedef struct {
    CHAR16 *(EFIAPI *ConvertDeviceNodeToText)(const EFI_DEVICE_PATH_PROTOCOL *,
                                              BOOLEAN, BOOLEAN);
    CHAR16 *(EFIAPI *ConvertDevicePathToText)(const EFI_DEVICE_PATH_PROTOCOL *,
                                              BOOLEAN, BOOLEAN);
} EFI_DEVICE_PATH_TO_TEXT_PROTOCOL;

typedef EFI_STATUS(EFIAPI *protocols_per_handle_fn)(EFI_HANDLE, EFI_GUID ***,
                                                    UINTN *);

typedef struct {
    VENDOR_DEVICE_PATH       vendor;
    EFI_DEVICE_PATH_PROTOCOL end;
} vcd_path;

typedef struct {
    EFI_BLOCK_IO_PROTOCOL  bio;
    EFI_BLOCK_IO_MEDIA     media;
    vcd_path               path;
    EFI_BLOCK_IO_PROTOCOL *parent;
    UINT64                 base;
    EFI_HANDLE             handle;
} vcd_device;

static vcd_device g_dev;
static EFI_SYSTEM_TABLE *g_st;
static EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *g_d2t;

/* ---- tiny output helpers ---- */

static void out(const CHAR16 *s)
{
    g_st->ConOut->OutputString(g_st->ConOut, (CHAR16 *)s);
}

static void out_hex(UINT64 v, int digits)
{
    static const CHAR16 tbl[] = L"0123456789ABCDEF";
    CHAR16 buf[19];
    int i;

    for (i = 0; i < digits; i++)
        buf[digits - 1 - i] = tbl[(v >> (i * 4)) & 0xFu];
    buf[digits] = 0;
    out(buf);
}

static void out_dec(UINT64 v)
{
    CHAR16 buf[21];
    int i = 20;

    buf[20] = 0;
    if (v == 0) {
        out(L"0");
        return;
    }
    while (v != 0 && i > 0) {
        buf[--i] = (CHAR16)(L'0' + (v % 10));
        v /= 10;
    }
    out(buf + i);
}

static void out_nl(void)
{
    out(L"\r\n");
}

static void out_guid(const EFI_GUID *g)
{
    const UINT8 *d = (const UINT8 *)g;
    int i;

    out_hex(g->Data1, 8);
    out(L"-");
    out_hex(g->Data2, 4);
    out(L"-");
    out_hex(g->Data3, 4);
    out(L"-");
    for (i = 0; i < 8; i++) {
        if (i == 2)
            out(L"-");
        out_hex(d[8 + i], 2);
    }
}

static void out_path(EFI_HANDLE h)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *dp;
    CHAR16 *text;

    if (g_st->BootServices->HandleProtocol(h, &dp_guid, (void **)&dp) !=
        EFI_SUCCESS) {
        out(L"(no device path)");
        return;
    }
    if (g_d2t == (EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *)0) {
        out(L"(no DevicePathToText)");
        return;
    }
    text = g_d2t->ConvertDevicePathToText(dp, FALSE, FALSE);
    if (text == (CHAR16 *)0) {
        out(L"(conversion failed)");
        return;
    }
    out(text);
    g_st->BootServices->FreePool(text);
}

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    const UINT8 *p = (const UINT8 *)a;
    const UINT8 *q = (const UINT8 *)b;
    int i;

    for (i = 0; i < 16; i++) {
        if (p[i] != q[i])
            return 0;
    }
    return 1;
}

/* ---- the published Block I/O instance ---- */

static EFI_STATUS EFIAPI vcd_reset(EFI_BLOCK_IO_PROTOCOL *self,
                                   BOOLEAN extended)
{
    (void)self;
    (void)extended;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI vcd_read(EFI_BLOCK_IO_PROTOCOL *self, UINT32 media_id,
                                  EFI_LBA lba, UINTN size, void *buffer)
{
    vcd_device *dev = (vcd_device *)(void *)self;
    EFI_BLOCK_IO_PROTOCOL *p = dev->parent;
    UINT64 off;
    UINT64 pbs;

    if (media_id != dev->media.MediaId)
        return EFI_MEDIA_CHANGED;
    if (size == 0)
        return EFI_SUCCESS;
    if (buffer == (void *)0)
        return EFI_INVALID_PARAMETER;
    if (size % dev->media.BlockSize != 0)
        return EFI_BAD_BUFFER_SIZE;
    if (lba > dev->media.LastBlock)
        return EFI_INVALID_PARAMETER;

    off = dev->base + (UINT64)lba * dev->media.BlockSize;
    pbs = p->Media->BlockSize;
    if (off % pbs != 0 || size % pbs != 0)
        return EFI_INVALID_PARAMETER;
    return p->ReadBlocks(p, p->Media->MediaId, off / pbs, size, buffer);
}

static EFI_STATUS EFIAPI vcd_write(EFI_BLOCK_IO_PROTOCOL *self, UINT32 media_id,
                                   EFI_LBA lba, UINTN size, void *buffer)
{
    (void)self;
    (void)media_id;
    (void)lba;
    (void)size;
    (void)buffer;
    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI vcd_flush(EFI_BLOCK_IO_PROTOCOL *self)
{
    (void)self;
    return EFI_SUCCESS;
}

/* Copied from falcon-efi/src/iso/iso.c device_init(): this is the presentation
 * under test, so it must not drift from the loader's. */
static void device_init(vcd_device *dev, UINT64 size)
{
    EFI_GUID vendor_guid = FALCON_ISO_MEDIA_GUID;

    dev->media.MediaId = 1;
    dev->media.RemovableMedia = TRUE;
    dev->media.MediaPresent = TRUE;
    dev->media.LogicalPartition = FALSE;
    dev->media.ReadOnly = TRUE;
    dev->media.WriteCaching = FALSE;
    dev->media.BlockSize = ISO_CD_BLOCK_SIZE;
    dev->media.IoAlign = 0;
    dev->media.LastBlock = size / ISO_CD_BLOCK_SIZE - 1;

    dev->bio.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
    dev->bio.Media = &dev->media;
    dev->bio.Reset = vcd_reset;
    dev->bio.ReadBlocks = vcd_read;
    dev->bio.WriteBlocks = vcd_write;
    dev->bio.FlushBlocks = vcd_flush;

    dev->path.vendor.Header.Type = MEDIA_DEVICE_PATH;
    dev->path.vendor.Header.SubType = MEDIA_VENDOR_DP;
    dp_set_node_len(&dev->path.vendor.Header, (UINT16)sizeof(dev->path.vendor));
    dev->path.vendor.Guid = vendor_guid;
    dp_set_end(&dev->path.end);
}

/* ---- finding the ISO bytes ---- */

/* The parent is whichever whole (non-partition) block device carries "CD001"
 * at byte 32768, i.e. the raw ISO attached to QEMU as a disk. */
static EFI_STATUS find_iso_parent(EFI_BOOT_SERVICES *bs,
                                  EFI_BLOCK_IO_PROTOCOL **found)
{
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    UINTN count = 0;
    UINTN i;
    EFI_STATUS status;
    static UINT8 sector[4096];

    status = bs->LocateHandleBuffer(ByProtocol, &bio_guid, (void *)0, &count,
                                    &handles);
    if (EFI_ERROR(status))
        return status;

    for (i = 0; i < count; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio;
        UINT32 bs_bytes;
        UINT64 lba;

        if (bs->HandleProtocol(handles[i], &bio_guid, (void **)&bio) !=
            EFI_SUCCESS)
            continue;
        if (bio->Media->LogicalPartition || !bio->Media->MediaPresent)
            continue;
        bs_bytes = bio->Media->BlockSize;
        if (bs_bytes == 0 || bs_bytes > sizeof(sector) ||
            32768u % bs_bytes != 0)
            continue;
        lba = 32768u / bs_bytes;
        if (bio->ReadBlocks(bio, bio->Media->MediaId, lba, bs_bytes, sector) !=
            EFI_SUCCESS)
            continue;
        if (sector[1] == 'C' && sector[2] == 'D' && sector[3] == '0' &&
            sector[4] == '0' && sector[5] == '1') {
            out(L"parent block device: ");
            out_path(handles[i]);
            out(L"  bsize=");
            out_dec(bs_bytes);
            out(L" lastblock=");
            out_dec(bio->Media->LastBlock);
            out_nl();
            *found = bio;
            bs->FreePool(handles);
            return EFI_SUCCESS;
        }
    }
    bs->FreePool(handles);
    return EFI_NOT_FOUND;
}

/* ---- reporting ---- */

static void dump_handle(EFI_HANDLE h, UINTN idx)
{
    EFI_BOOT_SERVICES *bs = g_st->BootServices;
    protocols_per_handle_fn pph =
        (protocols_per_handle_fn)(void *)bs->ProtocolsPerHandle;
    EFI_GUID **guids = (EFI_GUID **)0;
    UINTN n = 0;
    UINTN i;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID dio_guid = EFI_DISK_IO_PROTOCOL_GUID;
    EFI_GUID pi_guid = EFI_PARTITION_INFO_PROTOCOL_GUID;

    out(L"  [");
    out_dec(idx);
    out(L"] ");
    out_path(h);
    out_nl();
    out(L"      protocols:");
    if (pph(h, &guids, &n) != EFI_SUCCESS) {
        out(L" (ProtocolsPerHandle failed)");
        out_nl();
        return;
    }
    for (i = 0; i < n; i++) {
        out(L" ");
        if (guid_eq(guids[i], &sfs_guid))
            out(L"SimpleFileSystem");
        else if (guid_eq(guids[i], &bio_guid))
            out(L"BlockIo");
        else if (guid_eq(guids[i], &dio_guid))
            out(L"DiskIo");
        else if (guid_eq(guids[i], &pi_guid))
            out(L"PartitionInfo");
        else
            out_guid(guids[i]);
    }
    out_nl();
    g_st->BootServices->FreePool(guids);

    if (bs->HandleProtocol(h, &bio_guid, (void **)&guids) == EFI_SUCCESS) {
        EFI_BLOCK_IO_PROTOCOL *bio;

        if (bs->HandleProtocol(h, &bio_guid, (void **)&bio) == EFI_SUCCESS) {
            out(L"      media: bsize=");
            out_dec(bio->Media->BlockSize);
            out(L" lastblock=");
            out_dec(bio->Media->LastBlock);
            out(L" logical=");
            out_dec(bio->Media->LogicalPartition);
            out(L" rev=0x");
            out_hex(bio->Revision, 8);
            out_nl();
        }
    }
}

static void probe_files(EFI_HANDLE h)
{
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
    EFI_FILE_PROTOCOL *root;
    EFI_FILE_PROTOCOL *f;
    static CHAR16 *paths[] = {
        L"\\EFI\\BOOT\\BOOTX64.EFI",
        L"\\efi\\boot\\bootx64.efi",
        L"\\efi\\microsoft\\boot\\bcd",
        L"\\EFI\\MICROSOFT\\BOOT\\BCD",
        L"\\sources\\boot.wim",
    };
    static UINT8 dirent[1024];
    UINTN i;

    if (g_st->BootServices->HandleProtocol(h, &sfs_guid, (void **)&sfs) !=
        EFI_SUCCESS)
        return;
    if (sfs->OpenVolume(sfs, &root) != EFI_SUCCESS) {
        out(L"      OpenVolume failed");
        out_nl();
        return;
    }
    out(L"      root entries:");
    for (;;) {
        UINTN n = sizeof(dirent);

        if (root->Read(root, &n, dirent) != EFI_SUCCESS || n == 0)
            break;
        out(L" ");
        /* EFI_FILE_INFO: Size(8) FileSize(8) PhysicalSize(8) 3x TIME(16)
         * Attribute(8) then the null-terminated name. */
        out((CHAR16 *)(void *)(dirent + 80));
    }
    out_nl();
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        EFI_STATUS s = root->Open(root, &f, paths[i], EFI_FILE_MODE_READ, 0);

        out(L"      ");
        out(paths[i]);
        out(L" : ");
        if (s == EFI_SUCCESS) {
            out(L"PRESENT");
            f->Close(f);
        } else {
            out(L"absent (0x");
            out_hex(s, 16);
            out(L")");
        }
        out_nl();
    }
    root->Close(root);
}

static void dump_sfs(const CHAR16 *title)
{
    EFI_BOOT_SERVICES *bs = g_st->BootServices;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    UINTN count = 0;
    UINTN i;

    out(title);
    out_nl();
    if (bs->LocateHandleBuffer(ByProtocol, &sfs_guid, (void *)0, &count,
                               &handles) != EFI_SUCCESS) {
        out(L"  (none)");
        out_nl();
        return;
    }
    for (i = 0; i < count; i++) {
        dump_handle(handles[i], i);
        probe_files(handles[i]);
    }
    bs->FreePool(handles);
}

static void dump_blockio(const CHAR16 *title)
{
    EFI_BOOT_SERVICES *bs = g_st->BootServices;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    UINTN count = 0;
    UINTN i;

    out(title);
    out_nl();
    if (bs->LocateHandleBuffer(ByProtocol, &bio_guid, (void *)0, &count,
                               &handles) != EFI_SUCCESS) {
        out(L"  (none)");
        out_nl();
        return;
    }
    for (i = 0; i < count; i++)
        dump_handle(handles[i], i);
    bs->FreePool(handles);
}

/* ---- the actual chainload, so the two candidate ESPs can be compared by
 * running them rather than by reading them ----
 *
 * BOOT_TARGET 1 = the El Torito FAT child with the uppercase removable-media
 * path (exactly what falcon-efi/src/iso/iso.c does today).
 * BOOT_TARGET 2 = the UDF child with the lowercase path the UDF namespace
 * actually stores.
 */
#ifndef BOOT_TARGET
#define BOOT_TARGET 0
#endif
#ifndef REAL_CD
#define REAL_CD 0
#endif

#if BOOT_TARGET != 0
static EFI_STATUS append_file(EFI_BOOT_SERVICES *bs,
                              EFI_DEVICE_PATH_PROTOCOL *base,
                              const CHAR16 *name, UINTN name_bytes,
                              EFI_DEVICE_PATH_PROTOCOL **outp)
{
    UINTN end_size = sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN base_len = dp_total_len(base);
    UINTN node_len = end_size + name_bytes;
    UINTN prefix = base_len - end_size;
    UINT8 *buf = (UINT8 *)0;
    EFI_DEVICE_PATH_PROTOCOL *node;
    EFI_STATUS status;
    UINTN i;

    status = bs->AllocatePool(EfiLoaderData, prefix + node_len + end_size,
                              (void **)&buf);
    if (EFI_ERROR(status))
        return status;
    for (i = 0; i < prefix; i++)
        buf[i] = ((const UINT8 *)base)[i];
    node = (EFI_DEVICE_PATH_PROTOCOL *)(void *)(buf + prefix);
    node->Type = MEDIA_DEVICE_PATH;
    node->SubType = MEDIA_FILEPATH_DP;
    dp_set_node_len(node, (UINT16)node_len);
    for (i = 0; i < name_bytes; i++)
        buf[prefix + end_size + i] = ((const UINT8 *)name)[i];
    dp_set_end((EFI_DEVICE_PATH_PROTOCOL *)(void *)(buf + prefix + node_len));
    *outp = (EFI_DEVICE_PATH_PROTOCOL *)(void *)buf;
    return EFI_SUCCESS;
}

/* Which of our two children is which: the UDF child carries a second vendor
 * node, the El Torito child carries a CDROM() node. */
static EFI_HANDLE pick_child(EFI_BOOT_SERVICES *bs, int want_udf)
#if REAL_CD
{
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    UINTN count = 0;
    UINTN i;

    if (bs->LocateHandleBuffer(ByProtocol, &sfs_guid, (void *)0, &count,
                               &handles) != EFI_SUCCESS)
        return (EFI_HANDLE)0;
    for (i = 0; i < count; i++) {
        EFI_DEVICE_PATH_PROTOCOL *dp;
        const EFI_DEVICE_PATH_PROTOCOL *node;
        const EFI_DEVICE_PATH_PROTOCOL *last = (const void *)0;
        const UINT8 *p;

        if (bs->HandleProtocol(handles[i], &dp_guid, (void **)&dp) !=
            EFI_SUCCESS)
            continue;
        p = (const UINT8 *)dp;
        for (;;) {
            node = (const EFI_DEVICE_PATH_PROTOCOL *)(const void *)p;
            if (dp_is_end(node))
                break;
            last = node;
            p += dp_node_len(node);
        }
        if (last == (const void *)0)
            continue;
        if (last->Type != MEDIA_DEVICE_PATH)
            continue;
        if ((want_udf && last->SubType == MEDIA_VENDOR_DP) ||
            (!want_udf && last->SubType == 0x02 /* MEDIA_CDROM_DP */)) {
            EFI_HANDLE hit = handles[i];

            bs->FreePool(handles);
            return hit;
        }
    }
    bs->FreePool(handles);
    return (EFI_HANDLE)0;
}
#else
{
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    UINTN count = 0;
    UINTN i;

    if (bs->LocateHandleBuffer(ByProtocol, &sfs_guid, (void *)0, &count,
                               &handles) != EFI_SUCCESS)
        return (EFI_HANDLE)0;
    for (i = 0; i < count; i++) {
        EFI_DEVICE_PATH_PROTOCOL *dp;
        const UINT8 *p;
        const EFI_DEVICE_PATH_PROTOCOL *second;
        UINTN j;
        int ours = 1;

        if (bs->HandleProtocol(handles[i], &dp_guid, (void **)&dp) !=
            EFI_SUCCESS)
            continue;
        p = (const UINT8 *)dp;
        for (j = 0; j < sizeof(g_dev.path.vendor); j++) {
            if (p[j] != ((const UINT8 *)&g_dev.path.vendor)[j])
                ours = 0;
        }
        if (!ours)
            continue;
        second = (const EFI_DEVICE_PATH_PROTOCOL *)(const void *)
            (p + sizeof(g_dev.path.vendor));
        if ((want_udf && second->SubType == MEDIA_VENDOR_DP) ||
            (!want_udf && second->SubType != MEDIA_VENDOR_DP)) {
            EFI_HANDLE hit = handles[i];

            bs->FreePool(handles);
            return hit;
        }
    }
    bs->FreePool(handles);
    return (EFI_HANDLE)0;
}
#endif /* REAL_CD */

static void do_boot(EFI_HANDLE parent_image)
{
    EFI_BOOT_SERVICES *bs = g_st->BootServices;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
#if BOOT_TARGET == 2
    static const CHAR16 name[] = L"\\efi\\boot\\bootx64.efi";
    EFI_HANDLE esp = pick_child(bs, 1);
#else
    static const CHAR16 name[] = L"\\EFI\\BOOT\\BOOTX64.EFI";
    EFI_HANDLE esp = pick_child(bs, 0);
#endif
    EFI_DEVICE_PATH_PROTOCOL *esp_dp;
    EFI_DEVICE_PATH_PROTOCOL *load_dp;
    EFI_HANDLE child = (EFI_HANDLE)0;
    EFI_STATUS status;

    if (esp == (EFI_HANDLE)0) {
        out(L"BOOT: no candidate ESP\r\n");
        return;
    }
    out(L"BOOT from: ");
    out_path(esp);
    out(L"  file: ");
    out(name);
    out_nl();
    if (bs->HandleProtocol(esp, &dp_guid, (void **)&esp_dp) != EFI_SUCCESS)
        return;
    if (EFI_ERROR(append_file(bs, esp_dp, name, sizeof(name), &load_dp)))
        return;
    status = bs->LoadImage(FALSE, parent_image, load_dp, (void *)0, 0, &child);
    out(L"LoadImage -> 0x");
    out_hex(status, 16);
    out_nl();
    if (EFI_ERROR(status))
        return;
    out(L"StartImage...\r\n");
    status = bs->StartImage(child, (UINTN *)0, (CHAR16 **)0);
    out(L"StartImage returned 0x");
    out_hex(status, 16);
    out_nl();
}
#endif /* BOOT_TARGET */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    EFI_BOOT_SERVICES *bs;
    EFI_GUID d2t_guid = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_BLOCK_IO_PROTOCOL *parent = (EFI_BLOCK_IO_PROTOCOL *)0;
    UINT64 size;
    EFI_STATUS status;

    g_st = st;
    bs = st->BootServices;
    bs->LocateProtocol(&d2t_guid, (void *)0, (void **)&g_d2t);

    out(L"\r\n==== VCD-SPIKE-START ====\r\n");

#if REAL_CD
    out(L"REAL_CD mode: firmware's own CD-ROM, nothing published\r\n");
    dump_sfs(L"---- SimpleFileSystem (real CD) ----");
    do_boot(image_handle);
    out(L"==== VCD-SPIKE-END ====\r\n");
    return EFI_SUCCESS;
#endif
    status = find_iso_parent(bs, &parent);
    if (EFI_ERROR(status)) {
        out(L"ERROR: no ISO-bearing block device found\r\n");
        return status;
    }

    size = ((UINT64)parent->Media->LastBlock + 1) * parent->Media->BlockSize;
    size -= size % ISO_CD_BLOCK_SIZE;

    device_init(&g_dev, size);
    g_dev.parent = parent;
    g_dev.base = 0;

    out(L"publishing virtual CD: bsize=2048 lastblock=");
    out_dec(g_dev.media.LastBlock);
    out_nl();

    dump_sfs(L"---- SimpleFileSystem BEFORE publish ----");

    status = bs->InstallMultipleProtocolInterfaces(
        &g_dev.handle, &bio_guid, &g_dev.bio, &dp_guid, &g_dev.path,
        (void *)0);
    if (EFI_ERROR(status)) {
        out(L"ERROR: InstallMultipleProtocolInterfaces 0x");
        out_hex(status, 16);
        out_nl();
        return status;
    }

    status = bs->ConnectController(g_dev.handle, (EFI_HANDLE *)0,
                                   (EFI_DEVICE_PATH_PROTOCOL *)0, TRUE);
    out(L"ConnectController(recursive) -> 0x");
    out_hex(status, 16);
    out_nl();

    out(L"---- our own handle after connect ----\r\n");
    dump_handle(g_dev.handle, 0);

    dump_blockio(L"---- BlockIo AFTER publish ----");
    dump_sfs(L"---- SimpleFileSystem AFTER publish ----");

#if BOOT_TARGET != 0
    do_boot(image_handle);
#endif
    out(L"==== VCD-SPIKE-END ====\r\n");
    return EFI_SUCCESS;
}
