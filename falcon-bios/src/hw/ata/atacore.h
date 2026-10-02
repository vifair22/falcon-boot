/* falcon jump's ATA.MOD: the ATAPI command blocks and the answers, apart from
 * the port I/O (DESIGN Section 13.4).
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

/* How a PCI IDE controller runs one of its channels, and where. */
typedef enum {
    ATA_CHAN_COMPAT = 0,     /* compatibility mode: the legacy ports */
    ATA_CHAN_NATIVE,         /* native mode: the ports its BARs name */
    ATA_CHAN_UNASSIGNED      /* native, and no I/O port in the BARs: no ports */
} ata_chan_mode;

#define ATA_CHAN_NO_CTRL 0xFFu   /* ctrl_idx: the legacy pair with no controller */

typedef struct {
    uint16_t base;        /* the command block; 0 when unassigned */
    uint16_t ctrl;        /* the control register; 0 when unassigned */
    uint8_t  mode;        /* ata_chan_mode */
    uint8_t  ctrl_idx;    /* the controller in the list it came from */
    uint8_t  channel;     /* 0 primary, 1 secondary */
} ata_chan;

/* A PCI IDE controller (class 0x0101) as its configuration space says it:
 * the programming interface (offset 0x09) and BARs 0-3 (0x10-0x1C), raw. */
typedef struct {
    uint8_t  progif;
    uint32_t bar[4];
} ata_ide_ctrl;

/* The channels to probe, from the PCI IDE controllers found. A channel in
 * native mode (programming interface bit 0 for the primary, bit 2 for the
 * secondary) is at the I/O ports its BARs hold (0 and 1 for the primary, 2
 * and 3 for the secondary; the control register two past the second), one
 * in compatibility mode at the legacy pair (0x1F0/0x3F6, 0x170/0x376). A
 * native channel whose BAR holds no I/O port (bit 0 clear, zero, or past
 * the 16-bit port space) is listed as unassigned, so the caller can say so.
 * A pair of ports already listed is not listed again, so the legacy pair
 * appears once however many controllers claim it; with no controller at
 * all, the list is the legacy pair. The count, at most `cap`. */
unsigned ata_channel_list(const ata_ide_ctrl *ctrls, unsigned nctrl,
                          ata_chan *out, unsigned cap);

#endif /* FALCON_BIOS_ATACORE_H */
