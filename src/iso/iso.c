/* Virtual CD over an exFAT-backed ISO file, plus the chainload of the ESP
 * the firmware finds inside it. See src/iso/iso.h for the contract and
 * docs/DESIGN.md Section 6 for why the firmware, not us, does the parsing.
 */

#include <iso/iso.h>
#include <efi/blockio.h>
#include <efi/devpath.h>
#include <efi/sfs.h>
#include <efi/shimguid.h>
#include <family/family.h>
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
    fs_file               file;
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
    avail = fs_file_size(&dev->file) - off;
    if (avail > (uint64_t)size)
        avail = (uint64_t)size;
    if (fs_file_read(&dev->file, off, avail, buffer) != FS_OK)
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

static void device_init(iso_device *dev, uint64_t size)
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
    dev->media.LastBlock = size / ISO_BLOCK_SIZE - 1;

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

/* ---- looking inside the ESP: family and signature presence ---- */

/* Fingerprint paths are short by construction (family.c's longest is a
 * loader entry filename); a path that does not fit is a fingerprint that
 * does not match, which is the same answer as "absent". */
#define PROBE_PATH_MAX 128u

/* Enough for a PE's DOS stub, COFF header, optional header and the whole
 * data directory array. Anything with a further e_lfanew is not a PE we can
 * read with a single short read, and we say so rather than guess. */
#define PE_HEAD_MAX 1024u

/* Offsets inside the optional header, which is where PE32 and PE32+ differ:
 * the 64-bit form widens three fields, pushing NumberOfRvaAndSizes and the
 * directory array 16 bytes later (PE/COFF spec 3.4). */
#define OPT_MAGIC_PE32   0x10bu
#define OPT_MAGIC_PE32P  0x20bu
#define DIR_SECURITY     4u     /* certificate table (Authenticode) */

/* The probe's ctx is the ESP's root directory handle itself; there is no
 * per-probe state worth a wrapper struct. */
static EFI_FILE_PROTOCOL *probe_root(void *ctx)
{
    return (EFI_FILE_PROTOCOL *)ctx;
}

/* "/EFI/BOOT/BOOTX64.EFI" -> L"\EFI\BOOT\BOOTX64.EFI". The File Protocol
 * wants backslashes and CHAR16; family.c speaks ASCII with forward slashes so
 * the identical fingerprints run against a host directory tree in a test. */
static int path_to_ucs2(const char *path, CHAR16 *out, UINTN cap)
{
    UINTN i = 0;

    if (path == (const char *)0)
        return -1;
    while (path[i] != '\0') {
        unsigned char c = (unsigned char)path[i];

        if (i + 1 >= cap || c >= 0x80)
            return -1;
        out[i] = (c == '/') ? L'\\' : (CHAR16)c;
        i++;
    }
    out[i] = 0;
    return 0;
}

static EFI_STATUS probe_open(void *ctx, const char *path,
                             EFI_FILE_PROTOCOL **file)
{
    EFI_FILE_PROTOCOL *root = probe_root(ctx);
    CHAR16 wide[PROBE_PATH_MAX];

    if (root == (EFI_FILE_PROTOCOL *)0)
        return EFI_INVALID_PARAMETER;
    if (path_to_ucs2(path, wide, PROBE_PATH_MAX) != 0)
        return EFI_INVALID_PARAMETER;
    return root->Open(root, file, wide, EFI_FILE_MODE_READ, 0);
}

static int probe_exists(void *ctx, const char *path)
{
    EFI_FILE_PROTOCOL *file;

    if (probe_open(ctx, path, &file) != EFI_SUCCESS)
        return 0;
    file->Close(file);
    return 1;
}

/* Bounded read at an arbitrary offset. The File Protocol has no positioned
 * read, so an offset is a SetPosition ahead of the Read; the handle is opened
 * and closed per call, so the seek cannot leak into anyone else's position.
 *
 * A seek past the end is not an error here: the spec allows it and the Read
 * that follows simply delivers nothing, which is the "short only at end of
 * file" the family_probe contract already describes. Every fingerprint
 * family.c has, and the PE header read below, still start at zero; the offset
 * is honoured so that a later one does not have to be written around. */
