/* falcon jump's drivers: SCSI command blocks and answers. See scsi.h. */

#include <hw/scsi/scsi.h>

static void clear(uint8_t cdb[SCSI_CDB_MAX])
{
    size_t i;

    for (i = 0; i < SCSI_CDB_MAX; i++)
        cdb[i] = 0;
}

size_t scsi_cdb_test_unit_ready(uint8_t cdb[SCSI_CDB_MAX])
{
    clear(cdb);
    cdb[0] = SCSI_TEST_UNIT_READY;
    return 6u;
}

size_t scsi_cdb_request_sense(uint8_t cdb[SCSI_CDB_MAX], uint8_t len)
{
    clear(cdb);
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = len;
    return 6u;
}

size_t scsi_cdb_inquiry(uint8_t cdb[SCSI_CDB_MAX], uint8_t len)
{
    clear(cdb);
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = len;
    return 6u;
}

size_t scsi_cdb_read_capacity(uint8_t cdb[SCSI_CDB_MAX])
{
    clear(cdb);
    cdb[0] = SCSI_READ_CAPACITY;
    return 10u;
}

static size_t rw10(uint8_t cdb[SCSI_CDB_MAX], uint8_t op, uint32_t lba,
                   uint16_t count)
{
    clear(cdb);
    cdb[0] = op;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)((lba >> 16) & 0xFFu);
    cdb[4] = (uint8_t)((lba >> 8) & 0xFFu);
    cdb[5] = (uint8_t)(lba & 0xFFu);
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)(count & 0xFFu);
    return 10u;
}

size_t scsi_cdb_read10(uint8_t cdb[SCSI_CDB_MAX], uint32_t lba,
                       uint16_t count)
{
    return rw10(cdb, SCSI_READ_10, lba, count);
}

size_t scsi_cdb_write10(uint8_t cdb[SCSI_CDB_MAX], uint32_t lba,
                        uint16_t count)
{
    return rw10(cdb, SCSI_WRITE_10, lba, count);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int scsi_parse_capacity(const uint8_t data[8], uint64_t *sectors,
                        uint32_t *block)
{
    uint32_t last = be32(data);
    uint32_t len = be32(data + 4);

    if (sectors == (uint64_t *)0 || block == (uint32_t *)0 || len == 0 ||
        last == 0xFFFFFFFFu)
        return -1;
    *sectors = (uint64_t)last + 1u;
    *block = len;
    return 0;
}

int scsi_parse_sense(const uint8_t *data, size_t len)
{
    uint8_t code;

    if (data == (const uint8_t *)0 || len < 3u)
        return -1;
    code = (uint8_t)(data[0] & 0x7Fu);
    if (code != 0x70u && code != 0x71u)
        return -1;
    return data[2] & 0x0F;
}
