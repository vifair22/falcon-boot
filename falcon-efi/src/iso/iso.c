/* Virtual media over an exFAT-backed image file, plus the chainload of the ESP
 * the firmware finds inside it. See falcon-efi/src/iso/iso.h for the contract
 * and docs/DESIGN.md Section 6 for why the firmware, not us, does the parsing.
 *
 * One publisher, two flavors (DESIGN Section 6 stage 2). The flavor is two
 * numbers - the advertised block size and the file-size rule that follows
 * from it - and nothing else: the device path node, the driver connect, the
 * ESP hunt, the family/signature survey, the chainload and the teardown are
 * one implementation shared by both. A .iso is published with 2048-byte
 * blocks so the firmware's partition driver looks for an El Torito catalog;
 * a .img is published with 512-byte sectors so the same driver's GPT/MBR path
 * runs instead, or so the FAT driver binds the whole device for a
 * partition-table-less superfloppy.
 */

#include <iso/iso.h>
#include <iso/overlay.h>
#include <efi/arch.h>
#include <efi/blockio.h>
#include <efi/devpath.h>
#include <efi/sfs.h>
#include <loader/loadops.h>
#include <efi/shimguid.h>
#include <media/family.h>
#include <media/bootfs.h>
#include <media/medium.h>
#include <lib/string.h>
#include <tui/tui.h>
#include <pe/pe.h>
#include <trust/probe.h>

/* El Torito lives on 2048-byte media; the firmware's partition driver only
 * looks for a boot catalog when the block size says CD-ROM. */
#define ISO_CD_BLOCK_SIZE 2048u

/* The sector size every GPT/MBR and FAT structure on a disk image is laid out
 * against. Advertising anything else here would misalign the firmware's whole
 * reading of the file. */
#define ISO_DISK_BLOCK_SIZE 512u

/* Identifies our synthetic media in device paths. A vendor-defined node is
 * the spec's answer for a device with no real bus behind it, and it is the
 * same node for both flavors: firmware partition and filesystem drivers bind
 * on the Block I/O instance and what its media says, never on the shape of
 * the path above it.
 *
 * It is a HARDWARE vendor node (VenHw), not a media one. A payload that finds
 * its CD drive by cutting its own path back to the hardware part, the way a
 * real CD's PciRoot/Pci/Sata/CDROM becomes PciRoot/Pci/Sata, is left with
 * nothing under a media root: VMware ESXi's loader does exactly that and
 * fails with "get_boot_device: Not found". */
#define FALCON_ISO_MEDIA_GUID \
    { 0x887579f6, 0x4287, 0x4bc4, \
      { 0x99, 0x92, 0x15, 0xa3, 0xb5, 0x61, 0xce, 0x63 } }

/* Removable-media default path: every El Torito ESP that expects to be
 * booted by firmware puts its loader here (UEFI spec 3.5.1.1). */
static const CHAR16 boot_file_path[] = L"\\EFI\\BOOT\\BOOT" EFI_ARCH_UP ".EFI";

/* The same path in the ASCII, forward-slash form the probe helpers take. The
 * two spellings have to stay identical character for character, case
 * included: one is the string LoadImage is handed and the other is what
 * decides which volume gets handed it. */
#define ISO_BOOT_PATH "/EFI/BOOT/BOOT" EFI_ARCH_UP ".EFI"

/* How many of our own volumes are kept. A medium publishes one (a plain ESP)
 * or two (an El Torito FAT boot image plus the ISO's own filesystem); the
 * bound is what keeps a firmware that enumerates something unexpected costing
 * a fixed amount of work per menu entry rather than an unbounded amount. */
#define ISO_MAX_VOLUMES 4u

typedef struct {
    FALCON_VENDOR_DEVICE_PATH vendor;
    EFI_DEVICE_PATH_PROTOCOL  end;
} iso_device_path;

/* Device path nodes are byte-packed; a compiler that padded this struct
 * would publish a path the firmware cannot walk. */
_Static_assert(sizeof(iso_device_path) ==
                   sizeof(FALCON_VENDOR_DEVICE_PATH) +
                       sizeof(EFI_DEVICE_PATH_PROTOCOL),
               "device path nodes must pack without padding");

typedef struct {
    /* First member on purpose: the interface pointer the firmware hands back
     * to our callbacks is the address of the instance. */
    EFI_BLOCK_IO_PROTOCOL bio;
    EFI_BLOCK_IO_MEDIA    media;
    iso_device_path       path;
    fs_file               file;
    /* The two sizes, kept apart on purpose (falcon-efi/src/iso/overlay.h).
     * image_size is how far the file on p2 runs; virt_size is how far the
     * MEDIUM runs, which an append region makes larger and block truncation can
     * make smaller. Every bounds decision in the read path is against
     * virt_size, and image_size is only ever asked "how many of these bytes
     * physically exist". */
    uint64_t              image_size;
    uint64_t              virt_size;
    /* The modified view, copied by value so no caller-owned array has to
     * outlive the boot. The fs_file objects it points at still do; see the
     * lifetime note in overlay.h. Cleared by device_init and by teardown, so
     * a set can never survive into a different entry's boot. */
    iso_overlay           ovl;
    EFI_HANDLE            handle;
    UINT8                 in_use;
    UINT8                 connected;
} iso_device;

/* The filesystems the firmware mounted inside one medium. Index 0 is the one
 * to boot; the rest exist because the bootable volume is not always the one
 * that says what the medium is. */
typedef struct {
    EFI_HANDLE vol[ISO_MAX_VOLUMES];
    UINTN      n;
} iso_volumes;

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

