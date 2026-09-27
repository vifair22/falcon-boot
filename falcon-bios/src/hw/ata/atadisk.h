/* falcon jump's drivers: an ATA disk over PIO at any I/O base (DESIGN
 * Section 14.4). A CompactFlash card behind a PC Card bridge is one
 * (PCC.MOD), and so is a disk on an IDE channel the BIOS does not boot from.
 * Polled, the drive's interrupt masked while a command runs, and nothing
 * of the core used: it runs in a module's resident half. LBA28, and LBA48
 * for a transfer LBA28 cannot address on a disk that has it (DESIGN
 * Section 13.10): an IDE disk or a CF card in an old laptop can be larger
 * than 128 GiB today. Which one a transfer takes is atacore.c's
 * ata_addressing, host-tested.
 */

#ifndef FALCON_BIOS_ATADISK_H
#define FALCON_BIOS_ATADISK_H

#include <stdint.h>

typedef struct {
    uint16_t base;          /* the command block: data at +0 .. command at +7 */
    uint16_t ctrl;          /* alternate status / device control */
    uint8_t  slave;
    uint64_t sectors;
    int      lba48;         /* IDENTIFY says the 48-bit feature set is on */
    char     model[41];
} atadisk;

/* IDENTIFY DEVICE: fills sectors and model. 0 for an LBA ATA disk. */
int atadisk_identify(atadisk *d);

/* `count` 512-byte sectors from `lba`. 0 on success. */
int atadisk_read(const atadisk *d, uint64_t lba, uint32_t count, void *buf);
int atadisk_write(const atadisk *d, uint64_t lba, uint32_t count,
                  const void *buf);

#endif /* FALCON_BIOS_ATADISK_H */