static int probe_read(void *ctx, const char *path, uint64_t off, void *buf,
                      size_t len, size_t *got)
{
    EFI_FILE_PROTOCOL *file;
    UINTN size = (UINTN)len;
    EFI_STATUS status;

    *got = 0;
    if (probe_open(ctx, path, &file) != EFI_SUCCESS)
        return -1;
    if (off != 0 && EFI_ERROR(file->SetPosition(file, off))) {
        file->Close(file);
        return -1;
    }
    status = file->Read(file, &size, buf);
    file->Close(file);
    if (EFI_ERROR(status))
        return -1;
    *got = (size_t)size;
    return 0;
}

static EFI_STATUS esp_open_root(EFI_BOOT_SERVICES *bs, EFI_HANDLE esp,
                                EFI_FILE_PROTOCOL **root)
{
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
    EFI_STATUS status;

    *root = (EFI_FILE_PROTOCOL *)0;
    status = bs->HandleProtocol(esp, &sfs_guid, (void **)&sfs);
    if (EFI_ERROR(status))
        return status;
    return sfs->OpenVolume(sfs, root);
}

static const family_desc *root_family(EFI_FILE_PROTOCOL *root)
{
    family_probe probe;

    probe.exists = probe_exists;
    probe.read = probe_read;
    probe.ctx = root;
    return family_detect(&probe);
}

/* Does the ISO's default loader carry a certificate table?
 *
 * Read the header, look at data directory 4, report whether its size field is
 * nonzero. That is the entire check, and it is deliberately the entire check:
 * a nonzero size says bytes claiming to be a signature are attached, nothing
 * about whose key signed them or whether the platform will accept them. The
 * answer feeds a menu label and nothing else. */
static iso_sig root_signature(EFI_FILE_PROTOCOL *root)
{
    uint8_t head[PE_HEAD_MAX];
    size_t got = 0;
    size_t pe;
    size_t opt;
    size_t nrva_off;
    size_t dir_off;
    uint16_t magic;
    uint32_t nrva;

    if (probe_read(root, "/EFI/BOOT/BOOTX64.EFI", 0, head, sizeof(head),
                   &got) != 0)
        return ISO_SIG_NO_EFI;

    /* From here on the file exists but does not parse, which is a different
     * fact from "no loader" and gets a different label. */
    if (got < 0x40u || head[0] != 'M' || head[1] != 'Z')
        return ISO_SIG_UNKNOWN;
    pe = (size_t)get_le32(head + 0x3c);
    if (pe > got || got - pe < 24u + 2u)
        return ISO_SIG_UNKNOWN;
    if (memcmp(head + pe, "PE\0\0", 4) != 0)
        return ISO_SIG_UNKNOWN;

    opt = pe + 24u; /* signature (4) + COFF file header (20) */
    magic = get_le16(head + opt);
    if (magic == OPT_MAGIC_PE32P) {
        nrva_off = 108u;
        dir_off = 112u;
    } else if (magic == OPT_MAGIC_PE32) {
        nrva_off = 92u;
        dir_off = 96u;
    } else {
        return ISO_SIG_UNKNOWN;
    }

    if (got - opt < nrva_off + 4u)
        return ISO_SIG_UNKNOWN;
    nrva = get_le32(head + opt + nrva_off);
    /* An image with a short directory array simply has no certificate table
     * entry, which is an answer, not a parse failure. */
    if (nrva <= DIR_SECURITY)
        return ISO_SIG_ABSENT;
    if (got - opt < dir_off + (DIR_SECURITY + 1u) * 8u)
        return ISO_SIG_UNKNOWN;

    return get_le32(head + opt + dir_off + DIR_SECURITY * 8u + 4u) != 0
               ? ISO_SIG_PRESENT
               : ISO_SIG_ABSENT;
}

/* ---- chainload ---- */

/* The load path of DESIGN Section 7.6, resolved per boot attempt.
 *
 * Preferring SHIM_IMAGE_LOADER over gBS->LoadImage is not about capability:
 * the spike measured the two to be the same function on shim 16.x under
 * Secure Boot, because shim patches the boot-services table in place. It is
 * about intent and durability. Calling the protocol says which loader we
 * meant, it keeps working if a future shim stops patching the table, and it
 * is the interface upstream maintains.
 *
 * The fallback is not a weakening. With Secure Boot off there is nothing to
 * verify and the firmware's LoadImage is the honest loader; with Secure Boot
 * on and no shim in the chain the firmware's LoadImage is the enforcing one.
 * Either way the decision is the platform's, never ours. */