/* Serves len bytes of the VIRTUAL medium at off from the image file itself.
 *
 * A CD image whose length is not a whole number of 2048-byte blocks still
 * advertises that last partial block, so serve the bytes that exist and zero
 * the rest rather than failing the read. A disk image cannot get here with a
 * partial tail: iso_open refuses one outright, because a sector the firmware
 * half-reads is a partition table it half-believes.
 *
 * The clamp is written as a comparison rather than the subtraction it used to
 * be for a reason that only appeared with the append region: off is a virtual
 * offset and can legitimately sit PAST the end of the image file, at which
 * point "image_size - off" underflows into an enormous avail and the read runs
 * off the end of the caller's buffer. Past the image, every byte is a zero
 * this device is inventing, which is the same answer the short tail gets. */
static EFI_STATUS read_image(iso_device *dev, uint64_t off, uint64_t len,
                             UINT8 *dst)
{
    uint64_t avail = (off < dev->image_size) ? dev->image_size - off : 0;

    if (avail > len)
        avail = len;
    if (avail != 0 && fs_file_read(&dev->file, off, avail, dst) != FS_OK)
        return EFI_DEVICE_ERROR;
    if (avail < len)
        memset(dst + avail, 0, (size_t)(len - avail));
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI iso_read_blocks(EFI_BLOCK_IO_PROTOCOL *self,
                                         UINT32 media_id, EFI_LBA lba,
                                         UINTN size, void *buffer)
{
    iso_device *dev = device_of(self);
    UINTN bs = dev->media.BlockSize;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t nseg = 0;
    size_t i;
    uint64_t off;

    if (media_id != dev->media.MediaId)
        return EFI_MEDIA_CHANGED;
    if (size == 0)
        return EFI_SUCCESS;
    if (buffer == (void *)0)
        return EFI_INVALID_PARAMETER;
    if (size % bs != 0)
        return EFI_BAD_BUFFER_SIZE;
    if (lba > dev->media.LastBlock)
        return EFI_INVALID_PARAMETER;

    off = (uint64_t)lba * bs;
    if (size / bs - 1 > dev->media.LastBlock - lba)
        return EFI_INVALID_PARAMETER;

    /* The overwhelmingly common case is a medium with no modified view at all,
     * and it must cost exactly what it always did: one call, no split, no
     * segment array touched. */
    if (dev->ovl.n == 0)
        return read_image(dev, off, (uint64_t)size, (UINT8 *)buffer);

    /* Otherwise cut the request at every override boundary it crosses. A
     * single block is routinely part image and part sidecar - overrides are
     * byte ranges and blocks are 2048 bytes, and nothing aligns the two - so
     * the split is what keeps a straddling read from being served entirely
     * from the wrong source. ISO_MAX_SEGMENTS is the proven worst case, so
     * NOSPACE here would be a bug in the bound rather than in the data. */
    if (iso_overlay_split(&dev->ovl, dev->virt_size, off, (uint64_t)size,
                          seg, ISO_MAX_SEGMENTS, &nseg) != ISO_OVL_OK)
        return EFI_INVALID_PARAMETER;

    for (i = 0; i < nseg; i++) {
        UINT8 *dst = (UINT8 *)buffer + (seg[i].virt_off - off);

        if (seg[i].mem != (const uint8_t *)0) {
            memcpy(dst, seg[i].mem + seg[i].src_off, (UINTN)seg[i].len);
        } else if (seg[i].src == (fs_file *)0) {
            EFI_STATUS status = read_image(dev, seg[i].src_off, seg[i].len,
                                           dst);

            if (EFI_ERROR(status))
                return status;
        } else if (fs_file_read(seg[i].src, seg[i].src_off, seg[i].len,
                                dst) != FS_OK) {
            /* No zero fill on this side: iso_overlay_check proved at install
             * time that the sidecar holds every byte this entry promises, so a
             * short read here is a real device error and is reported as one. */
            return EFI_DEVICE_ERROR;
        }
    }
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

/* image_size is the file on p2; virt_size is the medium, already truncated to
 * whole blocks by iso_overlay_media_size. ovl may be NULL, which is the
 * ordinary unmodified medium and is what iso_survey_media always passes. */
static void device_init(iso_device *dev, uint64_t image_size,
                        uint64_t virt_size, UINT32 block_size,
                        const iso_overlay *ovl)
{
    EFI_GUID vendor_guid = FALCON_ISO_MEDIA_GUID;
    UINT64 instance;

    dev->image_size = image_size;
    dev->virt_size = virt_size;
    /* Never left over from the previous entry: a stale src pointer here would
     * be a different ISO's sidecar served into this one's address space. */
    memset(&dev->ovl, 0, sizeof(dev->ovl));
    if (ovl != (const iso_overlay *)0)
        memcpy(&dev->ovl, ovl, sizeof(dev->ovl));

    dev->media.MediaId = g_next_media_id++;
    /* True for both flavors and honest for both: the file can be deleted or
     * replaced between boots exactly as a removable medium can be swapped.
     * It is also what keeps the firmware's FAT driver willing to bind the
     * whole device for a superfloppy image. */
    dev->media.RemovableMedia = TRUE;
    dev->media.MediaPresent = TRUE;
    dev->media.LogicalPartition = FALSE;
    dev->media.ReadOnly = TRUE;
    dev->media.WriteCaching = FALSE;
    /* The one field that carries the flavor: 2048 says "optical, go look for
     * an El Torito catalog", 512 says "disk, go read a partition table". */
    dev->media.BlockSize = block_size;
    dev->media.IoAlign = 0;
    dev->media.LastBlock = virt_size / block_size - 1;

    dev->bio.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
    dev->bio.Media = &dev->media;
    dev->bio.Reset = iso_reset;
    dev->bio.ReadBlocks = iso_read_blocks;
    dev->bio.WriteBlocks = iso_write_blocks;
    dev->bio.FlushBlocks = iso_flush_blocks;

    dev->path.vendor.vendor.Header.Type = HARDWARE_DEVICE_PATH;
    dev->path.vendor.vendor.Header.SubType = HW_VENDOR_DP;
    dp_set_node_len(&dev->path.vendor.vendor.Header,
                    (UINT16)sizeof(dev->path.vendor));
    dev->path.vendor.vendor.Guid = vendor_guid;
    instance = (UINT64)(UINTN)dev;
    memcpy(dev->path.vendor.instance, &instance, sizeof(instance));
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
         * ISO: leaving in_use set is what refuses that. The override set stays
         * too, for the same reason - the drivers still reading this device
         * must keep seeing the view they were connected against. */
        return status;
    }

    /* The last reader is gone, so the modified view goes with it. device_init
     * would clear this anyway; doing it here as well means the window in which
     * a released device still holds pointers to another entry's sidecars does
     * not exist at all. */
    memset(&dev->ovl, 0, sizeof(dev->ovl));
    dev->image_size = 0;
    dev->virt_size = 0;
    dev->handle = (EFI_HANDLE)0;
    dev->in_use = 0;
    return EFI_SUCCESS;
}

/* ---- finding the filesystems the firmware mounted inside the ISO ---- */

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

/* Does this handle's device path start with our virtual media's vendor node?
 *
 * "Starts with" covers both flavors and both binding shapes: a partition
 * child carries our vendor node plus its own, and a superfloppy image whose
 * FAT driver bound straight to our Block I/O handle carries our vendor node
 * and nothing but the end node after it. Both compare equal over the prefix,
 * which is all this asks. */
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

/* The medium's filesystems are whichever Simple File System handles appeared
 * while we were connecting drivers. Comparing handle sets is firmer than
 * matching device paths alone, because it does not depend on how the
 * firmware's partition driver chooses to name its children; the device path is
 * then used only to tell our own children from anyone else's.
 *
 * It is also what makes the superfloppy case work for free: a FAT driver that
 * binds to our own handle rather than to a partition child installs the new
 * Simple File System interface on a handle that was not in the before-set
 * either, so the diff finds it the same way.
 *
 * More than one is the normal case, not an oddity: a CD image with an El
 * Torito catalog yields a FAT boot image and the ISO's own filesystem as two
 * separate children. Which of them boots is decided in select_boot_volume,
 * which needs to read files and so lives further down. */
static EFI_STATUS collect_iso_volumes(EFI_BOOT_SERVICES *bs, iso_device *dev,
                                      EFI_HANDLE *before, UINTN before_n,
                                      iso_volumes *out)
{
    EFI_HANDLE *after = (EFI_HANDLE *)0;
    EFI_HANDLE first_new = (EFI_HANDLE)0;
    UINTN after_n = 0;
    UINTN i;
    EFI_STATUS status;

    out->n = 0;
    status = sfs_handles(bs, &after, &after_n);
    if (EFI_ERROR(status))
        return status;

    for (i = 0; i < after_n; i++) {
        if (handle_in(after[i], before, before_n))
            continue;
        if (first_new == (EFI_HANDLE)0)
            first_new = after[i];
        if (out->n < ISO_MAX_VOLUMES && is_our_child(bs, dev, after[i]))
            out->vol[out->n++] = after[i];
    }

    if (after != (EFI_HANDLE *)0)
        bs->FreePool(after);

    if (out->n != 0)
        return EFI_SUCCESS;
    /* Nothing carried our vendor node, but something did appear: a firmware
     * whose children we cannot recognise is still better served by the one
     * new filesystem than by no filesystem at all. */
    if (first_new == (EFI_HANDLE)0)
        return EFI_NOT_FOUND;
    out->vol[0] = first_new;
    out->n = 1;
    return EFI_SUCCESS;
}

/* ---- looking inside the ESP: family and signature presence ---- */

/* Fingerprint paths are short by construction (family.c's longest is a
 * loader entry filename); a path that does not fit is a fingerprint that
 * does not match, which is the same answer as "absent". */
#define PROBE_PATH_MAX 128u

/* Enough for a PE's DOS stub, COFF header, optional header and the whole
 * data directory array. A page, not a kilobyte: memtest86+ keeps its x86
 * boot-protocol setup code in front of the PE header, whose e_lfanew is
 * 0x471. Anything with a further e_lfanew is not a PE we can read with a
 * single short read, and we say so rather than guess. */
#define PE_HEAD_MAX 4096u

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

/* Folds an ASCII path to lower case. Returns 0 when the result is worth
 * trying, nonzero when there is nothing to retry: already lower case, or too
 * long for the buffer. */
static int path_fold_lower(const char *path, char *out, size_t cap)
{
    size_t i = 0;
    int changed = 0;

    while (path[i] != '\0') {
        char c = path[i];

        if (i + 1 >= cap)
            return -1;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + ('a' - 'A'));
            changed = 1;
        }
        out[i] = c;
        i++;
    }
    out[i] = '\0';
    return changed ? 0 : -1;
}

