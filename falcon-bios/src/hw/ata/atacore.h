/* falcon jump's ATA.MOD: the ATAPI command blocks and the answers, apart from
 * the port I/O (DESIGN Section 14.4).
 *
 * What a CD drive is told and what it says back are byte layouts - SCSI
 * command blocks, big-endian fields, a model string with its bytes swapped
 * in pairs - and a layout that is off by one byte asks for the wrong sector
 * and gets it. So they are here, host-tested, as vcdcore.c is to vcdres.S;
 * ata.c is the registers.
 */

#ifndef FALCON_BIOS_ATACORE_H
#define FALCON_BIOS_ATACORE_H

#include <stddef.h>
#include <stdint.h>

#define ATAPI_CDB_LEN   12u
#define ATAPI_SECTOR    2048u

#define SCSI_TEST_UNIT_READY 0x00u
#define SCSI_REQUEST_SENSE   0x03u
#define SCSI_READ_CAPACITY   0x25u
#define SCSI_READ_10         0x28u

/* The sense keys the driver acts on. */
#define SENSE_NONE            0x0u
#define SENSE_NOT_READY       0x2u
#define SENSE_UNIT_ATTENTION  0x6u

void atapi_cdb_test_unit_ready(uint8_t cdb[ATAPI_CDB_LEN]);
void atapi_cdb_request_sense(uint8_t cdb[ATAPI_CDB_LEN], uint8_t len);
void atapi_cdb_read_capacity(uint8_t cdb[ATAPI_CDB_LEN]);
void atapi_cdb_read10(uint8_t cdb[ATAPI_CDB_LEN], uint32_t lba,
                      uint16_t count);

/* READ CAPACITY's eight bytes: the last block's address and the block
 * length. *sectors is the count, one more than the last address. 0 on
 * success; -1 when the answer is not a CD's (a block length other than
 * 2048, or no blocks). */
int atapi_parse_capacity(const uint8_t data[8], uint64_t *sectors);

/* REQUEST SENSE's fixed-format answer: the sense key, or -1 when the
 * answer is not fixed format. */
int atapi_parse_sense(const uint8_t *data, size_t len);

/* IDENTIFY PACKET DEVICE's 256 words: whether the device is an ATAPI CD
 * (general configuration bits 15-14 are 10b, bits 12-8 are 5), and its model
 * string, trimmed, into `model` of `cap` bytes. Nonzero for a CD. */
int atapi_parse_identify(const uint16_t words[256], char *model, size_t cap);

#endif /* FALCON_BIOS_ATACORE_H */
