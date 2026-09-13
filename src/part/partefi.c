/* Firmware plumbing for partition identification: PartitionInfo, device path
 * walking, and the parent-disk GPT parse that stands in for both when the
 * firmware is too old to have either. See partefi.h for the shape and
 * docs/DESIGN.md Section 3 for why any of it exists.
 */

#include <part/partefi.h>
#include <efi/blockio.h>
#include <efi/devpath.h>
#include <efi/partinfo.h>
#include <lib/string.h>

/* A device path with more nodes than this is not something we are going to
 * find a partition on; the bound exists so a path missing its end node
 * cannot walk off into firmware memory. */
#define DP_NODE_LIMIT 64u

/* Largest partition table this loader will hold for one disk. A GPT is
 * conventionally 128 entries and ours uses four of them; a disk with more
 * used partitions than this still classifies its first 128, and the parser
 * flags the truncation. */
#define PART_MAX_ENTRIES 128u

/* ---- GPT-over-BlockIO plumbing ---- */

typedef struct {
    EFI_BLOCK_IO_PROTOCOL *bio;
} part_bio_ctx;

/* gpt_parse only ever asks for whole blocks at block-aligned offsets, so
 * this is a straight forward to ReadBlocks; anything else is a bug in the
 * parser rather than a device condition. */
static int part_bio_read(void *ctxv, uint64_t off, uint64_t len, void *buf)
{
    part_bio_ctx *ctx = ctxv;
    UINT32 bs = ctx->bio->Media->BlockSize;

    if (bs == 0 || off % bs != 0 || len % bs != 0)
        return -1;
    if (ctx->bio->ReadBlocks(ctx->bio, ctx->bio->Media->MediaId,
                             off / bs, len, buf) != EFI_SUCCESS)
        return -1;
    return 0;
}

/* ---- caches, all reset together by part_scan_reset ---- */

static gpt_part g_disk_parts[PART_MAX_ENTRIES];
static gpt_table g_disk_table = { g_disk_parts, PART_MAX_ENTRIES, 0, 0 };
static EFI_HANDLE g_disk_handle;             /* NULL: nothing parsed yet */
static int g_disk_rc = GPT_ERR_BAD_ARG;

static BOOLEAN g_present_known[PART_ROLE_COUNT];
static BOOLEAN g_present[PART_ROLE_COUNT];

void part_scan_reset(void)
{
    unsigned i;

    g_disk_handle = NULL;
    g_disk_rc = GPT_ERR_BAD_ARG;
    g_disk_table.count = 0;
    g_disk_table.truncated = 0;
    for (i = 0; i < PART_ROLE_COUNT; i++) {
        g_present_known[i] = FALSE;
        g_present[i] = FALSE;
    }
}

/* The parsed GPT of one whole disk, re-parsing only when the handle changes.
 * Firmware hands out a disk's partitions consecutively, so this single-entry
 * cache turns what looks like a per-partition parse into one per disk. */
static int disk_table(EFI_HANDLE disk, EFI_BLOCK_IO_PROTOCOL *bio,
                      const gpt_table **out)
{
    if (g_disk_handle != disk) {
        part_bio_ctx ctx;

        ctx.bio = bio;
        g_disk_handle = disk;
        g_disk_rc = gpt_parse(&g_disk_table, part_bio_read, &ctx,
                              bio->Media->BlockSize);
    }
    *out = &g_disk_table;
    return g_disk_rc;
}

/* ---- device path walking ---- */

/* The last HARDDRIVE node of a path, with *prefix_len set to the byte length
 * of everything before it, which is exactly the parent disk's path without
 * its end node. "Last" rather than "first" because a path may describe a
 * partition inside an image on another partition; the innermost node is the
 * one that describes this handle. */
