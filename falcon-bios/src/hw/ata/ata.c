/* falcon jump's ATA.MOD: ATAPI CD drives on the legacy IDE channels
 * (docs/DESIGN.md Section 14.4 and 14.6).
 *
 * A module, not stage 3: linked at JMOD_SLOT_ATA, read off the floppy only
 * when the CD route is chosen, and left resident behind the reverse thunk
 * (jump/rthunk.S) to answer the medium's own bootloader. Everything here that
 * the resident half reaches - int13 and what it calls - uses only this file,
 * atacore.c and falcon-core's mem routines linked into this image: by then
 * the core, and its services table, are gone (jump/jmod.h).
 *
 * PIO and polled. Both are what every IDE controller since the first
 * answers, and polling is all a resident with interrupts off can do; the
 * drive's own interrupt is masked (nIEN) while a command runs, so the BIOS's
 * IRQ handler is not left an interrupt it did not ask for.
 *
 * The legacy channels only (0x1F0 and 0x170), which is what the machines
 * Section 14 is for put their optical drive on: the ThinkPad A21m's PIIX4,
 * and QEMU's. A controller in PCI native mode is a later change.
 */

#include <hw/jmodhdr.h>
#include <hw/modrt.h>
#include <hw/ata/atacore.h>
#include <hw/blk13.h>

#include <lib/string.h>

/* Registers, as offsets from the command block's base. */
#define REG_DATA     0
#define REG_FEATURES 1
#define REG_IREASON  2
#define REG_BCNT_LO  4
#define REG_BCNT_HI  5
#define REG_SELECT   6
#define REG_STATUS   7      /* read */
#define REG_COMMAND  7      /* write */

#define ST_ERR  0x01u
#define ST_DRQ  0x08u
#define ST_BSY  0x80u

#define CTL_NIEN 0x02u

#define CMD_PACKET          0xA0u
#define CMD_IDENTIFY_PACKET 0xA1u

/* The most a single DRQ block may carry: 31 CD sectors, even, and below the
 * sixteen-bit byte count's ceiling. */
#define BYTE_LIMIT 0xF800u

/* Waits, in 10 us steps: a command's handshake, and a read, which may have
 * to wait for a disc to spin up. */
#define WAIT_SHORT  100000u      /* 1 s */
#define WAIT_LONG   1000000u     /* 10 s */

#define DEVS_MAX 4u

typedef struct {
    uint16_t base;
    uint16_t ctrl;
    uint8_t  slave;
    uint64_t sectors;
    char     model[41];
} ata_dev;

static ata_dev g_dev[DEVS_MAX];
static unsigned g_ndev;
static const jmod_services *g_svc;   /* probe time only */

/* The device the resident answers for, as INT 13h sees it (hw/blk13.c):
 * filled in by serve. */
static unsigned g_serve = DEVS_MAX;
static int serve_read(uint64_t lba, uint32_t count, void *buf);
static blk13_dev g_blk = {
    ATAPI_SECTOR, 0, 1, serve_read, 0, BYTE_LIMIT / ATAPI_SECTOR
};

static const struct {
    uint16_t base;
    uint16_t ctrl;
} k_channels[] = {
    { 0x1F0u, 0x3F6u },
    { 0x170u, 0x376u },
};

static void log_s(const char *s)
{
    if (g_svc != (const jmod_services *)0)
        g_svc->log(s);
}

/* 400 ns for the device's status to settle after a select: four reads of
 * the alternate status register, which is what the specification counts. */
static void settle(const ata_dev *d)
{
    (void)inb(d->ctrl);
    (void)inb(d->ctrl);
    (void)inb(d->ctrl);
    (void)inb(d->ctrl);
}

/* The status once BSY is clear, or 0xFF when it never clears. */
static uint8_t wait_ready(const ata_dev *d, uint32_t steps)
{
    while (steps-- > 0) {
        uint8_t s = inb(d->ctrl);

        if ((s & ST_BSY) == 0)
            return s;
        io_delay_us(10);
    }
    return 0xFFu;
}

static void select_dev(const ata_dev *d)
{
    outb((uint16_t)(d->base + REG_SELECT), (uint8_t)(0xA0u | ((unsigned)d->slave << 4)));
    settle(d);
}

/* One packet command: `cdb` out, up to `cap` bytes back into `buf`. 0 on
 * success with *got the bytes the device sent (more than cap are read and
 * dropped); -1 on a timeout or an error status. */
