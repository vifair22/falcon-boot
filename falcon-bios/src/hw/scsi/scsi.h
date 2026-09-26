/* falcon jump's drivers: the SCSI command blocks and answers both of them
 * speak (DESIGN Section 14.4).
 *
 * An ATAPI CD drive and a USB stick are both told what to do in SCSI: the
 * same READ(10), READ CAPACITY and REQUEST SENSE, carried in a 12-byte ATAPI
 * packet by ATA.MOD and in a Bulk-Only Transport wrapper by the USB modules.
 * The layouts are big-endian and a byte off reads the wrong block without
 * complaint, so they are here, once, host-tested.
 */

#ifndef FALCON_BIOS_SCSI_H
#define FALCON_BIOS_SCSI_H

#include <stddef.h>
#include <stdint.h>

/* Every command block this builds is at most this long; a 10-byte command
 * leaves the rest zero, which is how ATAPI pads it to its 12. */
#define SCSI_CDB_MAX 12u

#define SCSI_TEST_UNIT_READY 0x00u
#define SCSI_REQUEST_SENSE   0x03u
#define SCSI_INQUIRY         0x12u
#define SCSI_READ_CAPACITY   0x25u
#define SCSI_READ_10         0x28u
#define SCSI_WRITE_10        0x2Au

#define SENSE_NONE           0x0u
#define SENSE_NOT_READY      0x2u
#define SENSE_UNIT_ATTENTION 0x6u

/* Each fills `cdb` (SCSI_CDB_MAX bytes) and returns the command's length. */
size_t scsi_cdb_test_unit_ready(uint8_t cdb[SCSI_CDB_MAX]);
size_t scsi_cdb_request_sense(uint8_t cdb[SCSI_CDB_MAX], uint8_t len);
size_t scsi_cdb_inquiry(uint8_t cdb[SCSI_CDB_MAX], uint8_t len);
size_t scsi_cdb_read_capacity(uint8_t cdb[SCSI_CDB_MAX]);
size_t scsi_cdb_read10(uint8_t cdb[SCSI_CDB_MAX], uint32_t lba,
                       uint16_t count);
size_t scsi_cdb_write10(uint8_t cdb[SCSI_CDB_MAX], uint32_t lba,
                        uint16_t count);

/* READ CAPACITY(10)'s eight bytes: *sectors is the block count (one more
 * than the last address) and *block the block length. 0 on success; -1 on
 * an answer that describes nothing (no blocks, a zero block length). */
int scsi_parse_capacity(const uint8_t data[8], uint64_t *sectors,
                        uint32_t *block);

/* REQUEST SENSE's fixed-format answer: the sense key, or -1 when the answer
 * is not fixed format. */
int scsi_parse_sense(const uint8_t *data, size_t len);

#endif /* FALCON_BIOS_SCSI_H */
