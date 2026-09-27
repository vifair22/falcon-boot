/* The single discovery pass of DESIGN Section 9. See discover.h for why it
 * exists; this file is the mechanism.
 */

#include <discover/discover.h>

#include <lib/string.h>
#include <blk/rebl.h>
#include <discover/partefi.h>
#include <efi/loadedimage.h>

static const uint8_t luks2_magic[6] = { 'L', 'U', 'K', 'S', 0xba, 0xbe };

static discover_dev g_devs[DISCOVER_MAX];
static UINTN g_count;
static UINTN g_skipped;

/* The ragged ends of every read the drivers make, one sector of the largest
 * block size we take (4096, a 4Kn disk). One buffer for every device: the
 * loader makes one read at a time, and blk_rebl_read copies out of it before
 * it returns. A device with larger blocks still gets its whole-block reads,
 * and fails the others. */
static UINT8 g_bounce[4096];

/* Block I/O itself: whole blocks at block-aligned offsets, which is the only
 * thing blk_rebl_read asks of it. */
static int read_blocks(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    discover_dev *d = ctx;
    UINT32 bs = d->bio->Media->BlockSize;

    if (off % bs != 0 || len % bs != 0)
        return -1;
    if (d->bio->ReadBlocks(d->bio, d->bio->Media->MediaId, off / bs,
                           (UINTN)len, buf) != EFI_SUCCESS)
        return -1;
    return 0;
}

/* The one place the UEFI loader meets the drivers, so the one place the
 * re-blocking layer goes (DESIGN Section 3.4): every consumer that mounts,
 * probes or unlocks a row reads through here, and none of them needs to
 * know the disk's block size. Built per call rather than kept per row, so a
 * medium changed under a row is read at the block size it has now. */
int discover_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    discover_dev *d = ctx;
    blk_rebl r;

    if (d == (discover_dev *)0 || d->bio == (EFI_BLOCK_IO_PROTOCOL *)0 ||
        d->bio->Media->BlockSize == 0)
        return -1;
    r.read = read_blocks;
    r.ctx = d;
    r.sector_size = d->bio->Media->BlockSize;
    r.bounce = g_bounce;
    r.bounce_len = (uint32_t)sizeof g_bounce;
    return blk_rebl_read(&r, off, len, buf);
}

/* Cheap first question about an unknown volume: is there a LUKS2 primary
 * header here at all? One block read, before any parse, because the common
 * case on a real machine is a handle for something that is not ours. */
static uint8_t probe_luks(discover_dev *d)
{
    uint8_t magic[sizeof luks2_magic];

    if (discover_read(d, 0, sizeof magic, magic) != 0)
        return 0;
    return (uint8_t)(memcmp(magic, luks2_magic, sizeof luks2_magic) == 0);
}

static void classify(EFI_SYSTEM_TABLE *st, discover_dev *d)
{
    d->media_present = (uint8_t)(d->bio->Media->MediaPresent ? 1 : 0);
    d->accept[PART_ROLE_ISO] =
        (uint8_t)(part_scan_accept(st, d->handle, PART_ROLE_ISO) ? 1 : 0);
    d->accept[PART_ROLE_SHELF] =
        (uint8_t)(part_scan_accept(st, d->handle, PART_ROLE_SHELF) ? 1 : 0);

    d->has_type = (uint8_t)(part_type_guid(st, d->handle, &d->type) == PART_OK);
    d->role = FS_HINT_NONE;
    if (part_is_falcon_iso(st, d->handle))
        d->role = FS_HINT_ISO;
    else if (part_is_falcon_shelf(st, d->handle))
        d->role = FS_HINT_SHELF;

    d->luks2 = 0;
    d->fs = (const fs_ops *)0;
    if (!d->media_present)
        return;

    d->luks2 = probe_luks(d);
    if (d->luks2)
        return;
    /* The role hint is the GPT entry's, not the scan filter's: the filter says
     * which handles a role may look at (and accepts everything when nothing
     * is stamped), while the hint says what this partition claims to be, and
     * only the second is a reason to try one driver before another. */
    d->fs = fs_identify(discover_read, d, d->role);
}

/* The partition the loader was read from, or NULL. Under shim that is still
 * our own ESP: shim loads its next stage from its own device. */
static EFI_HANDLE boot_device(EFI_SYSTEM_TABLE *st, EFI_HANDLE image)
{
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = (EFI_LOADED_IMAGE_PROTOCOL *)0;

    if (image == (EFI_HANDLE)0 ||
        st->BootServices->HandleProtocol(image, &li_guid, (void **)&li) !=
            EFI_SUCCESS ||
        li == (EFI_LOADED_IMAGE_PROTOCOL *)0)
        return (EFI_HANDLE)0;
    return li->DeviceHandle;
}

void discover_scan(EFI_SYSTEM_TABLE *st, EFI_HANDLE image)
{
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    EFI_HANDLE boot = (EFI_HANDLE)0;
    UINTN count = 0;
    UINTN i;

    g_count = 0;
    g_skipped = 0;
    /* Media that appeared since the last look would otherwise be judged
     * against a cached picture of which roles exist in the machine. */
    part_scan_reset();

    if (st == (EFI_SYSTEM_TABLE *)0)
        return;
    if (st->BootServices->LocateHandleBuffer(ByProtocol, &bio_guid, (void *)0,
                                             &count, &handles) != EFI_SUCCESS)
        return;
    boot = boot_device(st, image);

    for (i = 0; i < count; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio;
        discover_dev *d;

        if (st->BootServices->HandleProtocol(handles[i], &bio_guid,
                                             (void **)&bio) != EFI_SUCCESS)
            continue;
        if (g_count >= DISCOVER_MAX) {
            g_skipped++;
            continue;
        }
        d = &g_devs[g_count];
        memset(d, 0, sizeof(*d));
        d->handle = handles[i];
        d->bio = bio;
        classify(st, d);
        if (boot != (EFI_HANDLE)0)
            d->boot_disk =
                (uint8_t)(part_same_disk(st, handles[i], boot) ? 1 : 0);
        g_count++;
    }

    st->BootServices->FreePool(handles);
}

UINTN discover_count(void)
{
    return g_count;
}

UINTN discover_skipped(void)
{
    return g_skipped;
}

discover_dev *discover_at(UINTN i)
{
    return (i < g_count) ? &g_devs[i] : (discover_dev *)0;
}

discover_dev *discover_next(part_role role, UINTN *cursor)
{
    UINTN i;

    if (cursor == (UINTN *)0 || (UINTN)role >= PART_ROLE_COUNT)
        return (discover_dev *)0;
    for (i = *cursor; i < g_count; i++) {
        if (!g_devs[i].media_present)
            continue;
        if (!g_devs[i].accept[role])
            continue;
        *cursor = i + 1u;
        return &g_devs[i];
    }
    *cursor = g_count;
    return (discover_dev *)0;
}
