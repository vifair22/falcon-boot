/* falcon jump's ATA.MOD: the ATAPI command blocks and answers. See
 * atacore.h. */

#include <hw/ata/atacore.h>

int atapi_parse_capacity(const uint8_t data[8], uint64_t *sectors)
{
    uint32_t block;

    if (scsi_parse_capacity(data, sectors, &block) != 0 ||
        block != ATAPI_SECTOR)
        return -1;
    return 0;
}

/* Words 27-46, forty characters, each word's bytes the wrong way round for
 * a string: the model, trimmed, into `model` of `cap` bytes. */
static void model_string(const uint16_t words[256], char *model, size_t cap)
{
    size_t n = 0;
    size_t i;

    if (model == (char *)0 || cap == 0)
        return;
    for (i = 27; i <= 46 && n + 2u < cap; i++) {
        model[n++] = (char)(words[i] >> 8);
        model[n++] = (char)(words[i] & 0xFFu);
    }
    while (n > 0 && (model[n - 1] == ' ' || model[n - 1] == '\0'))
        n--;
    model[n] = '\0';
    for (i = 0; i < n; i++) {
        if (model[i] < ' ' || model[i] > '~')
            model[i] = '?';
    }
}

int atapi_parse_identify(const uint16_t words[256], char *model, size_t cap)
{
    uint16_t cfg = words[0];

    model_string(words, model, cap);
    return (cfg & 0xC000u) == 0x8000u && ((cfg >> 8) & 0x1Fu) == 5u;
}

int ata_parse_identify_disk(const uint16_t words[256], uint64_t *sectors,
                            char *model, size_t cap)
{
    uint32_t n;

    /* Word 0's top two bits 10b mark a packet device, except 0x848A, the
     * signature a CompactFlash card gives, which is an ATA device. */
    if (sectors == (uint64_t *)0 ||
        ((words[0] & 0xC000u) == 0x8000u && words[0] != 0x848Au))
        return -1;
    if ((words[49] & (1u << 9)) == 0)
        return -1;                   /* no LBA: CHS only, not handled */
    n = (uint32_t)words[60] | ((uint32_t)words[61] << 16);
    if (n == 0)
        return -1;
    *sectors = n;
    model_string(words, model, cap);
    return 0;
}
