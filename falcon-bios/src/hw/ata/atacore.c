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
                            int *lba48, char *model, size_t cap)
{
    uint64_t n;
    int big;

    /* Word 0's top two bits 10b mark a packet device, except 0x848A, the
     * signature a CompactFlash card gives, which is an ATA device. */
    if (sectors == (uint64_t *)0 || lba48 == (int *)0 ||
        ((words[0] & 0xC000u) == 0x8000u && words[0] != 0x848Au))
        return -1;
    if ((words[49] & (1u << 9)) == 0)
        return -1;                   /* no LBA: CHS only, not handled */
    big = (words[83] & 0xC000u) == 0x4000u && (words[83] & (1u << 10)) != 0 &&
          (words[86] & (1u << 10)) != 0;
    n = (uint64_t)words[60] | ((uint64_t)words[61] << 16);
    if (big) {
        uint64_t n48 = (uint64_t)words[100] | ((uint64_t)words[101] << 16) |
                       ((uint64_t)words[102] << 32) |
                       ((uint64_t)words[103] << 48);

        /* A disk that says 48-bit but fills no count is taken at its
         * 28-bit one. */
        if (n48 != 0)
            n = n48;
    }
    if (n == 0)
        return -1;
    *sectors = n;
    *lba48 = big;
    model_string(words, model, cap);
    return 0;
}

ata_addr ata_addressing(int lba48, uint64_t lba, uint32_t count)
{
    if (count == 0)
        return ATA_ADDR_NONE;
    /* Linux's rule (lba_28_ok): the transfer ends below 2^28, so its last
     * sector is at most 0x0FFFFFFE, the highest a 28-bit count of
     * 0x0FFFFFFF sectors has. */
    if (count <= 256u && lba < 0x10000000u && lba + count < 0x10000000u)
        return ATA_ADDR_LBA28;
    if (lba48 && count <= 65536u && lba < ((uint64_t)1 << 48) &&
        lba + count <= ((uint64_t)1 << 48))
        return ATA_ADDR_LBA48;
    return ATA_ADDR_NONE;
}
