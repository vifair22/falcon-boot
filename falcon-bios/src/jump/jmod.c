/* falcon jump: checking a module before it runs. See jmod.h. */

#include <jump/jmod.h>

#include <start/biosdefs.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

jmod_status jmod_check(const uint8_t *hdr, size_t have, uint64_t file_len,
                       uint32_t slot, uint32_t slot_bytes, jmod_info *out)
{
    uint32_t image_end;
    uint32_t mem_end;
    uint32_t init;
    size_t i;

    if (hdr == (const uint8_t *)0 || out == (jmod_info *)0 ||
        slot_bytes <= JMOD_STACK_BYTES)
        return JMOD_ERR_ARG;
    if (have < JMOD_HDR_LEN || file_len < JMOD_HDR_LEN ||
        rd32(hdr + JMOD_OFF_MAGIC) != JMOD_MAGIC)
        return JMOD_ERR_MAGIC;
    if (rd16(hdr + JMOD_OFF_ABI) != JMOD_ABI ||
        rd16(hdr + JMOD_OFF_HDR_LEN) != JMOD_HDR_LEN)
        return JMOD_ERR_ABI;
    if (rd32(hdr + JMOD_OFF_SLOT) != slot)
        return JMOD_ERR_SLOT;

    image_end = rd32(hdr + JMOD_OFF_IMAGE_END);
    mem_end = rd32(hdr + JMOD_OFF_MEM_END);
    init = rd32(hdr + JMOD_OFF_INIT);
    /* The file is exactly the image, the image sits inside what it uses in
     * memory, and that leaves the slot's top for the module's stack. */
    if (image_end < slot || image_end - slot != file_len ||
        mem_end < image_end ||
        mem_end - slot > slot_bytes - JMOD_STACK_BYTES)
        return JMOD_ERR_SIZE;
    if (init < slot + JMOD_HDR_LEN || init >= image_end)
        return JMOD_ERR_ENTRY;

    out->slot = slot;
    out->image_len = image_end - slot;
    out->mem_len = mem_end - slot;
    out->init = init;
    for (i = 0; i < 8u; i++) {
        char c = (char)hdr[JMOD_OFF_NAME + i];

        out->name[i] = (c >= ' ' && c <= '~') ? c : '\0';
    }
    out->name[8] = '\0';
    return JMOD_OK;
}
