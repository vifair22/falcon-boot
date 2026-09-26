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

int atapi_parse_identify(const uint16_t words[256], char *model, size_t cap)
{
    uint16_t cfg = words[0];
    size_t n = 0;
    size_t i;

    if (model != (char *)0 && cap > 0) {
        /* Words 27-46, forty characters, each word's bytes the wrong way
         * round for a string. */
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
    return (cfg & 0xC000u) == 0x8000u && ((cfg >> 8) & 0x1Fu) == 5u;
}