static int packet(const ata_dev *d, const uint8_t cdb[ATAPI_CDB_LEN],
                  uint8_t *buf, uint32_t cap, uint32_t *got)
{
    uint8_t s;
    unsigned i;
    int rc = -1;

    *got = 0;
    if (wait_ready(d, WAIT_SHORT) == 0xFFu)
        return -1;
    select_dev(d);
    s = wait_ready(d, WAIT_SHORT);
    if (s == 0xFFu || (s & ST_DRQ) != 0)
        return -1;

    outb(d->ctrl, CTL_NIEN);
    outb((uint16_t)(d->base + REG_FEATURES), 0);         /* PIO */
    outb((uint16_t)(d->base + REG_BCNT_LO), (uint8_t)(BYTE_LIMIT & 0xFFu));
    outb((uint16_t)(d->base + REG_BCNT_HI), (uint8_t)(BYTE_LIMIT >> 8));
    outb((uint16_t)(d->base + REG_COMMAND), CMD_PACKET);
    settle(d);

    s = wait_ready(d, WAIT_SHORT);
    if (s == 0xFFu || (s & ST_ERR) != 0 || (s & ST_DRQ) == 0)
        goto out;
    for (i = 0; i < ATAPI_CDB_LEN; i += 2u)
        outw((uint16_t)(d->base + REG_DATA),
             (uint16_t)((unsigned)cdb[i] | ((unsigned)cdb[i + 1u] << 8)));
    settle(d);

    for (;;) {
        uint32_t n;
        uint32_t w;

        s = wait_ready(d, WAIT_LONG);
        if (s == 0xFFu)
            goto out;
        s = inb((uint16_t)(d->base + REG_STATUS));   /* and clears INTRQ */
        if ((s & ST_ERR) != 0)
            goto out;
        if ((s & ST_DRQ) == 0)
            break;
        n = (uint32_t)inb((uint16_t)(d->base + REG_BCNT_LO)) |
            ((uint32_t)inb((uint16_t)(d->base + REG_BCNT_HI)) << 8);
        for (w = 0; w < n; w += 2u) {
            uint16_t v = inw((uint16_t)(d->base + REG_DATA));

            if (*got + 1u < cap) {
                buf[*got] = (uint8_t)(v & 0xFFu);
                buf[*got + 1u] = (uint8_t)(v >> 8);
            }
            *got += 2u;
        }
    }
    rc = 0;
out:
    (void)inb((uint16_t)(d->base + REG_STATUS));
    outb(d->ctrl, 0);
    return rc;
}

static int sense_key(const ata_dev *d)
{
    uint8_t cdb[ATAPI_CDB_LEN];
    uint8_t data[18];
    uint32_t got;

    (void)scsi_cdb_request_sense(cdb, (uint8_t)sizeof data);
    if (packet(d, cdb, data, sizeof data, &got) != 0)
        return -1;
    return scsi_parse_sense(data, got < sizeof data ? got : sizeof data);
}

/* A command that may meet a disc still spinning up or just changed: a UNIT
 * ATTENTION is taken once and retried, a NOT READY waited out for up to ten
 * seconds. */
static int packet_retry(const ata_dev *d, const uint8_t cdb[ATAPI_CDB_LEN],
                        uint8_t *buf, uint32_t cap, uint32_t *got)
{
    unsigned tries;

    for (tries = 0; tries < 20u; tries++) {
        int key;

        if (packet(d, cdb, buf, cap, got) == 0)
            return 0;
        key = sense_key(d);
        if (key == (int)SENSE_UNIT_ATTENTION)
            continue;
        if (key != (int)SENSE_NOT_READY)
            return -1;
        io_delay_us(500000u);
    }
    return -1;
}

