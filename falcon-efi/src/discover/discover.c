/* The single discovery pass of DESIGN Section 9, and identification by
 * record of Section 3.1. See discover.h for why it exists; this file is the
 * mechanism.
 */

#include <discover/discover.h>

#include <lib/string.h>
#include <discover/partefi.h>
#include <efi/loadedimage.h>
#include <ident/identfs.h>
#include <luks/luks.h>

/* Largest block size any firmware Block I/O instance we care about reports,
 * and therefore the size of the one-block probe buffer. */
#define PROBE_BUF 4096u

static const uint8_t luks2_magic[6] = { 'L', 'U', 'K', 'S', 0xba, 0xbe };

static discover_dev g_devs[DISCOVER_MAX];
static UINTN g_count;
static UINTN g_skipped;

/* The boot partition's record, and what reading it came to. */
static discover_id_state g_id_state = DISCOVER_ID_NO_DEVICE;
static ident_rec g_install;

/* Scratch for reading one row's record at a time: a mounted volume and its
 * /.falcon, or a LUKS2 header's JSON. Static, being kilobytes each; the
 * scan reads one row at a time and keeps nothing mounted. */
static ident_fs_scratch g_rec_fs;
static luks_vol g_rec_luks;
static char g_rec_json[LUKS_JSON_CAP];

int discover_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    discover_dev *d = ctx;
    UINT32 bs;

    if (d == (discover_dev *)0 || d->bio == (EFI_BLOCK_IO_PROTOCOL *)0)
        return -1;
    bs = d->bio->Media->BlockSize;
    if (bs == 0 || off % bs != 0 || len % bs != 0)
        return -1;
    if (d->bio->ReadBlocks(d->bio, d->bio->Media->MediaId, off / bs,
                           (UINTN)len, buf) != EFI_SUCCESS)
        return -1;
    return 0;
}

/* Cheap first question about an unknown volume: is there a LUKS2 primary
 * header here at all? One block read, before any parse, because the common
 * case on a real machine is a handle for something that is not ours. */
static uint8_t probe_luks(discover_dev *d)
{
    static UINT8 blk[PROBE_BUF];
    UINT32 bs = d->bio->Media->BlockSize;

    if (bs == 0 || bs > PROBE_BUF)
        return 0;
    if (d->bio->ReadBlocks(d->bio, d->bio->Media->MediaId, 0, bs, blk) !=
        EFI_SUCCESS)
        return 0;
    return (uint8_t)(memcmp(blk, luks2_magic, sizeof luks2_magic) == 0);
}

/* The record a row carries, if any: the falcon token for a LUKS2 volume
 * (read before any key is known), /.falcon for a volume a driver claimed.
 * Nothing else is a record: a FAT volume's binary record names a `boot`
 * partition, which no consumer of the inventory asks for. */
static void read_record(discover_dev *d)
{
    int rc = IDENT_ERR_NONE;

    d->has_rec = 0;
    if (d->luks2) {
        uint32_t version = 0;
        const char *role = (const char *)0;
        const char *id = (const char *)0;
        size_t role_len = 0;
        size_t id_len = 0;

        if (luks_open(&g_rec_luks, discover_read, d, g_rec_json,
                      LUKS_JSON_CAP) == LUKS_OK &&
            luks_falcon_token(&g_rec_luks, &version, &role, &role_len, &id,
                              &id_len) == LUKS_OK)
            rc = ident_from_strings(version, role, role_len, id, id_len,
                                    &d->rec);
    } else if (d->fs != (const fs_ops *)0) {
        if (fs_mount(&g_rec_fs.vol, d->fs, discover_read, d) == FS_OK)
            rc = ident_read_vol(&g_rec_fs.vol, &g_rec_fs, &d->rec);
        g_rec_fs.vol.ops = (const struct fs_ops *)0;
    }
    d->has_rec = (uint8_t)(rc == IDENT_OK ? 1 : 0);
}

