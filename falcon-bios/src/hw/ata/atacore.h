/* falcon jump's ATA.MOD: the ATAPI command blocks and the answers, apart from
 * the port I/O (DESIGN Section 14.4).
 *
 * What a CD drive says back is byte layouts - a model string with its bytes
 * swapped in pairs, a capacity that is only a CD's at 2048 bytes a block -
 * and a layout off by one byte misreads without complaint. So they are here,
 * host-tested, as vcdcore.c is to vcdres.S; the SCSI command blocks are
 * hw/scsi's, shared with the USB modules; ata.c is the registers.
 */

#ifndef FALCON_BIOS_ATACORE_H
#define FALCON_BIOS_ATACORE_H

#include <stddef.h>
#include <stdint.h>

#include <hw/scsi/scsi.h>

#define ATAPI_CDB_LEN   SCSI_CDB_MAX
#define ATAPI_SECTOR    2048u

/* READ CAPACITY's answer for a CD: the block count, when the block length
 * is 2048. 0 on success; -1 otherwise. */
int atapi_parse_capacity(const uint8_t data[8], uint64_t *sectors);

/* IDENTIFY PACKET DEVICE's 256 words: whether the device is an ATAPI CD
 * (general configuration bits 15-14 are 10b, bits 12-8 are 5), and its model
 * string, trimmed, into `model` of `cap` bytes. Nonzero for a CD. */
int atapi_parse_identify(const uint16_t words[256], char *model, size_t cap);

#endif /* FALCON_BIOS_ATACORE_H */