/* IDENTIFY PACKET DEVICE: whether a CD is there, and its name. */
static int identify(ata_dev *d)
{
    uint16_t words[256];
    uint8_t s;
    unsigned i;

    s = inb(d->ctrl);
    if (s == 0xFFu)
        return 0;                      /* a floating bus: no channel */
    select_dev(d);
    if (wait_ready(d, WAIT_SHORT) == 0xFFu)
        return 0;
    outb(d->ctrl, CTL_NIEN);
    outb((uint16_t)(d->base + REG_COMMAND), CMD_IDENTIFY_PACKET);
    settle(d);
    s = wait_ready(d, WAIT_SHORT);
    if (s == 0xFFu || (s & ST_ERR) != 0 || (s & ST_DRQ) == 0) {
        (void)inb((uint16_t)(d->base + REG_STATUS));
        outb(d->ctrl, 0);
        return 0;
    }
    for (i = 0; i < 256u; i++)
        words[i] = inw((uint16_t)(d->base + REG_DATA));
    (void)inb((uint16_t)(d->base + REG_STATUS));
    outb(d->ctrl, 0);
    return atapi_parse_identify(words, d->model, sizeof d->model);
}

static void capacity(ata_dev *d)
{
    uint8_t cdb[ATAPI_CDB_LEN];
    uint8_t data[8];
    uint32_t got;

    d->sectors = 0;
    (void)scsi_cdb_read_capacity(cdb);
    if (packet_retry(d, cdb, data, sizeof data, &got) != 0 || got < 8u ||
        atapi_parse_capacity(data, &d->sectors) != 0)
        d->sectors = 0;
}

static unsigned ata_probe(void)
{
    unsigned c;
    unsigned sl;

    g_ndev = 0;
    for (c = 0; c < sizeof k_channels / sizeof k_channels[0]; c++) {
        for (sl = 0; sl < 2u && g_ndev < DEVS_MAX; sl++) {
            ata_dev *d = &g_dev[g_ndev];

            d->base = k_channels[c].base;
            d->ctrl = k_channels[c].ctrl;
            d->slave = (uint8_t)sl;
            if (!identify(d))
                continue;
            capacity(d);
            log_s("falcon-bios: ATA.MOD: ");
            log_s(d->model);
            log_s(d->sectors != 0 ? ", a disc in it\n" : ", no disc\n");
            g_ndev++;
        }
    }
    return g_ndev;
}

static int ata_dev_info(unsigned idx, jmod_dev *out)
{
    if (idx >= g_ndev || out == (jmod_dev *)0)
        return -1;
    out->kind = JMOD_DEV_CD;
    out->sector_size = ATAPI_SECTOR;
    out->sectors = g_dev[idx].sectors;
    memcpy(out->label, g_dev[idx].model, sizeof out->label);
    return 0;
}

static int ata_read(unsigned idx, uint64_t lba, uint32_t count, void *buf)
{
    uint8_t *p = buf;

    if (idx >= g_ndev || lba > 0xFFFFFFFFu)
        return -1;
    while (count > 0) {
        uint8_t cdb[ATAPI_CDB_LEN];
        uint32_t n = count > BYTE_LIMIT / ATAPI_SECTOR ?
                     BYTE_LIMIT / ATAPI_SECTOR : count;
        uint32_t got;

        (void)scsi_cdb_read10(cdb, (uint32_t)lba, (uint16_t)n);
        if (packet_retry(&g_dev[idx], cdb, p, n * ATAPI_SECTOR, &got) != 0 ||
            got < n * ATAPI_SECTOR)
            return -1;
        p += n * ATAPI_SECTOR;
        lba += n;
        count -= n;
    }
    return 0;
}

static int ata_serve(unsigned idx, uint8_t drive)
{
    (void)drive;
    if (idx >= g_ndev)
        return -1;
    g_serve = idx;
    g_blk.sectors = g_dev[idx].sectors;
    /* From here the resident answers, and the core's services go away. */
    g_svc = (const jmod_services *)0;
    return 0;
}

/* ---- the resident: INT 13h for the drive the stub answers ---- */

static int serve_read(uint64_t lba, uint32_t count, void *buf)
{
    return ata_read(g_serve, lba, count, buf);
}

static void ata_int13(jmod_regs *r)
{
    if (g_serve >= g_ndev) {
        r->eax = (r->eax & 0xFFFF00FFu) | 0xAA00u;   /* drive not ready */
        r->flags = 1;
        return;
    }
    blk13_call(&g_blk, r);
}

static int ata_init(const jmod_services *svc, jmod_ops *ops)
{
    g_svc = svc;
    ops->probe = ata_probe;
    ops->dev = ata_dev_info;
    ops->read = ata_read;
    ops->serve = ata_serve;
    ops->int13 = ata_int13;
    return 0;
}

JMOD_HEADER("ATA", JMOD_SLOT_ATA, ata_init);
