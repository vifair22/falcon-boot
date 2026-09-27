/* Partition naming over the inventory's whole disks. See disks.h. */

#include <discover/disks.h>

#include <discover/discover.h>
#include <discover/partefi.h>
#include <lib/string.h>

/* A machine with more disks than this is a server; the partitions of the
 * ones past it go unnamed, which costs an INI section naming them its match
 * and nothing else. */
#define DISKS_MAX 8u
/* Entries kept per disk. The table is read whole (PART_READ_MAX) and the
 * first DISKS_PARTS kept, which covers every Windows and Linux layout. */
#define DISKS_PARTS   32u
#define PART_READ_MAX 128u

typedef struct {
    EFI_HANDLE   handle;
    part_disk_id id;
    UINT32       n;
    disks_part   part[DISKS_PARTS];
} disk_row;

static disk_row g_disk[DISKS_MAX];
static UINTN g_disk_n;
/* Scratch for one read; static, being 24 KB. */
static part_entry g_entries[PART_READ_MAX];

UINTN disks_scan(EFI_SYSTEM_TABLE *st)
{
    UINTN i;

    (void)st;
    g_disk_n = 0;
    for (i = 0; i < discover_count() && g_disk_n < DISKS_MAX; i++) {
        discover_dev *d = discover_at(i);
        disk_row *r = &g_disk[g_disk_n];
        part_table t;
        UINT32 k;

        if (d == (discover_dev *)0 || !d->media_present ||
            d->bio->Media->LogicalPartition)
            continue;
        memset(&t, 0, sizeof t);
        t.parts = g_entries;
        t.cap = PART_READ_MAX;
        if (part_read_table(&t, discover_read, d, d->bio->Media->BlockSize,
                            (uint64_t)d->bio->Media->LastBlock + 1u) !=
            PART_OK)
            continue;
        r->handle = d->handle;
        r->id = t.id;
        r->n = 0;
        for (k = 0; k < t.count && r->n < DISKS_PARTS; k++) {
            disks_part *p = &r->part[r->n++];

            p->disk = t.id;
            p->number = g_entries[k].number;
            p->start_lba = g_entries[k].start_lba;
            memcpy(p->unique, g_entries[k].unique, GPT_GUID_LEN);
        }
        g_disk_n++;
    }
    return g_disk_n;
}

EFI_STATUS disks_part_of(EFI_SYSTEM_TABLE *st, EFI_HANDLE h, disks_part *out)
{
    UINT64 start = 0;
    UINTN i;
    UINT32 k;

    if (st == (EFI_SYSTEM_TABLE *)0 || out == (disks_part *)0 ||
        part_start_lba(st, h, &start) != EFI_SUCCESS)
        return EFI_NOT_FOUND;
    for (i = 0; i < g_disk_n; i++) {
        if (!part_on_disk(st, g_disk[i].handle, h))
            continue;
        for (k = 0; k < g_disk[i].n; k++) {
            if (g_disk[i].part[k].start_lba == start) {
                *out = g_disk[i].part[k];
                return EFI_SUCCESS;
            }
        }
    }
    return EFI_NOT_FOUND;
}

EFI_STATUS disks_part_by_node(UINT8 sig_type, const UINT8 sig[16],
                              UINT64 start, disks_part *out)
{
    UINTN i;
    UINT32 k;

    if (sig == (const UINT8 *)0 || out == (disks_part *)0)
        return EFI_INVALID_PARAMETER;
    for (i = 0; i < g_disk_n; i++) {
        const disk_row *r = &g_disk[i];
        UINT32 mbr_sig = (UINT32)sig[0] | ((UINT32)sig[1] << 8) |
                         ((UINT32)sig[2] << 16) | ((UINT32)sig[3] << 24);

        if (sig_type == 1u &&
            (r->id.kind != PART_KIND_MBR || r->id.mbr_sig == 0 ||
             r->id.mbr_sig != mbr_sig))
            continue;
        if (sig_type == 2u && r->id.kind != PART_KIND_GPT)
            continue;
        if (sig_type != 1u && sig_type != 2u)
            return EFI_NOT_FOUND;
        for (k = 0; k < r->n; k++) {
            const disks_part *p = &r->part[k];

            if ((sig_type == 2u &&
                 memcmp(p->unique, sig, GPT_GUID_LEN) == 0) ||
                (sig_type == 1u && p->start_lba == start)) {
                *out = *p;
                return EFI_SUCCESS;
            }
        }
    }
    return EFI_NOT_FOUND;
}
