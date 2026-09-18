/* The ISO extent map. See falcon-core/src/media/extmap.h for the layout and
 * for why it travels in an EFI variable. */

#include <media/extmap.h>

#include <lib/string.h>

static const char g_magic[8] = { 'F', 'L', 'C', 'N', 'X', 'T', '0', '1' };

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static int block_size_ok(uint32_t bs)
{
    return bs == 512u || bs == 1024u || bs == 2048u || bs == 4096u;
}

size_t extmap_size(uint32_t n_extents)
{
    return EXTMAP_HDR_LEN + (size_t)n_extents * EXTMAP_EXT_LEN;
}

int extmap_encode(const extmap_hdr *hdr, const extmap_extent *ext,
                  uint8_t *buf, size_t cap, size_t *len)
{
    uint64_t sum = 0;
    uint32_t i;
    size_t need;

    if (hdr == (const extmap_hdr *)0 || ext == (const extmap_extent *)0 ||
        buf == (uint8_t *)0 || len == (size_t *)0)
        return EXTMAP_ERR_BAD_ARG;
    *len = 0;
    if (hdr->n_extents == 0 || hdr->n_extents > EXTMAP_MAX_EXTENTS ||
        !block_size_ok(hdr->block_size))
        return EXTMAP_ERR_RANGE;
    for (i = 0; i < hdr->n_extents; i++) {
        if (ext[i].length == 0 || ext[i].length > ~sum)
            return EXTMAP_ERR_RANGE;
        sum += ext[i].length;
    }
    if (sum != hdr->iso_size)
        return EXTMAP_ERR_RANGE;
    need = extmap_size(hdr->n_extents);
    if (cap < need)
        return EXTMAP_ERR_NOSPACE;

    memset(buf, 0, EXTMAP_HDR_LEN);
    memcpy(buf, g_magic, sizeof g_magic);
    put32(buf + 8, 1);
    put32(buf + 12, EXTMAP_HDR_LEN);
    memcpy(buf + 16, hdr->part_guid, 16);
    put64(buf + 32, hdr->part_lba);
    put32(buf + 40, hdr->block_size);
    put32(buf + 44, hdr->n_extents);
    put64(buf + 48, hdr->iso_size);
    for (i = 0; i < hdr->n_extents; i++) {
        uint8_t *e = buf + EXTMAP_HDR_LEN + (size_t)i * EXTMAP_EXT_LEN;

        put64(e, ext[i].offset);
        put64(e + 8, ext[i].length);
    }
    *len = need;
    return EXTMAP_OK;
}

int extmap_decode(const uint8_t *buf, size_t len, extmap_hdr *hdr)
{
    uint64_t sum = 0;
    uint32_t i;

    if (buf == (const uint8_t *)0 || hdr == (extmap_hdr *)0)
        return EXTMAP_ERR_BAD_ARG;
    if (len < EXTMAP_HDR_LEN || memcmp(buf, g_magic, sizeof g_magic) != 0 ||
        get_le32(buf + 8) != 1u || get_le32(buf + 12) != EXTMAP_HDR_LEN)
        return EXTMAP_ERR_FORMAT;
    memcpy(hdr->part_guid, buf + 16, 16);
    hdr->part_lba = get_le64(buf + 32);
    hdr->block_size = get_le32(buf + 40);
    hdr->n_extents = get_le32(buf + 44);
    hdr->iso_size = get_le64(buf + 48);
    if (!block_size_ok(hdr->block_size) || hdr->n_extents == 0 ||
        hdr->n_extents > EXTMAP_MAX_EXTENTS ||
        len < extmap_size(hdr->n_extents))
        return EXTMAP_ERR_FORMAT;
    for (i = 0; i < hdr->n_extents; i++) {
        extmap_extent e;

        extmap_extent_at(buf, i, &e);
        if (e.length == 0 || e.length > ~sum || e.offset > ~e.length)
            return EXTMAP_ERR_RANGE;
        sum += e.length;
    }
    return sum == hdr->iso_size ? EXTMAP_OK : EXTMAP_ERR_RANGE;
}

void extmap_extent_at(const uint8_t *buf, uint32_t i, extmap_extent *out)
{
    const uint8_t *e = buf + EXTMAP_HDR_LEN + (size_t)i * EXTMAP_EXT_LEN;

    out->offset = get_le64(e);
    out->length = get_le64(e + 8);
}
