/* falcon-bios: the x86 boot protocol's arithmetic (linuxcore.h).
 */

#include <linux/linuxcore.h>

static uint8_t rd8(const uint8_t *p, unsigned off)
{
    return p[off];
}

static uint16_t rd16(const uint8_t *p, unsigned off)
{
    return (uint16_t)((uint16_t)p[off] | ((uint16_t)p[off + 1u] << 8));
}

static uint32_t rd32(const uint8_t *p, unsigned off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1u] << 8) |
           ((uint32_t)p[off + 2u] << 16) | ((uint32_t)p[off + 3u] << 24);
}

static uint64_t rd64(const uint8_t *p, unsigned off)
{
    return (uint64_t)rd32(p, off) | ((uint64_t)rd32(p, off + 4u) << 32);
}

uint64_t linuxcore_align4(uint64_t v)
{
    return (v + 3u) & ~(uint64_t)3u;
}

uint64_t linuxcore_initrd_len(const uint64_t *sizes, unsigned n)
{
    uint64_t total = 0;
    unsigned i;

    if (sizes == (const uint64_t *)0)
        return 0;
    for (i = 0; i < n; i++)
        total += linuxcore_align4(sizes[i]);
    return total;
}

uint64_t linuxcore_initrd_at(const uint64_t *sizes, unsigned n, unsigned i,
                             uint64_t *pad_out)
{
    uint64_t at = 0;
    unsigned k;

    if (pad_out != (uint64_t *)0)
        *pad_out = 0;
    if (sizes == (const uint64_t *)0 || i >= n)
        return 0;
    for (k = 0; k < i; k++)
        at += linuxcore_align4(sizes[k]);
    if (pad_out != (uint64_t *)0)
        *pad_out = linuxcore_align4(sizes[i]) - sizes[i];
    return at;
}

linuxcore_status linuxcore_geometry(const uint8_t *setup, size_t setup_len,
                                    uint64_t file_len, uint64_t pref_load,
                                    uint64_t map_end, uint64_t setup_cap,
                                    linux_geom *out)
{
    uint64_t sects;
    uint64_t align;

    if (setup == (const uint8_t *)0 || out == (linux_geom *)0)
        return LINUXCORE_ERR_ARG;
    /* 1024, not 512: see HDR_HEADER in the header. */
    if (setup_len < 1024u || file_len < 1024u)
        return LINUXCORE_ERR_TOO_SMALL;

    if (rd16(setup, HDR_BOOT_FLAG) != 0xAA55u ||
        rd32(setup, HDR_HEADER) != HDRS_MAGIC)
        return LINUXCORE_ERR_NOT_BZIMAGE;

    out->version = rd16(setup, HDR_VERSION);
    if (out->version < LINUXCORE_MIN_VERSION)
        return LINUXCORE_ERR_VERSION;
    /* Either width boots through the 32-bit entry. Which one this is
     * decides the falcon-hook that rides along, and whether the processor
     * can run it at all; both are the caller's to act on. */
    out->is_64 = (rd16(setup, HDR_XLOADFLAGS) & XLF_KERNEL_64) != 0;

    sects = rd8(setup, HDR_SETUP_SECTS);
    if (sects == 0)
        sects = 4;                  /* what 0 has always meant */
    out->setup_len = (sects + 1u) * 512u;
    if (out->setup_len > setup_cap)
        return LINUXCORE_ERR_SETUP_LONG;
    if (file_len <= out->setup_len)
        return LINUXCORE_ERR_NO_KERNEL;
    out->pm_len = file_len - out->setup_len;

    /* A relocatable kernel takes any address aligned to kernel_alignment;
     * one that is not takes its preferred address and nothing else. */
    align = rd32(setup, HDR_KERNEL_ALIGNMENT);
    if (align == 0)
        align = 0x200000u;
    out->stage_at = 0;
    if (rd8(setup, HDR_RELOCATABLE) == 0) {
        out->load_at = rd64(setup, HDR_PREF_ADDRESS);
        if (out->load_at == 0)
            return LINUXCORE_ERR_NOT_RELOC;
        if (out->load_at < LINUXCORE_LOW_MIN)
            return LINUXCORE_ERR_TOO_LOW;
    } else {
        out->load_at = (pref_load + align - 1u) & ~(align - 1u);
    }

    /* init_size is what the kernel needs to decompress and run in, which is
     * larger than the image; the loader's address space has to hold all
     * of it. */
    out->init_size = rd32(setup, HDR_INIT_SIZE);
    if (out->init_size < out->pm_len)
        out->init_size = out->pm_len;
    if (out->load_at > map_end || map_end - out->load_at < out->init_size)
        return LINUXCORE_ERR_NO_ROOM;
    out->end = out->load_at + out->init_size;

    /* The classic fixed address is 1 MiB, which is where stage 3 is running:
     * reading there would overwrite the code doing the reading. So it is
     * read above both the loader and the kernel's own footprint, and moved
     * down last. The move runs downward from a higher source, so the two
     * may not overlap in the other direction, which this rules out. */
    if (out->load_at < pref_load) {
        uint64_t at = pref_load > out->end ? pref_load : out->end;

        at = (at + 0xFFFu) & ~(uint64_t)0xFFFu;
        if (at > map_end || map_end - at < out->pm_len)
            return LINUXCORE_ERR_NO_ROOM;
        out->stage_at = at;
        out->end = at + out->pm_len;
    }

    out->cmdline_cap = rd32(setup, HDR_CMDLINE_SIZE);
    if (out->cmdline_cap == 0 || out->cmdline_cap > 4096u)
        out->cmdline_cap = 4096u;

    out->initrd_max = rd32(setup, HDR_INITRD_ADDR_MAX);
    if (out->initrd_max == 0)
        out->initrd_max = 0x37FFFFFFu;
    if (out->initrd_max >= map_end)
        out->initrd_max = map_end - 1u;
    return LINUXCORE_OK;
}