static const HARDDRIVE_DEVICE_PATH *hd_node(const EFI_DEVICE_PATH_PROTOCOL *dp,
                                            UINTN *prefix_len)
{
    const UINT8 *base = (const UINT8 *)dp;
    const UINT8 *p = base;
    const HARDDRIVE_DEVICE_PATH *found = NULL;
    unsigned n;

    if (dp == NULL)
        return NULL;

    for (n = 0; n < DP_NODE_LIMIT; n++) {
        const EFI_DEVICE_PATH_PROTOCOL *node =
            (const EFI_DEVICE_PATH_PROTOCOL *)p;
        UINT16 len = dp_node_len(node);

        if (len < sizeof(*node))
            return NULL;
        if (dp_is_end(node))
            return found;
        if (node->Type == MEDIA_DEVICE_PATH &&
            node->SubType == MEDIA_HARDDRIVE_DP &&
            len >= sizeof(HARDDRIVE_DEVICE_PATH)) {
            found = (const HARDDRIVE_DEVICE_PATH *)node;
            *prefix_len = (UINTN)(p - base);
        }
        p += len;
    }
    return NULL;
}

/* Is `disk` the path of the device `part`'s HARDDRIVE node sits on? True
 * when the disk's path, minus its end node, is byte-identical to the
 * partition path's prefix. Matching on the starting LBA alone would not do:
 * every disk in the machine has a partition starting at LBA 2048. */
static BOOLEAN dp_is_parent(const EFI_DEVICE_PATH_PROTOCOL *disk,
                            const EFI_DEVICE_PATH_PROTOCOL *part,
                            UINTN prefix_len)
{
    UINTN dlen = dp_total_len(disk);

    if (dlen <= sizeof(EFI_DEVICE_PATH_PROTOCOL))
        return FALSE;
    dlen -= sizeof(EFI_DEVICE_PATH_PROTOCOL);
    if (dlen != prefix_len)
        return FALSE;
    return (BOOLEAN)(memcmp(disk, part, prefix_len) == 0);
}

/* ---- route 1: the PartitionInfo protocol ---- */

static int type_from_partinfo(EFI_SYSTEM_TABLE *st, EFI_HANDLE h,
                              EFI_GUID *out)
{
    EFI_GUID pi_guid = EFI_PARTITION_INFO_PROTOCOL_GUID;
    EFI_PARTITION_INFO_PROTOCOL *pi = NULL;
    const UINT8 *raw;

    if (st->BootServices->HandleProtocol(h, &pi_guid,
                                         (void **)&pi) != EFI_SUCCESS)
        return PART_UNKNOWN;
    if (pi == NULL || pi->Type != PARTITION_TYPE_GPT)
        return PART_UNKNOWN;

    /* The protocol structure is packed, so the GUID is copied from a computed
     * byte offset rather than through &pi->Info.Gpt.PartitionTypeGUID: taking
     * the address of a packed member is what the compiler rightly objects to,
     * and the offset is the same ABI either way. */
    raw = (const UINT8 *)pi;
    memcpy(out, raw + offsetof(EFI_PARTITION_INFO_PROTOCOL, Info),
           GPT_GUID_LEN);
    return PART_OK;
}

/* ---- route 2: HARDDRIVE node plus the parent disk's GPT ---- */

