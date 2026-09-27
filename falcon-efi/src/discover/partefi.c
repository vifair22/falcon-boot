/* Device-path plumbing for partition handles. See partefi.h.
 */

#include <discover/partefi.h>
#include <efi/devpath.h>
#include <lib/string.h>

/* A device path with more nodes than this is not something we are going to
 * find a partition on; the bound exists so a path missing its end node
 * cannot walk off into firmware memory. */
#define DP_NODE_LIMIT 64u

/* The last HARDDRIVE node of a path, with *prefix_len set to the byte length
 * of everything before it, which is exactly the parent disk's path without
 * its end node. "Last" rather than "first" because a path may describe a
 * partition inside an image on another partition; the innermost node is the
 * one that describes this handle. */
static const UINT8 *hd_node(const EFI_DEVICE_PATH_PROTOCOL *dp,
                            UINTN *prefix_len)
{
    const UINT8 *base = (const UINT8 *)dp;
    const UINT8 *p = base;
    const UINT8 *found = NULL;
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
            node->SubType == MEDIA_HARDDRIVE_DP && len >= HD_DP_LEN) {
            found = p;
            *prefix_len = (UINTN)(p - base);
        }
        p += len;
    }
    return NULL;
}

static EFI_DEVICE_PATH_PROTOCOL *path_of(EFI_SYSTEM_TABLE *st, EFI_HANDLE h)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;

    if (st->BootServices->HandleProtocol(h, &dp_guid, (void **)&dp) !=
        EFI_SUCCESS)
        return NULL;
    return dp;
}

BOOLEAN part_same_disk(EFI_SYSTEM_TABLE *st, EFI_HANDLE a, EFI_HANDLE b)
{
    EFI_DEVICE_PATH_PROTOCOL *da;
    EFI_DEVICE_PATH_PROTOCOL *db;
    UINTN la = 0;
    UINTN lb = 0;

    if (st == NULL || a == NULL || b == NULL)
        return FALSE;
    da = path_of(st, a);
    db = path_of(st, b);
    if (hd_node(da, &la) == NULL || hd_node(db, &lb) == NULL)
        return FALSE;
    /* A zero-length prefix is a partition node with no disk in front of
     * it, which says nothing about where it is. */
    if (la == 0 || la != lb)
        return FALSE;
    return (BOOLEAN)(memcmp(da, db, la) == 0);
}

EFI_STATUS part_start_lba(EFI_SYSTEM_TABLE *st, EFI_HANDLE h, UINT64 *lba)
{
    const UINT8 *hd;
    UINTN prefix = 0;

    if (st == NULL || h == NULL || lba == NULL)
        return EFI_INVALID_PARAMETER;
    hd = hd_node(path_of(st, h), &prefix);
    if (hd == NULL)
        return EFI_NOT_FOUND;
    /* The node is byte-packed; the field is copied, never dereferenced. */
    memcpy(lba, hd + HD_DP_OFF_START, sizeof *lba);
    return EFI_SUCCESS;
}

BOOLEAN part_on_disk(EFI_SYSTEM_TABLE *st, EFI_HANDLE disk, EFI_HANDLE part)
{
    EFI_DEVICE_PATH_PROTOCOL *dd;
    EFI_DEVICE_PATH_PROTOCOL *dp;
    UINTN lp = 0;
    UINTN ld;

    if (st == NULL || disk == NULL || part == NULL)
        return FALSE;
    dd = path_of(st, disk);
    dp = path_of(st, part);
    if (dd == NULL || hd_node(dp, &lp) == NULL || lp == 0)
        return FALSE;
    ld = dp_total_len(dd);
    if (ld < sizeof(EFI_DEVICE_PATH_PROTOCOL))
        return FALSE;
    ld -= sizeof(EFI_DEVICE_PATH_PROTOCOL);
    return (BOOLEAN)(ld == lp && memcmp(dd, dp, lp) == 0);
}
