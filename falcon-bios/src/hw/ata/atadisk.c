/* falcon jump's drivers: an ATA disk over PIO. See atadisk.h. */

#include <hw/ata/atadisk.h>
#include <hw/ata/atacore.h>
#include <hw/modrt.h>

#define REG_DATA    0
#define REG_COUNT   2
#define REG_LBA0    3
#define REG_LBA1    4
#define REG_LBA2    5
#define REG_DEVICE  6
#define REG_STATUS  7
#define REG_COMMAND 7

#define ST_ERR  0x01u
#define ST_DRQ  0x08u
#define ST_DF   0x20u
#define ST_BSY  0x80u

#define CTL_NIEN 0x02u

#define CMD_READ     0x20u
#define CMD_WRITE    0x30u
#define CMD_READ48   0x24u
#define CMD_WRITE48  0x34u
#define CMD_IDENTIFY 0xECu

/* 10 us steps: 5 s, for a card or disk still spinning up. */
#define WAIT 500000u

static uint8_t wait_ready(const atadisk *d)
{
    uint32_t steps = WAIT;

    while (steps-- > 0) {
        uint8_t s = inb(d->ctrl);

        if ((s & ST_BSY) == 0)
            return s;
        io_delay_us(10);
    }
    return 0xFFu;
}

static void settle(const atadisk *d)
{
    (void)inb(d->ctrl);
    (void)inb(d->ctrl);
    (void)inb(d->ctrl);
    (void)inb(d->ctrl);
}

/* Selects the drive and loads the LBA and count registers, as `mode` says:
 * LBA28's four bits in the device register, or LBA48's two passes through
 * each register's FIFO, the high bytes first. */
static int setup(const atadisk *d, uint64_t lba, uint32_t count,
                 ata_addr mode)
{
    if (mode == ATA_ADDR_NONE)
        return -1;
    if (wait_ready(d) == 0xFFu)
        return -1;
    outb((uint16_t)(d->base + REG_DEVICE),
         (uint8_t)(0xE0u | ((unsigned)d->slave << 4) |
                   (mode == ATA_ADDR_LBA28
                        ? ((uint32_t)(lba >> 24) & 0x0Fu) : 0u)));
    settle(d);
    if (wait_ready(d) == 0xFFu)
        return -1;
    outb(d->ctrl, CTL_NIEN);
    if (mode == ATA_ADDR_LBA48) {
        outb((uint16_t)(d->base + REG_COUNT), (uint8_t)((count >> 8) & 0xFFu));
        outb((uint16_t)(d->base + REG_LBA0), (uint8_t)((lba >> 24) & 0xFFu));
        outb((uint16_t)(d->base + REG_LBA1), (uint8_t)((lba >> 32) & 0xFFu));
        outb((uint16_t)(d->base + REG_LBA2), (uint8_t)((lba >> 40) & 0xFFu));
    }
    outb((uint16_t)(d->base + REG_COUNT), (uint8_t)(count & 0xFFu));
    outb((uint16_t)(d->base + REG_LBA0), (uint8_t)(lba & 0xFFu));
    outb((uint16_t)(d->base + REG_LBA1), (uint8_t)((lba >> 8) & 0xFFu));
    outb((uint16_t)(d->base + REG_LBA2), (uint8_t)((lba >> 16) & 0xFFu));
    return 0;
}

/* Waits for the drive to want a sector's worth of data. */
static int drq(const atadisk *d)
{
    uint8_t s = wait_ready(d);

    if (s == 0xFFu || (s & (ST_ERR | ST_DF)) != 0 || (s & ST_DRQ) == 0)
        return -1;
    return 0;
}

static void done(const atadisk *d)
{
    (void)inb((uint16_t)(d->base + REG_STATUS));    /* clears INTRQ */
    outb(d->ctrl, 0);
}

int atadisk_identify(atadisk *d)
{
    uint16_t w[256];
    unsigned i;
    int rc = -1;

    if (inb(d->ctrl) == 0xFFu)
        return -1;                   /* a floating bus: nothing there */
    if (setup(d, 0, 1, ATA_ADDR_LBA28) != 0)
        return -1;
    outb((uint16_t)(d->base + REG_COMMAND), CMD_IDENTIFY);
    settle(d);
    if (drq(d) == 0) {
        for (i = 0; i < 256u; i++)
            w[i] = inw((uint16_t)(d->base + REG_DATA));
        rc = ata_parse_identify_disk(w, &d->sectors, &d->lba48, d->model,
                                     sizeof d->model);
    }
    done(d);
    return rc;
}

static int transfer(const atadisk *d, uint64_t lba, uint32_t count,
                    uint8_t *p, int write)
{
    while (count > 0) {
        uint32_t n = count > 256u ? 256u : count;
        ata_addr mode = ata_addressing(d->lba48, lba, n);
        uint32_t s;
        int rc = 0;

        if (lba + n > d->sectors || setup(d, lba, n, mode) != 0)
            return -1;
        outb((uint16_t)(d->base + REG_COMMAND),
             mode == ATA_ADDR_LBA48 ? (write ? CMD_WRITE48 : CMD_READ48)
                                    : (write ? CMD_WRITE : CMD_READ));
        settle(d);
        for (s = 0; s < n && rc == 0; s++) {
            unsigned i;

            if (drq(d) != 0) {
                rc = -1;
                break;
            }
            for (i = 0; i < 256u; i++) {
                if (write)
                    outw((uint16_t)(d->base + REG_DATA),
                         (uint16_t)((unsigned)p[2u * i] |
                                    ((unsigned)p[2u * i + 1u] << 8)));
                else {
                    uint16_t v = inw((uint16_t)(d->base + REG_DATA));

                    p[2u * i] = (uint8_t)(v & 0xFFu);
                    p[2u * i + 1u] = (uint8_t)(v >> 8);
                }
            }
            p += 512;
        }
        if (rc == 0 && write) {
            uint8_t st = wait_ready(d);

            if (st == 0xFFu || (st & (ST_ERR | ST_DF)) != 0)
                rc = -1;
        }
        done(d);
        if (rc != 0)
            return -1;
        lba += n;
        count -= n;
    }
    return 0;
}

int atadisk_read(const atadisk *d, uint64_t lba, uint32_t count, void *buf)
{
    return transfer(d, lba, count, buf, 0);
}

int atadisk_write(const atadisk *d, uint64_t lba, uint32_t count,
                  const void *buf)
{
    return transfer(d, lba, count, (uint8_t *)(uintptr_t)buf, 1);
}