typedef struct {
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN, EFI_HANDLE,
                                   EFI_DEVICE_PATH_PROTOCOL *, void *, UINTN,
                                   EFI_HANDLE *);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE, UINTN *, CHAR16 **);
    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE);
    /* Which of the two the members came from; the menu's failure line names
     * it, which is the only way an operator can tell the paths apart. */
    BOOLEAN via_shim;
} load_ops;

/* Resolved on every attempt rather than cached at startup: the loader has no
 * startup hook of its own to hang it off, and one LocateProtocol per boot is
 * not worth a cache that could go stale. */
static void resolve_load_ops(EFI_BOOT_SERVICES *bs, load_ops *ops)
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
    load_ops ops;
    EFI_STATUS status;

    resolve_load_ops(bs, &ops);

    result->stage = L"reading the ISO ESP's device path";
    status = bs->HandleProtocol(esp_handle, &dp_guid, (void **)&esp_path);
    if (EFI_ERROR(status))
        return status;

    result->stage = L"building the payload's device path";
    status = build_load_path(bs, esp_path, &load_path);
    if (EFI_ERROR(status))
        return status;

    /* SourceBuffer stays NULL on both paths: see build_load_path above for
     * why the image is always named rather than handed over as bytes. */
    result->stage = ops.via_shim
                        ? L"LoadImage of \\EFI\\BOOT\\BOOTX64.EFI (shim)"
                        : L"LoadImage of \\EFI\\BOOT\\BOOTX64.EFI (firmware)";
    status = ops.LoadImage(FALSE, parent_image, load_path, (void *)0, 0,
                           &child);
    bs->FreePool(load_path);
    if (EFI_ERROR(status)) {
        /* A refusal under Secure Boot is the one failure that still leaves a
         * loaded image behind (spec 7.4): the platform read and relocated it,
         * then declined to run it. Unloading is the caller's job, and not
         * doing it leaks the image on every refused entry. It has to be the
         * unloader that matches the loader: shim tracks the images it loaded
         * itself, so handing one of them to the firmware's UnloadImage would
         * leave shim's bookkeeping behind. */
        if (status == EFI_SECURITY_VIOLATION && child != (EFI_HANDLE)0)
            ops.UnloadImage(child);
        return status;
    }

    result->stage = L"StartImage";
    status = ops.StartImage(child, &exit_size, &exit_data);
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

/* ---- entry points ---- */

/* Everything up to and including "the firmware has mounted the ISO's ESP".
 * Booting and surveying need exactly this much and differ only in what they
 * do with the handle, so the sequence lives in one place.
 *
 * *owned says whether the caller now holds the virtual device and must call
 * device_teardown. It stays zero only when a previous device is still held by
 * a driver: that instance belongs to the earlier attempt, and tearing it down
 * from here would disconnect drivers this caller never connected. */
