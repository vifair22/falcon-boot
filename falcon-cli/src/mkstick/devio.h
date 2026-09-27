/* falcon-boot mkstick: whole sectors in and out of a disk or image, for
 * the commands that write where no file owns the bytes (`bios`, `station`).
 *
 * Nothing here decides where: that is bioscore.c's and stationcore.c's.
 * These are the loops around pread and pwrite that make a short transfer
 * an error rather than a silently partial stage.
 */

#ifndef FALCON_MKSTICK_DEVIO_H
#define FALCON_MKSTICK_DEVIO_H

#include <stddef.h>
#include <stdint.h>

#include <common/status.h>

typedef struct {
    int      fd;
    uint32_t sector;
} mks_dev;

cli_status dev_read(const mks_dev *dev, uint64_t lba, uint64_t sectors,
                    uint8_t *buf);
cli_status dev_write(const mks_dev *dev, uint64_t lba, const uint8_t *buf,
                     uint64_t len);
cli_status dev_zero(const mks_dev *dev, uint64_t lba, uint64_t sectors);

/* A stage written as a whole number of sectors: the image, then zeros to
 * the end of its last sector, so the run on disk is exactly the plan's. */
cli_status dev_write_stage(const mks_dev *dev, uint64_t lba,
                           const uint8_t *img, size_t len, uint64_t sectors);

#endif /* FALCON_MKSTICK_DEVIO_H */
