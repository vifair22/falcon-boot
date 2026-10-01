/* falcon jump's drivers: INT 13h for one block device, the half every
 * module's resident entry shares (DESIGN Section 13.6).
 *
 * The reverse thunk hands a module the caller's registers; this reads the
 * call out of them, asks the device, and writes the answer back. Which
 * device and how it moves bytes is the module's (blk13_dev); the extended
 * calls a bootloader makes are the same for a CD and a stick but for the
 * sector size.
 *
 * Runs in the resident, so it uses nothing but the module it is linked into
 * (jump/jmod.h): the caller's buffers are reached as linear addresses, which
 * paging-off protected mode can write directly.
 */

#ifndef FALCON_BIOS_BLK13_H
#define FALCON_BIOS_BLK13_H

#include <stdint.h>

#include <jump/jmod.h>

typedef struct {
    uint32_t sector_size;
    uint64_t sectors;
    int      removable;
    /* `count` sectors from `lba`; 0 on success. write may be NULL. */
    int (*read)(uint64_t lba, uint32_t count, void *buf);
    int (*write)(uint64_t lba, uint32_t count, const void *buf);
    /* The most one read or write may carry, in sectors. */
    uint32_t max_count;
} blk13_dev;

void blk13_call(const blk13_dev *dev, jmod_regs *r);

#endif /* FALCON_BIOS_BLK13_H */