static EFI_STATUS iso_open(EFI_SYSTEM_TABLE *st, fs_vol *vol,
                           const fs_dirent *file, const CHAR16 **stage,
                           EFI_HANDLE *esp_handle, UINT8 *owned)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_HANDLE *before = (EFI_HANDLE *)0;
    UINTN before_n = 0;
    uint64_t size;
    EFI_STATUS status;

    *esp_handle = (EFI_HANDLE)0;
    *owned = 0;
    *stage = L"preparing the virtual CD";

    if (g_dev.in_use) {
        *stage = L"previous virtual CD still held by a driver";
        return EFI_ACCESS_DENIED;
    }
    if (fs_file_open(vol, &g_dev.file, file) != FS_OK) {
        *stage = L"opening the ISO on its volume";
        return EFI_DEVICE_ERROR;
    }
    /* The open comes first because the size does: a directory entry carries
     * one on exFAT and not on every filesystem (ext4 keeps it in the inode),
     * so the byte count this device advertises is the opened file's. */
    size = fs_file_size(&g_dev.file);
    if (size < ISO_BLOCK_SIZE) {
        *stage = L"ISO is too small to be CD media";
        return EFI_UNSUPPORTED;
    }

    device_init(&g_dev, size);
    *owned = 1;

    *stage = L"publishing the virtual CD";
    status = bs->InstallMultipleProtocolInterfaces(
        &g_dev.handle,
        &bio_guid, &g_dev.bio,
        &dp_guid, &g_dev.path,
        (void *)0);
    if (EFI_ERROR(status))
        return status;
    g_dev.in_use = 1;

    /* Snapshot before connecting: the ISO's ESP is the filesystem that is
     * not here yet. */
    *stage = L"enumerating filesystems";
    status = sfs_handles(bs, &before, &before_n);
    if (EFI_ERROR(status))
        return status;

    /* The firmware's own partition driver parses El Torito and its FAT
     * driver mounts the embedded ESP; we write no ISO9660 reader for this
     * (DESIGN Section 6 stage 3). */
    *stage = L"ConnectController on the virtual CD";
    /* Set before the call: a partial bind still leaves drivers attached, and
     * teardown must disconnect them. */
    g_dev.connected = 1;
    status = bs->ConnectController(g_dev.handle, (EFI_HANDLE *)0,
                                   (EFI_DEVICE_PATH_PROTOCOL *)0, TRUE);
    if (!EFI_ERROR(status)) {
        *stage = L"finding the ISO's EFI System Partition";
        status = find_iso_esp(bs, &g_dev, before, before_n, esp_handle);
    }

    if (before != (EFI_HANDLE *)0)
        bs->FreePool(before);
    return status;
}

EFI_STATUS iso_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                    fs_vol *vol, const fs_dirent *file,
                    iso_boot_result *result)
{
    EFI_BOOT_SERVICES *bs;
    EFI_HANDLE esp_handle = (EFI_HANDLE)0;
    UINT8 owned = 0;
    EFI_STATUS status;
    EFI_STATUS teardown;

    if (st == (void *)0 || vol == (void *)0 || file == (void *)0 ||
        result == (void *)0)
        return EFI_INVALID_PARAMETER;

    bs = st->BootServices;
    result->status = EFI_SUCCESS;
    result->family = (const family_desc *)0;

    status = iso_open(st, vol, file, &result->stage, &esp_handle, &owned);
    if (!EFI_ERROR(status)) {
        EFI_FILE_PROTOCOL *root;

        /* Detection is reported, never acted on. It runs before the chainload
         * only so the answer exists while the ESP is still mounted; the load
         * path below is byte-for-byte what it was without it, and no cmdline
         * template has a consumer yet (DESIGN Section 6.2). */
        if (esp_open_root(bs, esp_handle, &root) == EFI_SUCCESS) {
            result->family = root_family(root);
            root->Close(root);
        }
        status = chainload(st, parent_image, esp_handle, result);
    }
    if (EFI_ERROR(status))
        result->status = status;

    if (!owned)
        return status;

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

EFI_STATUS iso_survey_media(EFI_SYSTEM_TABLE *st, fs_vol *vol,
                            const fs_dirent *file, iso_survey *out)
{
    EFI_BOOT_SERVICES *bs;
    EFI_HANDLE esp_handle = (EFI_HANDLE)0;
    EFI_FILE_PROTOCOL *root;
    const CHAR16 *stage;
    UINT8 owned = 0;
    EFI_STATUS status;
    EFI_STATUS teardown;

    if (out == (void *)0)
        return EFI_INVALID_PARAMETER;
    out->family = family_by_id(FAMILY_UNKNOWN);
    out->sig = ISO_SIG_UNKNOWN;
    if (st == (void *)0 || vol == (void *)0 || file == (void *)0)
        return EFI_INVALID_PARAMETER;

    bs = st->BootServices;
    status = iso_open(st, vol, file, &stage, &esp_handle, &owned);
    if (EFI_ERROR(status)) {
        /* A medium the firmware found nothing bootable in: no El Torito
         * catalog, or a catalog pointing at no ESP. Not an error worth
         * showing, just an entry we cannot say anything about. */
        if (status == EFI_NOT_FOUND)
            out->sig = ISO_SIG_NO_EFI;
    } else if (esp_open_root(bs, esp_handle, &root) == EFI_SUCCESS) {
        out->family = root_family(root);
        out->sig = root_signature(root);
        root->Close(root);
    }

    if (owned) {
        teardown = device_teardown(bs, &g_dev);
        if (EFI_ERROR(teardown))
            status = teardown;
    }
    return status;
}
