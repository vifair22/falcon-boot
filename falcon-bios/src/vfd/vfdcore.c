/* falcon-bios: floppy-emulation El Torito's host-tested half. See vfdcore.h.
 */

#include <vfd/vfdcore.h>

int vfd_geometry(uint8_t media, uint64_t image_bytes, vfd_geom *g)
{
    uint8_t sectors;
    uint8_t bios_type;

    if (g == (vfd_geom *)0)
        return VFD_ERR_BAD_ARG;

    /* Every standard floppy is 80 cylinders and 2 heads; the media type
     * fixes the sectors per track, and with it the size. The BIOS drive
     * type is what AH=08h and AH=15h report: 1 is a 360K/1.2M 5.25", 3 a
     * 1.44M 3.5", 4 a 2.88M 3.5". */
    switch (media) {
    case VFD_MEDIA_120M:
        sectors = 15u;
        bios_type = 1u;
        break;
    case VFD_MEDIA_144M:
        sectors = 18u;
        bios_type = 3u;
        break;
    case VFD_MEDIA_288M:
        sectors = 36u;
        bios_type = 4u;
        break;
    default:
        return VFD_ERR_MEDIA;
    }

    g->cylinders = 80u;
    g->heads = 2u;
    g->sectors = sectors;
    g->total_sectors = 80u * 2u * (uint32_t)sectors;
    g->media = media;
    g->bios_type = bios_type;

    /* The image is loaded whole and padded to the media size, so it may be
     * shorter but not longer than what the geometry addresses. */
    if (image_bytes > (uint64_t)g->total_sectors * VFD_SECTOR)
        return VFD_ERR_SIZE;
    return VFD_OK;
}

int vfd_chs_to_lba(const vfd_geom *g, uint16_t cyl, uint8_t head,
                   uint8_t sector, uint32_t *lba)
{
    if (g == (const vfd_geom *)0 || lba == (uint32_t *)0)
        return VFD_ERR_BAD_ARG;
    /* The sector number is one-based; cylinder and head are zero-based. A
     * head or sector out of range is a caller error the handler reports, not
     * something to wrap into a wrong sector. */
    if (cyl >= g->cylinders || head >= g->heads ||
        sector < 1u || sector > g->sectors)
        return VFD_ERR_RANGE;
    *lba = ((uint32_t)cyl * g->heads + head) * g->sectors +
           ((uint32_t)sector - 1u);
    return VFD_OK;
}

int vfd_run_on_track(const vfd_geom *g, uint32_t lba, uint32_t want,
                     uint32_t *n)
{
    uint32_t into_track;
    uint32_t left;

    if (g == (const vfd_geom *)0 || n == (uint32_t *)0 || want == 0u)
        return VFD_ERR_BAD_ARG;
    if (lba >= g->total_sectors)
        return VFD_ERR_RANGE;
    into_track = lba % g->sectors;
    left = (uint32_t)g->sectors - into_track;
    *n = want < left ? want : left;
    if (lba + *n > g->total_sectors)
        *n = g->total_sectors - lba;
    return VFD_OK;
}

int vfd_lba_to_chs(const vfd_geom *g, uint32_t lba, uint16_t *cyl,
                   uint8_t *head, uint8_t *sector)
{
    uint32_t track;

    if (g == (const vfd_geom *)0 || cyl == (uint16_t *)0 ||
        head == (uint8_t *)0 || sector == (uint8_t *)0 ||
        g->heads == 0u || g->sectors == 0u)
        return VFD_ERR_BAD_ARG;
    if (lba >= g->total_sectors)
        return VFD_ERR_RANGE;
    track = lba / g->sectors;
    *cyl = (uint16_t)(track / g->heads);
    *head = (uint8_t)(track % g->heads);
    *sector = (uint8_t)(lba % g->sectors + 1u);
    return VFD_OK;
}

int vfd_geometry_int13(uint8_t ch, uint8_t cl, uint8_t dh, vfd_geom *g)
{
    uint32_t cylinders;

    if (g == (vfd_geom *)0)
        return VFD_ERR_BAD_ARG;
    cylinders = ((uint32_t)ch | (((uint32_t)cl & 0xC0u) << 2)) + 1u;
    /* DH is the last head's index, so 0xFF would be 256 heads, which the
     * geometry's byte cannot hold; no drive reports it in practice. */
    if ((cl & 0x3Fu) == 0u || dh == 0xFFu)
        return VFD_ERR_RANGE;
    g->cylinders = (uint16_t)cylinders;
    g->heads = (uint8_t)(dh + 1u);
    g->sectors = (uint8_t)(cl & 0x3Fu);
    g->total_sectors = cylinders * g->heads * g->sectors;
    g->media = 0;
    g->bios_type = 0;
    return VFD_OK;
}