static void classify(discover_dev *d)
{
    d->media_present = (uint8_t)(d->bio->Media->MediaPresent ? 1 : 0);
    d->luks2 = 0;
    d->fs = (const fs_ops *)0;
    d->has_rec = 0;
    d->ours = 0;
    if (!d->media_present)
        return;

    d->luks2 = probe_luks(d);
    /* A LUKS2 volume's plaintext is nobody's until it is opened. */
    if (!d->luks2)
        d->fs = fs_identify(discover_read, d, FS_HINT_NONE);
    read_record(d);
    d->ours = (uint8_t)(d->has_rec && g_id_state == DISCOVER_ID_OK &&
                        ident_same_install(&d->rec, &g_install));
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

/* The boot partition's first sector, and the `boot` record in it (DESIGN
 * Section 3.1: FAT carries the record at byte 0x1DC of its boot sector).
 * Read through Block I/O rather than the filesystem: it is the one read
 * that needs no driver, and the firmware already has the partition open. */
static discover_id_state read_install(EFI_SYSTEM_TABLE *st, EFI_HANDLE boot)
{
    static UINT8 sector[4096];
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO_PROTOCOL *bio = (EFI_BLOCK_IO_PROTOCOL *)0;
    ident_rec r;
    int rc;

    if (boot == (EFI_HANDLE)0 ||
        st->BootServices->HandleProtocol(boot, &bio_guid, (void **)&bio) !=
            EFI_SUCCESS ||
        bio == (EFI_BLOCK_IO_PROTOCOL *)0 || !bio->Media->MediaPresent ||
        bio->Media->BlockSize < 512u || bio->Media->BlockSize > sizeof sector)
        return DISCOVER_ID_NO_DEVICE;
    if (bio->ReadBlocks(bio, bio->Media->MediaId, 0, bio->Media->BlockSize,
                        sector) != EFI_SUCCESS)
        return DISCOVER_ID_NO_DEVICE;
    rc = ident_decode_bin(sector, &r);
    if (rc == IDENT_ERR_NONE)
        return DISCOVER_ID_NONE;
    if (rc != IDENT_OK || r.role != IDENT_ROLE_BOOT)
        return DISCOVER_ID_BAD;
    g_install = r;
    return DISCOVER_ID_OK;
}

/* One row for handle h, when there is room. */
static void add_row(EFI_SYSTEM_TABLE *st, EFI_HANDLE h)
{
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO_PROTOCOL *bio;
    discover_dev *d;

    if (st->BootServices->HandleProtocol(h, &bio_guid, (void **)&bio) !=
        EFI_SUCCESS)
        return;
    if (g_count >= DISCOVER_MAX) {
        g_skipped++;
        return;
    }
    d = &g_devs[g_count];
    memset(d, 0, sizeof(*d));
    d->handle = h;
    d->bio = bio;
    classify(d);
    g_count++;
}

void discover_scan(EFI_SYSTEM_TABLE *st, EFI_HANDLE image)
{
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    EFI_HANDLE boot = (EFI_HANDLE)0;
    UINTN count = 0;
    UINTN i;
    UINTN pass;

    g_count = 0;
    g_skipped = 0;
    g_id_state = DISCOVER_ID_NO_DEVICE;
    memset(&g_install, 0, sizeof g_install);

    if (st == (EFI_SYSTEM_TABLE *)0)
        return;
    boot = boot_device(st, image);
    /* First, before any row is judged: every row is judged against it. */
    g_id_state = read_install(st, boot);

    if (st->BootServices->LocateHandleBuffer(ByProtocol, &bio_guid, (void *)0,
                                             &count, &handles) != EFI_SUCCESS)
        return;

    /* The boot disk's partitions, then everything else, so a machine with
     * more handles than DISCOVER_MAX loses rows of other disks, never of the
     * one the loader came from. */
    for (pass = 0; pass < 2u; pass++) {
        for (i = 0; i < count; i++) {
            BOOLEAN on_boot_disk =
                (BOOLEAN)(boot != (EFI_HANDLE)0 &&
                          (handles[i] == boot ||
                           part_same_disk(st, handles[i], boot)));

            if ((pass == 0u) != (on_boot_disk != FALSE))
                continue;
            add_row(st, handles[i]);
        }
    }

    st->BootServices->FreePool(handles);
}

discover_id_state discover_install(ident_rec *out)
{
    if (g_id_state == DISCOVER_ID_OK && out != (ident_rec *)0)
        *out = g_install;
    return g_id_state;
}

/* Broken by hand at 72 columns: the console wraps at its own width, which
 * is wherever a word happens to fall. */
const CHAR16 *discover_install_problem(discover_id_state state)
{
    switch (state) {
    case DISCOVER_ID_NO_DEVICE:
        return L"The firmware did not name the partition this loader started "
               L"from,\r\nso no ISO library or shelf is offered.";
    case DISCOVER_ID_NONE:
        return L"The partition this loader started from has no falcon "
               L"record,\r\nso no ISO library or shelf is offered.\r\n"
               L"Write the stick with a current mkstick.";
    case DISCOVER_ID_BAD:
        return L"The falcon record on the partition this loader started "
               L"from is\r\ndamaged or not a boot record, so no ISO library "
               L"or shelf is offered.\r\nWrite the stick with a current "
               L"mkstick.";
    case DISCOVER_ID_OK:
    default:
        return (const CHAR16 *)0;
    }
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

discover_dev *discover_next(ident_role role, UINTN *cursor)
{
    UINTN i;

    if (cursor == (UINTN *)0)
        return (discover_dev *)0;
    for (i = *cursor; i < g_count; i++) {
        if (!g_devs[i].media_present || !g_devs[i].ours ||
            g_devs[i].rec.role != role)
            continue;
        *cursor = i + 1u;
        return &g_devs[i];
    }
    *cursor = g_count;
    return (discover_dev *)0;
}
