/* falcon jump's ATA.MOD: the ATAPI command blocks and answers. See
 * atacore.h. */

#include <hw/ata/atacore.h>

static void clear(uint8_t cdb[ATAPI_CDB_LEN])
{
    size_t i;

    for (i = 0; i < ATAPI_CDB_LEN; i++)
        cdb[i] = 0;
}

void atapi_cdb_test_unit_ready(uint8_t cdb[ATAPI_CDB_LEN])
{
    clear(cdb);
    cdb[0] = SCSI_TEST_UNIT_READY;
}

void atapi_cdb_request_sense(uint8_t cdb[ATAPI_CDB_LEN], uint8_t len)
{
    clear(cdb);
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = len;
}

void atapi_cdb_read_capacity(uint8_t cdb[ATAPI_CDB_LEN])
{
    clear(cdb);
    cdb[0] = SCSI_READ_CAPACITY;
}

void atapi_cdb_read10(uint8_t cdb[ATAPI_CDB_LEN], uint32_t lba,
                      uint16_t count)
{
    clear(cdb);
    cdb[0] = SCSI_READ_10;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)((lba >> 16) & 0xFFu);
    cdb[4] = (uint8_t)((lba >> 8) & 0xFFu);
    cdb[5] = (uint8_t)(lba & 0xFFu);
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)(count & 0xFFu);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int atapi_parse_capacity(const uint8_t data[8], uint64_t *sectors)
{
    uint32_t last = be32(data);
    uint32_t block = be32(data + 4);

    if (sectors == (uint64_t *)0 || block != ATAPI_SECTOR ||
        last == 0xFFFFFFFFu)
        return -1;
    *sectors = (uint64_t)last + 1u;
    return 0;
}

int atapi_parse_sense(const uint8_t *data, size_t len)
{
    uint8_t code;

    if (data == (const uint8_t *)0 || len < 3u)
        return -1;
    code = (uint8_t)(data[0] & 0x7Fu);
    if (code != 0x70u && code != 0x71u)
        return -1;
    return data[2] & 0x0F;
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