static int type_from_parent_gpt(EFI_SYSTEM_TABLE *st, EFI_HANDLE h,
                                EFI_GUID *out)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
    const HARDDRIVE_DEVICE_PATH *hd;
    EFI_HANDLE *handles = NULL;
    UINTN prefix_len = 0;
    UINTN count = 0;
    UINTN i;
    UINT64 start;
    int rc = PART_UNKNOWN;

    if (st->BootServices->HandleProtocol(h, &dp_guid,
                                         (void **)&dp) != EFI_SUCCESS)
        return PART_NOT_PARTITION;
    hd = hd_node(dp, &prefix_len);
    if (hd == NULL)
        return PART_NOT_PARTITION;
    /* An MBR-signed node is a partition of an MBR disk: no GPT type GUID
     * exists for it, so there is nothing to go looking for. */
    if (hd->SignatureType != SIGNATURE_TYPE_GUID)
        return PART_NOT_PARTITION;
    start = hd->PartitionStart;

    if (st->BootServices->LocateHandleBuffer(ByProtocol, &bio_guid, NULL,
                                             &count, &handles) != EFI_SUCCESS)
        return PART_UNKNOWN;

    for (i = 0; i < count && rc != PART_OK; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        EFI_DEVICE_PATH_PROTOCOL *ddp = NULL;
        const gpt_table *tbl = NULL;
        const gpt_part *p;

        if (handles[i] == h)
            continue;
        if (st->BootServices->HandleProtocol(handles[i], &bio_guid,
                                             (void **)&bio) != EFI_SUCCESS)
            continue;
        /* Only whole disks carry a GPT, and only they can be a parent. */
        if (bio->Media->LogicalPartition || !bio->Media->MediaPresent)
            continue;
        if (st->BootServices->HandleProtocol(handles[i], &dp_guid,
                                             (void **)&ddp) != EFI_SUCCESS)
            continue;
        if (!dp_is_parent(ddp, dp, prefix_len))
            continue;
        if (disk_table(handles[i], bio, &tbl) != GPT_OK)
            continue;
        p = gpt_find_start(tbl, start);
        if (p == NULL)
            continue;
        memcpy(out, p->type, GPT_GUID_LEN);
        rc = PART_OK;
    }

    st->BootServices->FreePool(handles);
    return rc;
}

/* ---- public API ---- */

int part_type_guid(EFI_SYSTEM_TABLE *st, EFI_HANDLE h, EFI_GUID *out)
{
    int rc;

    if (st == NULL || h == NULL || out == NULL)
        return PART_UNKNOWN;

    rc = type_from_partinfo(st, h, out);
    if (rc == PART_OK)
        return rc;
    /* PartitionInfo absent, or present and describing something other than a
     * GPT partition. Either way the device path still knows, and on an MBR
     * disk it is route 2 that says so authoritatively. */
    return type_from_parent_gpt(st, h, out);
}

static BOOLEAN is_role(EFI_SYSTEM_TABLE *st, EFI_HANDLE h, part_role role)
{
    const uint8_t *want = part_role_guid(role);
    EFI_GUID got;

    if (want == NULL)
        return FALSE;
    if (part_type_guid(st, h, &got) != PART_OK)
        return FALSE;
    /* EFI_GUID and the on-disk entry share one byte layout, which is the
     * whole reason the constants in part.h are byte arrays. */
    return (BOOLEAN)gpt_guid_eq((const uint8_t *)&got, want);
}

BOOLEAN part_is_airies_iso(EFI_SYSTEM_TABLE *st, EFI_HANDLE h)
{
    return is_role(st, h, PART_ROLE_ISO);
}

BOOLEAN part_is_airies_shelf(EFI_SYSTEM_TABLE *st, EFI_HANDLE h)
{
    return is_role(st, h, PART_ROLE_SHELF);
}

BOOLEAN part_role_present(EFI_SYSTEM_TABLE *st, part_role role)
{
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    UINTN i;
    unsigned slot = (unsigned)role;
    BOOLEAN found = FALSE;

    if (st == NULL || slot >= PART_ROLE_COUNT)
        return FALSE;
    if (g_present_known[slot])
        return g_present[slot];

    if (st->BootServices->LocateHandleBuffer(ByProtocol, &bio_guid, NULL,
                                             &count, &handles) == EFI_SUCCESS) {
        for (i = 0; i < count && !found; i++) {
            if (is_role(st, handles[i], role))
                found = TRUE;
        }
        st->BootServices->FreePool(handles);
    }

    g_present[slot] = found;
    g_present_known[slot] = TRUE;
    return found;
}

BOOLEAN part_scan_accept(EFI_SYSTEM_TABLE *st, EFI_HANDLE h, part_role role)
{
    if (st == NULL || (unsigned)role >= PART_ROLE_COUNT)
        return TRUE;
    /* No airies-typed partition in the machine: nothing to prefer, so the
     * probe-everything behaviour the rig images rely on stands. */
    if (!part_role_present(st, role))
        return TRUE;
    return is_role(st, h, role);
}