/* Fingerprint open: the path as written, then the same path folded to lower
 * case.
 *
 * The constraint is that a File Protocol provider is not required to match
 * case-insensitively and EDK2's UDF driver does not. Measured on OVMF against
 * Windows install media: \EFI\BOOT\BOOTX64.EFI on the UDF volume is
 * EFI_NOT_FOUND while \efi\boot\bootx64.efi opens. The fingerprint table is
 * written in the case each family's own documentation uses, so without the
 * retry a mixed-case fingerprint misses a file that is there.
 *
 * The boot path is deliberately not opened this way. LoadImage is handed one
 * exact string, and a probe more forgiving than the loader would call a volume
 * bootable that the platform then refuses to boot. */
static EFI_STATUS probe_open_ci(void *ctx, const char *path,
                                EFI_FILE_PROTOCOL **file)
{
    char folded[PROBE_PATH_MAX];
    EFI_STATUS status = probe_open(ctx, path, file);

    if (!EFI_ERROR(status))
        return status;
    if (path_fold_lower(path, folded, sizeof(folded)) != 0)
        return status;
    return probe_open(ctx, folded, file);
}

static int probe_exists(void *ctx, const char *path)
{
    EFI_FILE_PROTOCOL *file;

    if (probe_open(ctx, path, &file) != EFI_SUCCESS)
        return 0;
    file->Close(file);
    return 1;
}

