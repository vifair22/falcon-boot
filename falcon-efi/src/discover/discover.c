/* The single discovery pass of DESIGN Section 9, and identification by
 * record of Section 3.1. See discover.h for why it exists; this file is the
 * mechanism.
 */

#include <discover/discover.h>

#include <lib/string.h>
#include <blk/rebl.h>
#include <discover/partefi.h>
#include <efi/devpath.h>
#include <efi/loadedimage.h>
#include <ident/identfs.h>
#include <mem/jnlmem.h>
#include <luks/luks.h>

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
/* For the journal table a plaintext shelf's record is read through. */
static EFI_BOOT_SERVICES *g_bs;
static ident_fs_scratch g_rec_fs;
static luks_vol g_rec_luks;
static char g_rec_json[LUKS_JSON_CAP];

/* The ragged ends of every read the drivers make, one sector of the largest
 * block size we take (4096, a 4Kn disk). One buffer for every device: the
 * loader makes one read at a time, and blk_rebl_read copies out of it before
 * it returns. A device with larger blocks still gets its whole-block reads,
 * and fails the others. */
static UINT8 g_bounce[4096];

/* Memory below 4 GiB for Block I/O to transfer into. A firmware that starts
 * the loader itself, rather than through shim, may place the image above
 * 4 GiB, and with it every static buffer; an ASUS B550's AMI NVMe driver
 * then refuses the read with EFI_UNSUPPORTED rather than bouncing it (DESIGN
 * Section 3.1). Allocated on first need and kept; never freed, since the
 * loader reads until it hands over. */
#define LOW_BOUNCE_PAGES 16u
static UINT8 *g_low;

static UINT8 *low_bounce(void)
{
    EFI_PHYSICAL_ADDRESS addr = 0xFFFFFFFFu;

    if (g_low == (UINT8 *)0 && g_bs != (EFI_BOOT_SERVICES *)0 &&
        g_bs->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                            LOW_BOUNCE_PAGES, &addr) == EFI_SUCCESS)
        g_low = (UINT8 *)(uintptr_t)addr;
    return g_low;
}

/* ReadBlocks into buf, through memory below 4 GiB when buf reaches above
 * it. len is whole blocks. */
static EFI_STATUS bio_read(EFI_BLOCK_IO_PROTOCOL *bio, UINT64 lba, UINTN len,
                           void *buf)
{
    UINT32 bs = bio->Media->BlockSize;
    UINTN chunk = (UINTN)LOW_BOUNCE_PAGES * 4096u;
    UINT8 *low;
    UINT8 *dst = (UINT8 *)buf;
    EFI_STATUS es;

    if ((uint64_t)(uintptr_t)buf + len <= 0x100000000ull)
        return bio->ReadBlocks(bio, bio->Media->MediaId, lba, len, buf);
    low = low_bounce();
    if (low == (UINT8 *)0 || bs == 0u || chunk < bs)
        return bio->ReadBlocks(bio, bio->Media->MediaId, lba, len, buf);
    chunk -= chunk % bs;
    while (len > 0u) {
        UINTN n = len < chunk ? len : chunk;

        es = bio->ReadBlocks(bio, bio->Media->MediaId, lba, n, low);
        if (es != EFI_SUCCESS)
            return es;
        memcpy(dst, low, n);
        dst += n;
        lba += n / bs;
        len -= n;
    }
    return EFI_SUCCESS;
}

/* Block I/O itself: whole blocks at block-aligned offsets, which is the only
 * thing blk_rebl_read asks of it. */
static int read_blocks(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    discover_dev *d = ctx;
    UINT32 bs = d->bio->Media->BlockSize;

    if (off % bs != 0 || len % bs != 0)
        return -1;
    if (bio_read(d->bio, off / bs, (UINTN)len, buf) != EFI_SUCCESS)
        return -1;
    return 0;
}

/* The one place the UEFI loader meets the drivers, so the one place the
 * re-blocking layer goes (DESIGN Section 3.4): every consumer that mounts,
 * probes or unlocks a row reads through here, and none of them needs to
 * know the disk's block size. Built per call rather than kept per row, so a
 * medium changed under a row is read at the block size it has now. */
static void (*g_read_hook)(void);

void discover_set_read_hook(void (*hook)(void))
{
    g_read_hook = hook;
}

static int read_piece(discover_dev *d, uint64_t off, uint64_t len, void *buf);

int discover_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    uint8_t *p = buf;

    if (g_read_hook == (void (*)(void))0)
        return read_piece(ctx, off, len, buf);
    while (len > 0) {
        uint64_t n = len < DISCOVER_READ_PIECE ? len : DISCOVER_READ_PIECE;

        g_read_hook();
        if (read_piece(ctx, off, n, p) != 0)
            return -1;
        off += n;
        p += n;
        len -= n;
    }
    return 0;
}

