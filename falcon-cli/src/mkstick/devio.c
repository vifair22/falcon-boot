/* falcon-boot mkstick: whole sectors in and out of a disk. See devio.h.
 */

#include <mkstick/devio.h>

#include <string.h>
#include <unistd.h>

#include <mkstick/bioscore.h>

cli_status dev_read(const mks_dev *dev, uint64_t lba, uint64_t sectors,
                    uint8_t *buf)
{
    uint64_t want = sectors * dev->sector;
    uint64_t done = 0;

    /* A zero-length read would return success having left the caller's
     * buffer untouched. Written as a statement and a literal return because
     * clang-tidy does not track what the variadic set_error hands back. */
    if (want == 0u) {
        (void)set_error(CLI_ERR_INTERNAL, "a read of no sectors at LBA %llu",
                        (unsigned long long)lba);
        return CLI_ERR_INTERNAL;
    }

    while (done < want) {
        ssize_t n = pread(dev->fd, buf + done, (size_t)(want - done),
                          (off_t)(lba * dev->sector + done));

        if (n < 0)
            return set_error_errno(CLI_ERR_IO, "reading LBA %llu",
                                   (unsigned long long)lba);
        if (n == 0)
            return set_error(CLI_ERR_IO, "short read at LBA %llu",
                             (unsigned long long)lba);
        done += (uint64_t)n;
    }
    return CLI_OK;
}

cli_status dev_write(const mks_dev *dev, uint64_t lba, const uint8_t *buf,
                     uint64_t len)
{
    uint64_t done = 0;

    while (done < len) {
        ssize_t n = pwrite(dev->fd, buf + done, (size_t)(len - done),
                           (off_t)(lba * dev->sector + done));

        if (n <= 0)
            return set_error_errno(CLI_ERR_IO, "writing LBA %llu",
                                   (unsigned long long)lba);
        done += (uint64_t)n;
    }
    return CLI_OK;
}

cli_status dev_zero(const mks_dev *dev, uint64_t lba, uint64_t sectors)
{
    uint8_t buf[64u * 1024u];
    uint64_t left = sectors * dev->sector;
    uint64_t at = lba;
    cli_status rc;

    memset(buf, 0, sizeof buf);
    while (left > 0u) {
        uint64_t use = (left < sizeof buf) ? left : sizeof buf;

        rc = dev_write(dev, at, buf, use);
        if (rc != CLI_OK)
            return rc;
        at += use / dev->sector;
        left -= use;
    }
    return CLI_OK;
}

cli_status dev_write_stage(const mks_dev *dev, uint64_t lba,
                           const uint8_t *img, size_t len, uint64_t sectors)
{
    size_t whole = len - (len % dev->sector);
    cli_status rc = CLI_OK;

    if (whole > 0u)
        rc = dev_write(dev, lba, img, whole);
    if (rc == CLI_OK && whole < len) {
        uint8_t tail[BIOS_SECTOR_MAX];

        memset(tail, 0, sizeof tail);
        memcpy(tail, img + whole, len - whole);
        rc = dev_write(dev, lba + whole / dev->sector, tail, dev->sector);
    }
    if (rc == CLI_OK && sectors * dev->sector < len)
        rc = set_error(CLI_ERR_INTERNAL, "a stage longer than its plan");
    return rc;
}