static int probe_exists_ci(void *ctx, const char *path)
{
    EFI_FILE_PROTOCOL *file;

    if (probe_open_ci(ctx, path, &file) != EFI_SUCCESS)
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
static int file_read_at(EFI_FILE_PROTOCOL *file, uint64_t off, void *buf,
                        size_t len, size_t *got)
{
    UINTN size = (UINTN)len;
    EFI_STATUS status;

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

static int probe_read_ci(void *ctx, const char *path, uint64_t off, void *buf,
                         size_t len, size_t *got)
{
    EFI_FILE_PROTOCOL *file;

    *got = 0;
    if (probe_open_ci(ctx, path, &file) != EFI_SUCCESS)
        return -1;
    return file_read_at(file, off, buf, len, got);
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

    probe.exists = probe_exists_ci;
    probe.read = probe_read_ci;
    probe.ctx = root;
    return family_detect(&probe);
}

/* Promotes the volume that holds the path we are about to load to index 0.
 *
 * Enumeration order is not an answer to "which of these boots". Windows
 * install media publishes two volumes: the El Torito FAT boot image, whose
 * \EFI\BOOT\BOOTX64.EFI is cdboot.efi and is the one that works, and the UDF
 * volume, whose \efi\boot\bootx64.efi is bootmgfw.efi and was measured to
 * fail LoadImage with EFI_LOAD_ERROR from a real CD-ROM as well as from ours.
 * The order the firmware happens to return them in must not decide that.
 *
 * The test is the exact boot path, not a case-folded one, for the reason
 * probe_open_ci states: a volume that cannot serve the string LoadImage will
 * be given is not the volume to boot. */
static void select_boot_volume(EFI_BOOT_SERVICES *bs, iso_volumes *vols)
{
    UINTN i;

    /* One candidate is not a choice, and the menu pays for this per entry. */
    if (vols->n < 2)
        return;

    for (i = 0; i < vols->n; i++) {
        EFI_FILE_PROTOCOL *root;
        int found;

        if (esp_open_root(bs, vols->vol[i], &root) != EFI_SUCCESS)
            continue;
        found = probe_exists(root, ISO_BOOT_PATH);
        root->Close(root);
        if (found) {
            EFI_HANDLE pick = vols->vol[i];

            vols->vol[i] = vols->vol[0];
            vols->vol[0] = pick;
            return;
        }
    }
}

/* Fingerprints the medium across its volumes, starting at index first.
 *
 * One volume is not enough on Microsoft's install media: oscdimg leaves the
 * ISO 9660 namespace holding a single stub README.TXT and puts every real
 * file, /sources/install.wim included, in the UDF namespace only, while the
 * El Torito boot image is a 1.4 MB FAT volume carrying nothing but the
 * loader. Fingerprinting the bootable volume alone can only ever answer
 * "unknown" there.
 *
 * first exists so a caller that has already fingerprinted the boot volume
 * does not pay for it twice, which is what keeps the menu's cost unchanged
 * for every medium that classifies on the first volume. */
static const family_desc *media_family(EFI_BOOT_SERVICES *bs,
                                       const iso_volumes *vols, UINTN first)
{
    UINTN i;

    for (i = first; i < vols->n; i++) {
        EFI_FILE_PROTOCOL *root;
        const family_desc *fam;

        if (esp_open_root(bs, vols->vol[i], &root) != EFI_SUCCESS)
            continue;
        fam = root_family(root);
        root->Close(root);
        if (fam->id != FAMILY_UNKNOWN)
            return fam;
    }
    return family_by_id(FAMILY_UNKNOWN);
}

iso_sig iso_pe_signature(const void *buf, UINTN len)
{
    pe_hdr h;
    size_t entry;
    uint32_t off, size;
    int rc;

    /* From here on the file exists but does not parse, which is a different
     * fact from "no loader" and gets a different label. */
    if (buf == (const void *)0 || pe_parse(buf, (size_t)len, &h) != PE_OK)
        return ISO_SIG_UNKNOWN;
    rc = pe_dir(&h, buf, PE_DIR_SECURITY, &entry, &off, &size);
    /* An image with a short directory array simply has no certificate table
     * entry, which is an answer, not a parse failure. */
    if (rc == PE_ERR_ABSENT)
        return ISO_SIG_ABSENT;
    if (rc != PE_OK)
        return ISO_SIG_UNKNOWN;
    return size != 0 ? ISO_SIG_PRESENT : ISO_SIG_ABSENT;
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

EFI_STATUS iso_boot_path(EFI_BOOT_SERVICES *bs, EFI_HANDLE esp_handle,
                         EFI_DEVICE_PATH_PROTOCOL **out)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *esp_path;
    EFI_STATUS status;

    if (bs == (EFI_BOOT_SERVICES *)0 || out == (EFI_DEVICE_PATH_PROTOCOL **)0)
        return EFI_INVALID_PARAMETER;
    *out = (EFI_DEVICE_PATH_PROTOCOL *)0;
    status = bs->HandleProtocol(esp_handle, &dp_guid, (void **)&esp_path);
    if (EFI_ERROR(status))
        return status;
    return build_load_path(bs, esp_path, out);
}

/* ---- our shim's verify protocol, out of the way of the ISO's ----
 *
 * An ISO's own chain is its shim, then its signed GRUB, and GRUB asks "a"
 * SHIM_LOCK to verify the kernel: LocateProtocol, which answers with the
 * first instance installed. Under this loader that is the stick's shim,
 * installed before the ISO's shim ran. A shim 15.x (MX Linux 25.2 ships
 * one) records that its loader verified the kernel only when its own
 * protocol is asked, and halts the machine at ExitBootServices otherwise
 * ("Bootloader has not verified loaded image. System is compromised.").
 *
 * So for the length of the payload's run, the instances already installed
 * are uninstalled, and put back exactly as they were when it returns. The
 * ISO's shim then installs the only one there is, which is the one its GRUB
 * finds. Nothing gets easier to run: a GRUB that finds no SHIM_LOCK under
 * Secure Boot refuses its kernel, and the one it finds is its own vendor's,
 * which the platform verified against db before any of this ran. With
 * Secure Boot off shim installs none and there is nothing to hide.
 */
#define SHIM_LOCK_HIDE_MAX 4u
static EFI_HANDLE g_lock_h[SHIM_LOCK_HIDE_MAX];
static void      *g_lock_if[SHIM_LOCK_HIDE_MAX];
static UINTN      g_lock_n;

static void shim_lock_hide(EFI_BOOT_SERVICES *bs)
{
    EFI_GUID guid = SHIM_LOCK_GUID;
    EFI_HANDLE *hs = (EFI_HANDLE *)0;
    UINTN n = 0, i;

    g_lock_n = 0;
    if (bs->LocateHandleBuffer(ByProtocol, &guid, (void *)0, &n, &hs) !=
        EFI_SUCCESS)
        return;
    for (i = 0; i < n && g_lock_n < SHIM_LOCK_HIDE_MAX; i++) {
        void *iface = (void *)0;

        if (bs->HandleProtocol(hs[i], &guid, &iface) != EFI_SUCCESS ||
            bs->UninstallProtocolInterface(hs[i], &guid, iface) !=
                EFI_SUCCESS)
            continue;
        g_lock_h[g_lock_n] = hs[i];
        g_lock_if[g_lock_n] = iface;
        g_lock_n++;
    }
    bs->FreePool(hs);
}

static void shim_lock_restore(EFI_BOOT_SERVICES *bs)
{
    EFI_GUID guid = SHIM_LOCK_GUID;
    UINTN i;

    for (i = 0; i < g_lock_n; i++)
        (void)bs->InstallProtocolInterface(&g_lock_h[i], &guid, 0u,
                                           g_lock_if[i]);
    g_lock_n = 0;
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
    tui_step(result->stage);
    status = bs->HandleProtocol(esp_handle, &dp_guid, (void **)&esp_path);
    if (EFI_ERROR(status))
        return status;

    result->stage = L"building the payload's device path";
    status = build_load_path(bs, esp_path, &load_path);
    if (EFI_ERROR(status))
        return status;

    /* Shim is here but not enforcing (Secure Boot off, or MokSBState off):
     * the payload is loaded by the firmware, as a firmware image. An image
     * shim's loader protocol created is not one to the firmware, and a
     * payload that loads and connects a driver of its own (VMware ESXi's
     * loader and its ISO9660 driver) then fails: the firmware's LoadImage
     * refuses a parent it did not load, and its ConnectController faults on
     * the driver. The firmware still enforces db when Secure Boot is on and
     * only shim's validation is off; that refusal is answered by loading
     * through shim's loader (DESIGN Section 7.6). */
    status = EFI_NOT_FOUND;   /* not loaded yet */
    if (ops.via_shim && ops.table_unhooked) {
        result->stage = L"LoadImage of \\EFI\\BOOT\\BOOT" EFI_ARCH_UP ".EFI (firmware)";
        tui_step(result->stage);
        status = bs->LoadImage(FALSE, parent_image, load_path, (void *)0, 0,
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
            tui_step_done(L"refused");
        }
    }

    /* SourceBuffer stays NULL on both paths: see build_load_path above for
     * why the image is always named rather than handed over as bytes. */
    if (status != EFI_SUCCESS) {
        result->stage = ops.via_shim
                            ? L"LoadImage of \\EFI\\BOOT\\BOOT" EFI_ARCH_UP ".EFI (shim)"
                            : L"LoadImage of \\EFI\\BOOT\\BOOT" EFI_ARCH_UP ".EFI (firmware)";
        tui_step(result->stage);
        status = ops.LoadImage(FALSE, parent_image, load_path, (void *)0, 0,
                               &child);
    }
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

    /* The hand-off (DESIGN Section 7.10): a payload may draw nothing for
     * seconds, or draw only on a console we cannot see (Windows media's
     * cdboot.efi puts its press-any-key prompt on GOP); the page above the
     * cursor says where the silence began. */
    tui_step(L"handing over to the payload");
    tui_handoff();

    result->stage = L"StartImage";
    shim_lock_hide(bs);
    status = ops.StartImage(child, &exit_size, &exit_data);
    shim_lock_restore(bs);
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

/* Why the override set is refused, in words the user can act on. The set is
 * computed from bytes off a writable exFAT partition, so "the modified view
 * of this medium is wrong, and here is how" is the message that matters; a
 * single "bad overlay" would send someone reading hex. */
static const CHAR16 *overlay_reason(int rc)
{
    switch (rc) {
    case ISO_OVL_ERR_COUNT:
        return L"the override set has more entries than the loader allows";
    case ISO_OVL_ERR_EMPTY:
        return L"the override set has a zero-length entry";
    case ISO_OVL_ERR_ORDER:
        return L"the override set is unsorted or its entries overlap";
    case ISO_OVL_ERR_RANGE:
        return L"an override reaches past the end of the virtual medium";
    case ISO_OVL_ERR_SOURCE:
        return L"an override's sidecar file is missing or too short";
    case ISO_OVL_ERR_GEOM:
        return L"the appended region is not a whole number of blocks";
    default:
        return L"the override set failed validation";
    }
}

static EFI_STATUS iso_open(EFI_SYSTEM_TABLE *st, fs_vol *vol,
                           const fs_dirent *file, iso_media_kind kind,
                           const iso_overlay *ovl, const CHAR16 **stage,
                           iso_volumes *vols, UINT8 *owned)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_HANDLE *before = (EFI_HANDLE *)0;
    UINTN before_n = 0;
    UINT32 block_size = (kind == ISO_MEDIA_DISK) ? ISO_DISK_BLOCK_SIZE
                                                 : ISO_CD_BLOCK_SIZE;
    uint64_t src_sizes[ISO_MAX_OVERRIDES];
    uint64_t size;
    uint64_t virt_size;
    EFI_STATUS status;
    int rc;

    vols->n = 0;
    *owned = 0;
    *stage = L"preparing the virtual media";
    tui_step(*stage);

    if (g_dev.in_use) {
        *stage = L"previous virtual media still held by a driver";
        return EFI_ACCESS_DENIED;
    }
    if (fs_file_open(vol, &g_dev.file, file) != FS_OK) {
        *stage = L"opening the image on its volume";
        return EFI_DEVICE_ERROR;
    }
    /* The open comes first because the size does: a directory entry carries
     * one on exFAT and not on every filesystem (ext4 keeps it in the inode),
     * so the byte count this device advertises is the opened file's. */
    size = fs_file_size(&g_dev.file);
    if (size < block_size) {
        *stage = (kind == ISO_MEDIA_DISK)
                     ? L"the .img is too small to be disk media"
                     : L"the .iso is too small to be CD media";
        return EFI_UNSUPPORTED;
    }
    /* A disk image is raw sectors or it is nothing. A file whose length is
     * not a whole number of them is either truncated or a container format
     * (vhd/vdi/qcow2), and DESIGN Section 6 stage 2 refuses those on purpose:
     * the entire mechanism is "let the firmware parse what it already knows",
     * and we do not translate anything on the way. */
    if (kind == ISO_MEDIA_DISK && size % ISO_DISK_BLOCK_SIZE != 0) {
        *stage = L"the .img is not a whole number of 512-byte sectors";
        return EFI_UNSUPPORTED;
    }

    /* Validate the modified view BEFORE anything is published, so an invalid
     * set costs a refusal rather than a device that serves attacker-chosen
     * bytes. Everything the check needs about the sidecars is read here, which
     * is the only side that can call fs_file_size; the arithmetic itself stays
     * in the EFI-free overlay.h so it can be tested on the host. */
    virt_size = size - size % block_size;
    if (ovl != (const iso_overlay *)0 && (ovl->n != 0 || ovl->extra != 0)) {
        size_t i;

        if (ovl->n > ISO_MAX_OVERRIDES) {
            *stage = overlay_reason(ISO_OVL_ERR_COUNT);
            return EFI_INVALID_PARAMETER;
        }
        rc = iso_overlay_media_size(ovl, size, block_size, &virt_size);
        if (rc != ISO_OVL_OK) {
            *stage = overlay_reason(rc);
            return EFI_UNSUPPORTED;
        }
        for (i = 0; i < ovl->n; i++) {
            /* Bytes the loader computed itself are as long as it says:
             * nothing on the stick decides their length. */
            if (ovl->ovl[i].src == (fs_file *)0 &&
                ovl->ovl[i].mem != (const uint8_t *)0) {
                src_sizes[i] = ovl->ovl[i].src_off + ovl->ovl[i].len;
                continue;
            }
            if (ovl->ovl[i].src == (fs_file *)0) {
                *stage = overlay_reason(ISO_OVL_ERR_SOURCE);
                return EFI_INVALID_PARAMETER;
            }
            /* One file cursor cannot serve two interleaved streams: the exFAT
             * driver restarts its FAT walk on every backward seek, so an
             * override pointed at the image's own handle would turn each block
             * read into a full chain walk. Refuse rather than crawl. */
            if (ovl->ovl[i].src == &g_dev.file) {
                *stage = L"an override names the image itself as its sidecar";
                return EFI_INVALID_PARAMETER;
            }
            src_sizes[i] = fs_file_size(ovl->ovl[i].src);
        }
        rc = iso_overlay_check(ovl, virt_size, src_sizes);
        if (rc != ISO_OVL_OK) {
            *stage = overlay_reason(rc);
            return EFI_INVALID_PARAMETER;
        }
    }

    device_init(&g_dev, size, virt_size, block_size, ovl);
    *owned = 1;

    *stage = L"publishing the virtual media";
    tui_step(*stage);
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
    tui_step(*stage);
    status = sfs_handles(bs, &before, &before_n);
    if (EFI_ERROR(status))
        return status;

    /* The firmware's own drivers do the parsing - El Torito for a CD, GPT/MBR
     * (or nothing at all, for a superfloppy) for a disk - and its FAT driver
     * mounts whatever ESP that produces. We write no ISO9660 and no partition
     * reader for this (DESIGN Section 6 stage 3). Recursive, so the disk
     * flavor's partition child gets its own FAT driver bound in the same
     * call. */
    *stage = L"ConnectController on the virtual media";
    tui_step(*stage);
    /* Set before the call: a partial bind still leaves drivers attached, and
     * teardown must disconnect them. */
    g_dev.connected = 1;
    status = bs->ConnectController(g_dev.handle, (EFI_HANDLE *)0,
                                   (EFI_DEVICE_PATH_PROTOCOL *)0, TRUE);
    if (!EFI_ERROR(status)) {
        *stage = L"finding the medium's EFI System Partition";
        tui_step(*stage);
        status = collect_iso_volumes(bs, &g_dev, before, before_n, vols);
        if (!EFI_ERROR(status))
            select_boot_volume(bs, vols);
    }

    if (before != (EFI_HANDLE *)0)
        bs->FreePool(before);
    return status;
}

/* Whether the firmware's FAT driver reads a FAT whose sector size is not
 * the device's block size. U-Boot's does not: it surfaces the El Torito
 * image as a partition of the 2048-byte device and then refuses its 512-byte
 * FAT ("FAT sector size mismatch"). EDK2's reads through Disk I/O and does
 * not care. A device published, torn down and published again is not bound
 * a second time there, so this is asked before anything is published. */
static BOOLEAN firmware_reads_eltorito_fat(const EFI_SYSTEM_TABLE *st)
{
    static const CHAR16 uboot[] = L"Das U-Boot";
    const CHAR16 *v = st->FirmwareVendor;
    UINTN i;

    if (v == (const CHAR16 *)0)
        return TRUE;
    for (i = 0; uboot[i] != 0; i++)
        if (v[i] != uboot[i])
            return TRUE;
    return FALSE;
}

/* Whether the image is also a disk: an isohybrid MBR with an ESP entry, or
 * a protective one before a GPT. Booted as 512-byte media, such an image
 * is the stick it was written for, and the firmware finds its ESP by the
 * partition table instead of by El Torito. */
static BOOLEAN image_is_hybrid(fs_vol *vol, const fs_dirent *file)
{
    fs_file f;
    uint8_t mbr[512];
    unsigned i;

    if (fs_file_open(vol, &f, file) != FS_OK ||
        fs_file_read(&f, 0, sizeof mbr, mbr) != FS_OK)
        return FALSE;
    if (mbr[510] != 0x55u || mbr[511] != 0xAAu)
        return FALSE;
    for (i = 0; i < 4u; i++) {
        uint8_t type = mbr[446u + 16u * i + 4u];

        if (type == 0xEFu || type == 0xEEu)
            return TRUE;
    }
    return FALSE;
}

EFI_STATUS iso_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                    fs_vol *vol, const fs_dirent *file, iso_media_kind kind,
                    iso_boot_result *result)
{
    return iso_boot_overlaid(st, parent_image, vol, file, kind,
                             (const iso_overlay *)0, result);
}

EFI_STATUS iso_boot_overlaid(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                             fs_vol *vol, const fs_dirent *file,
                             iso_media_kind kind, const iso_overlay *ovl,
                             iso_boot_result *result)
{
    return iso_boot_overlaid_served(st, parent_image, vol, file, kind, ovl,
                                    (iso_served_fn)0, (void *)0, result);
}

EFI_STATUS iso_boot_overlaid_served(EFI_SYSTEM_TABLE *st,
                                    EFI_HANDLE parent_image, fs_vol *vol,
                                    const fs_dirent *file,
                                    iso_media_kind kind,
                                    const iso_overlay *ovl,
                                    iso_served_fn served, void *ctx,
                                    iso_boot_result *result)
{
    EFI_BOOT_SERVICES *bs;
    iso_volumes vols;
    UINT8 owned = 0;
    EFI_STATUS status;
    EFI_STATUS teardown;

    if (st == (void *)0 || vol == (void *)0 || file == (void *)0 ||
        result == (void *)0)
        return EFI_INVALID_PARAMETER;

    bs = st->BootServices;
    result->status = EFI_SUCCESS;
    result->family = (const family_desc *)0;

    /* A CD the firmware cannot boot by El Torito, published as the disk it
     * also is (firmware_reads_eltorito_fat, image_is_hybrid). */
    if (kind == ISO_MEDIA_CD && !firmware_reads_eltorito_fat(st) &&
        image_is_hybrid(vol, file))
        kind = ISO_MEDIA_DISK;
    status = iso_open(st, vol, file, kind, ovl, &result->stage, &vols, &owned);
    if (!EFI_ERROR(status) && served != (iso_served_fn)0) {
        /* The caller's image, with the medium published beside it. No
         * media_family here: the caller already knows what this medium is,
         * which is why it is starting something other than the medium's own
         * loader. */
        status = served(ctx, st, parent_image, vols.vol[0], result);
    } else if (!EFI_ERROR(status)) {
        /* Detection is reported, never acted on. It runs before the chainload
         * only so the answer exists while the volumes are still mounted; the
         * load path below is byte-for-byte what it was without it, and no
         * cmdline template has a consumer yet (DESIGN Section 6.2). */
        result->family = media_family(bs, &vols, 0);
        status = chainload(st, parent_image, vols.vol[0], result);
    }
    if (EFI_ERROR(status))
        result->status = status;

    if (!owned)
        return status;

    teardown = device_teardown(bs, &g_dev);
    if (EFI_ERROR(teardown)) {
        /* Worth overriding a payload's own exit status: from here on the
         * loader refuses further ISO boots, and the user needs to know. */
        result->stage = L"releasing the virtual media";
        result->status = teardown;
        return teardown;
    }
    return status;
}

/* ---- the survey ----
 *
 * The menu's label for a medium: what family it is, whether its default
 * loader carries a signature, and what the boot predictions make of it.
 *
 * None of it goes near the firmware: falcon-core opens the image file on p2
 * and answers from it (DESIGN Section 6). Three things follow:
 *
 *   - no ConnectController per entry, the firmware call some implementations
 *     fault in (the ESXi case above);
 *   - one spelling finds a file on every medium: fs_lookup folds case, where
 *     EDK2's UDF driver matches case-sensitively;
 *   - no cap on how many entries are probed: a probe is a few reads off p2,
 *     not a device published and torn down.
 *
 * What still asks the firmware is the chainload, and only the chainload:
 * LoadImage is handed a device path to a volume the platform mounted itself,
 * which is DESIGN Section 7.2 and is not something a medium falcon-core
 * parsed could satisfy. select_boot_volume and the probe helpers above stay
 * for it.
 *
 * The objects are static because they are about 22 KB together and the
 * loader has no heap worth putting them on. The medium in particular MUST
 * NOT be a local: its mounted volumes hold the addresses of offset shims
 * inside it (falcon-core/src/media/medium.h).
 */
static medium     g_survey_medium;
static fs_file    g_survey_file;
static bootfs_ctx g_survey_ctx;

/* bootcfg_read's scratch for the BIOS-only question, the direct route's
 * size. Pool, and only for a medium with no default loader: most rows never
 * ask it. */
#define SURVEY_CFG_SCRATCH ((UINTN)256u * 1024u)

/* Whether the machine can boot a CD with no default loader at all: its
 * kernel, directly, or nothing (DESIGN Section 7.8, {BIOS ONLY}). The
 * medium is g_survey_medium, already open. UNKNOWN when the pool cannot
 * spare the scratch: an unanswered question marks nothing. */
static uefiboot_answer survey_uefi(EFI_BOOT_SERVICES *bs, family_id fam)
{
    uefiboot_work *w = (uefiboot_work *)0;
    char *scratch = (char *)0;
    uefiboot_answer a = UEFIBOOT_UNKNOWN;

    if (bs->AllocatePool(EfiLoaderData, sizeof *w, (void **)&w) !=
        EFI_SUCCESS)
        return UEFIBOOT_UNKNOWN;
    if (bs->AllocatePool(EfiLoaderData, SURVEY_CFG_SCRATCH,
                         (void **)&scratch) == EFI_SUCCESS) {
        a = uefiboot_medium(&g_survey_medium, fam, w, scratch,
                            SURVEY_CFG_SCRATCH, EFI_ARCH_MACHINE);
        bs->FreePool(scratch);
    }
    bs->FreePool(w);
    return a;
}

/* The medium's default loader, opened on whichever volume carries it.
 *
 * The El Torito boot image first, because that is where it lives on optical
 * media, then every other volume: a .img has no boot image and keeps its
 * loader on whatever partition is the ESP. Returns 0 when no volume has one,
 * which is an answer about the medium rather than a failure. */
static int survey_open_loader(medium *m, fs_file *f, uint64_t *size)
{
    unsigned i;
    unsigned n = medium_count(m);
    fs_vol *boot = medium_boot_vol(m);

    for (i = 0; i <= n; i++) {
        fs_vol *v = (i == 0) ? boot : medium_vol_at(m, i - 1u);
        fs_dir scratch;
        fs_dirent ent;

        if (v == (fs_vol *)0 || (i > 0 && v == boot))
            continue;
        if (fs_lookup(v, ISO_BOOT_PATH, &scratch, &ent) != FS_OK)
            continue;
        if (ent.is_dir || fs_file_open(v, f, &ent) != FS_OK)
            continue;
        *size = fs_file_size(f);
        return 1;
    }
    return 0;
}

EFI_STATUS iso_survey_media(EFI_SYSTEM_TABLE *st, fs_vol *vol,
                            const fs_dirent *file, iso_media_kind kind,
                            const trust_stores *stores, iso_survey *out)
{
    fs_file loader;
    uint64_t loader_size = 0;
    unsigned i;

    if (out == (void *)0)
        return EFI_INVALID_PARAMETER;
    out->family = family_by_id(FAMILY_UNKNOWN);
    out->sig = ISO_SIG_UNKNOWN;
    out->uefi = UEFIBOOT_UNKNOWN;
    out->uki = 0;
    memset(&out->trust, 0, sizeof(out->trust));
    if (st == (void *)0 || vol == (void *)0 || file == (void *)0)
        return EFI_INVALID_PARAMETER;

    /* The flavour is a hint to the FIRMWARE about how to parse a published
     * device (iso.h), and there is no published device here: medium_open
     * works out what the bytes are. Only the BIOS-only question below reads
     * it: the direct route that question is about takes a CD and nothing
     * else. */

    if (fs_file_open(vol, &g_survey_file, file) != FS_OK)
        return EFI_NOT_FOUND;
    if (medium_open(&g_survey_medium, fs_file_read_cb, &g_survey_file,
                    fs_file_size(&g_survey_file)) != MEDIUM_OK) {
        /* Nothing on it we can read. Not an error worth showing, just an
         * entry we cannot say anything about. */
        out->sig = ISO_SIG_NO_EFI;
        return EFI_SUCCESS;
    }

    /* Every volume, in the precedence order medium_open put them in, until
     * one classifies. The boot image is included rather than skipped: it is
     * what the firmware path fingerprints first today, and leaving it out
     * would be a behaviour change with nothing measured behind it. */
    for (i = 0; i < medium_count(&g_survey_medium); i++) {
        family_probe probe;
        const family_desc *fam;

        bootfs_probe(&g_survey_ctx, medium_vol_at(&g_survey_medium, i),
                     &probe);
        fam = family_detect(&probe);
        if (fam->id != FAMILY_UNKNOWN) {
            out->family = fam;
            break;
        }
    }

    if (!survey_open_loader(&g_survey_medium, &loader, &loader_size)) {
        out->sig = ISO_SIG_NO_EFI;
        if (kind == ISO_MEDIA_CD)
            out->uefi = survey_uefi(st->BootServices, out->family->id);
        return EFI_SUCCESS;
    }
    out->uefi = UEFIBOOT_YES;
    {
        uint8_t head[PE_HEAD_MAX];
        uint64_t want = loader_size < sizeof head ? loader_size
                                                  : (uint64_t)sizeof head;

        if (want == 0 || fs_file_read(&loader, 0, want, head) != FS_OK)
            out->sig = ISO_SIG_UNKNOWN;
        else
            out->sig = iso_pe_signature(head, (UINTN)want);
    }
    /* The prediction reads the whole file, which for a first stage is always
     * small enough to hash, and reads it through the same volume the label
     * came from. */
    if (stores != (const trust_stores *)0 &&
        (out->sig == ISO_SIG_PRESENT || out->sig == ISO_SIG_ABSENT))
        trust_probe(stores, fs_file_read_cb, &loader, loader_size,
                    &out->trust);
    return EFI_SUCCESS;
}