static int read_piece(discover_dev *d, uint64_t off, uint64_t len, void *buf)
{
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
        fs_jmem mem;
        fs_jinfo info;

        /* A plaintext shelf its OS left mounted (hibernated, or the power
         * went) is read through its journal, or it would not be a row. */
        jnl_mem_make(&mem, g_bs);
        if (fs_mount_journal(&g_rec_fs.vol, d->fs, discover_read, d, &mem,
                             &info) == FS_OK)
            rc = ident_read_vol(&g_rec_fs.vol, &g_rec_fs, &d->rec);
        g_rec_fs.vol.ops = (const struct fs_ops *)0;
        jnl_release();
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

/* ---- the boot partition's status line ---- */

static char g_detail[512];
static size_t g_detail_len;

static void det(const char *t)
{
    while (*t != '\0' && g_detail_len < sizeof g_detail)
        g_detail[g_detail_len++] = *t++;
}

static void det_hex(UINT64 v, unsigned digits)
{
    static const char xd[] = "0123456789abcdef";
    char t[19];
    unsigned i;

    t[0] = '0';
    t[1] = 'x';
    for (i = 0; i < digits; i++)
        t[2 + i] = xd[(v >> (4u * (digits - 1u - i))) & 0xFu];
    t[2 + digits] = '\0';
    det(t);
}

/* The handle's device path as its nodes' type/subtype pairs ("02/01 03/17
 * 04/01"): enough to tell a partition from a disk, a RAM disk or a
 * firmware volume without a text protocol the firmware may not have. */
static void det_path(EFI_SYSTEM_TABLE *st, EFI_HANDLE h)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)0;
    const UINT8 *p;
    unsigned n;

    if (h == (EFI_HANDLE)0 ||
        st->BootServices->HandleProtocol(h, &dp_guid, (void **)&dp) !=
            EFI_SUCCESS ||
        dp == (EFI_DEVICE_PATH_PROTOCOL *)0) {
        det(" (no device path)");
        return;
    }
    det(" (path");
    p = (const UINT8 *)dp;
    for (n = 0; n < 12u; n++) {
        const EFI_DEVICE_PATH_PROTOCOL *node =
            (const EFI_DEVICE_PATH_PROTOCOL *)p;
        UINT16 len = dp_node_len(node);

        if (len < sizeof(*node) || dp_is_end(node))
            break;
        det(" ");
        det_hex(node->Type, 2);
        det("/");
        det_hex(node->SubType, 2);
        p += len;
    }
    det(")");
}

/* The boot partition's first sector, and the `boot` record in it (DESIGN
 * Section 3.1: FAT carries the record at byte 0x1DC of its boot sector).
 * Read through Block I/O rather than the filesystem: it is the one read
 * that needs no driver, and the firmware already has the partition open.
 *
 * The buffer honours the device's IoAlign (UEFI 2.10 Section 13.9): a
 * device may refuse a read into memory that is not aligned as it says,
 * and a static array is aligned only as the compiler and the image's
 * placement happen to leave it. Like every read here it goes through
 * bio_read, below 4 GiB. Each check that fails says so in the status
 * line. */
static discover_id_state read_install(EFI_SYSTEM_TABLE *st, EFI_HANDLE boot)
{
    static UINT8 sector[2u * 4096u];
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO_PROTOCOL *bio = (EFI_BLOCK_IO_PROTOCOL *)0;
    EFI_STATUS es;
    UINT8 *buf;
    UINTN align;
    ident_rec r;
    int rc;

    if (boot == (EFI_HANDLE)0) {
        det("no device handle");
        return DISCOVER_ID_NO_DEVICE;
    }
    es = st->BootServices->HandleProtocol(boot, &bio_guid, (void **)&bio);
    if (es != EFI_SUCCESS || bio == (EFI_BLOCK_IO_PROTOCOL *)0) {
        det("no Block I/O on the handle: ");
        det_hex((UINT64)es, 16);
        det_path(st, boot);
        return DISCOVER_ID_NO_DEVICE;
    }
    if (!bio->Media->MediaPresent) {
        det("Block I/O says no media");
        det_path(st, boot);
        return DISCOVER_ID_READ;
    }
    align = bio->Media->IoAlign > 1u ? bio->Media->IoAlign : 1u;
    if (bio->Media->BlockSize < 512u || bio->Media->BlockSize > 4096u ||
        align > 4096u || (align & (align - 1u)) != 0u) {
        det("Block I/O block size ");
        det_hex(bio->Media->BlockSize, 8);
        det(", IoAlign ");
        det_hex(bio->Media->IoAlign, 8);
        det_path(st, boot);
        return DISCOVER_ID_READ;
    }
    buf = (UINT8 *)(((uintptr_t)sector + (align - 1u)) & ~(uintptr_t)(align - 1u));
    es = bio_read(bio, 0, bio->Media->BlockSize, buf);
    if (es != EFI_SUCCESS) {
        det("reading sector 0 returned ");
        det_hex((UINT64)es, 16);
        det(" (MediaId ");
        det_hex(bio->Media->MediaId, 8);
        det(", LastBlock ");
        det_hex(bio->Media->LastBlock, 16);
        det(", buffer ");
        det_hex((UINT64)(uintptr_t)buf, 16);
        det(g_low != (UINT8 *)0 ? ", through memory below 4 GiB)" : ")");
        det_path(st, boot);
        return DISCOVER_ID_READ;
    }
    rc = ident_decode_bin(buf, &r);
    if (rc == IDENT_ERR_NONE) {
        det("sector 0 has no falcon record");
        return DISCOVER_ID_NONE;
    }
    if (rc != IDENT_OK || r.role != IDENT_ROLE_BOOT) {
        det("sector 0's falcon record is damaged or not a boot record");
        return DISCOVER_ID_BAD;
    }
    g_install = r;
    det("read: IoAlign ");
    det_hex(bio->Media->IoAlign, 8);
    det(", buffer ");
    det_hex((UINT64)(uintptr_t)buf, 16);
    det(g_low != (UINT8 *)0 ? ", through memory below 4 GiB" : "");
    det_path(st, boot);
    return DISCOVER_ID_OK;
}

const char *discover_install_detail(size_t *len)
{
    if (len != (size_t *)0)
        *len = g_detail_len;
    return g_detail;
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
    g_bs = st->BootServices;
    boot = boot_device(st, image);
    /* First, before any row is judged: every row is judged against it. */
    g_detail_len = 0;
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
    case DISCOVER_ID_READ:
        return L"The partition this loader started from would not read, so "
               L"no ISO\r\nlibrary or shelf is offered.";
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
