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

/* IDENTIFY DEVICE's 256 words for a disk: its sector count, whether it
 * takes 48-bit commands, and the model string, trimmed. 0 for an ATA
 * device that addresses by LBA (a CompactFlash card included, which says
 * 0x848A in word 0); -1 for a packet device or one that addresses by CHS
 * alone.
 *
 * *lba48 is set when word 83 is valid (bits 15-14 01b) and says the 48-bit
 * feature set is supported (bit 10), and word 86 that it is enabled (bit
 * 10); the count is then words 100-103, which a disk past 2^28 sectors
 * fills while words 60-61 stay at 0x0FFFFFFF. Otherwise the count is
 * words 60-61. */
int ata_parse_identify_disk(const uint16_t words[256], uint64_t *sectors,
                            int *lba48, char *model, size_t cap);

/* How a transfer of `count` sectors from `lba` is addressed. */
typedef enum {
    ATA_ADDR_NONE = 0,   /* past what the disk can be asked for */
    ATA_ADDR_LBA28,      /* READ/WRITE SECTORS: ends below 2^28, at most 256 */
    ATA_ADDR_LBA48       /* READ/WRITE SECTORS EXT: at most 65536 */
} ata_addr;

/* LBA28 wherever it reaches, since every ATA disk takes it and some
 * bridges and cards handle nothing else well; LBA48 only for a disk that
 * has it and a transfer LBA28 cannot address: one that ends past sector
 * 2^28 - 1 (128 GiB), or of more than 256 sectors. */
ata_addr ata_addressing(int lba48, uint64_t lba, uint32_t count);

#endif /* FALCON_BIOS_ATACORE_H */
