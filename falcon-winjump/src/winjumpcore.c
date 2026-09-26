/* The pure half of winjump's driver-mount fallback. See winjumpcore.h. */

#include "winjumpcore.h"

#include <lib/string.h>

/* GPT on-disk layout, little-endian. Header at LBA 1; entries where it says. */
#define GPT_SIG          "EFI PART"
#define GPT_SIG_LEN      8
#define GPT_OFF_ENTRIES_LBA   72   /* u64: first LBA of the entry array */
#define GPT_OFF_ENTRY_COUNT   80   /* u32: number of entries */
#define GPT_OFF_ENTRY_SIZE    84   /* u32: bytes per entry */
#define GPT_ENT_OFF_UNIQUE    16   /* 16 bytes: unique partition GUID */
#define GPT_ENT_OFF_FIRST_LBA 32   /* u64 */

int wj_gpt_match(const uint8_t *hdr_sector, const uint8_t *entries,
                 size_t entries_len, const uint8_t part_guid[16],
                 uint64_t *first_lba)
{
    uint32_t count, size, i;

    if (hdr_sector == (const uint8_t *)0 || entries == (const uint8_t *)0 ||
        part_guid == (const uint8_t *)0 || first_lba == (uint64_t *)0)
        return WJ_ERR_BAD_ARG;
    if (memcmp(hdr_sector, GPT_SIG, GPT_SIG_LEN) != 0)
        return WJ_ERR_FORMAT;

    count = get_le32(hdr_sector + GPT_OFF_ENTRY_COUNT);
    size = get_le32(hdr_sector + GPT_OFF_ENTRY_SIZE);
    /* A GPT entry is 128 bytes; a size below that or not covering the GUID
     * fields is a table this does not read. */
    if (size < 128)
        return WJ_ERR_FORMAT;

    for (i = 0; i < count; i++) {
        const uint8_t *e = entries + (size_t)i * size;

        if ((size_t)(i + 1) * size > entries_len)
            break;
        if (memcmp(e + GPT_ENT_OFF_UNIQUE, part_guid, 16) == 0) {
            *first_lba = get_le64(e + GPT_ENT_OFF_FIRST_LBA);
            return WJ_OK;
        }
    }
    return WJ_ERR_NOMATCH;
}

int wj_backing_extents(const extmap_hdr *hdr, const uint8_t *map, size_t map_len,
                       wj_extent *out, uint32_t cap, uint32_t *n)
{
    uint64_t part_base, iso_off = 0;
    uint32_t i;

    if (hdr == (const extmap_hdr *)0 || map == (const uint8_t *)0 ||
        out == (wj_extent *)0 || n == (uint32_t *)0)
        return WJ_ERR_BAD_ARG;
    if (map_len < extmap_size(hdr->n_extents))
        return WJ_ERR_FORMAT;
    if (hdr->n_extents > cap)
        return WJ_ERR_NOSPACE;

    part_base = hdr->part_lba * hdr->block_size;
    for (i = 0; i < hdr->n_extents; i++) {
        extmap_extent e;

        extmap_extent_at(map, i, &e);
        out[i].iso_offset = iso_off;
        out[i].disk_offset = part_base + e.offset;
        out[i].length = e.length;
        iso_off += e.length;
    }
    *n = hdr->n_extents;
    return WJ_OK;
}
