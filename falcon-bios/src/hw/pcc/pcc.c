/* falcon jump's PCC.MOD: a CompactFlash card in a PC Card slot, as a disk
 * (DESIGN Section 14.4; the CF route of 14.1).
 *
 * The slot is a CardBus bridge's 16-bit side, found on the PCI bus by its
 * class (0x0607): the ThinkPad A21m's TI PCI-1450, and every laptop bridge
 * of the era. Its registers are BAR0: the CardBus socket registers, and the
 * ExCA set (the i82365SL's, which every bridge copies) at +0x800. A card is
 * brought up the way Linux's yenta and i82365 drivers do it:
 *
 *   1. card detect (ExCA interface status), a 16-bit card, and the voltages
 *      it takes (socket present state);
 *   2. VCC on through the socket control register, the ExCA power
 *      register's output enable, and the card let out of reset once it has
 *      settled, then READY waited for;
 *   3. memory window 0 onto the card's attribute memory, where its CIS is
 *      (pcccore.c parses it) and its configuration option register;
 *   4. the configuration the CIS offers for contiguous I/O written to that
 *      register, which makes the card an ATA device at any 16-byte base;
 *   5. I/O window 0 there, and from then on it is an ATA disk (atadisk.c).
 *
 * The attribute memory window goes at 0xD0000, below a megabyte, where PC
 * Card drivers have put it since DOS: it is only open while the CIS is
 * read, and closed before the machine is handed on. The I/O windows are
 * 0x1E0-0x1EF for the first card and 0x1C0-0x1CF for a second, a range the
 * machines Section 14 is for leave unused.
 *
 * The resident half (int13 through hw/blk13.c) uses only this module, as
 * every module's must (jump/jmod.h).
 */

#include <hw/jmodhdr.h>
#include <hw/modrt.h>
#include <hw/pci.h>
#include <hw/blk13.h>
#include <hw/pcc/pcccore.h>
#include <hw/ata/atadisk.h>

#include <lib/string.h>

#define SOCK_PRESENT 0x08u
#define SOCK_CONTROL 0x10u
#define PS_16BIT     (1u << 4)
#define PS_3V        (1u << 11)
#define SC_VCC_5V    0x20u
#define SC_VCC_3V    0x30u

#define EXCA         0x800u
#define X_STATUS     0x01u
#define X_POWER      0x02u
#define X_INTCTL     0x03u
#define X_CSCINT     0x05u
#define X_ENABLE     0x06u
#define X_IOCTL      0x07u
#define X_IO0        0x08u
#define X_MEM0       0x10u
#define X_PAGE0      0x40u

#define ST_CD        0x0Cu
#define ST_READY     0x20u
#define PWR_OE       0x80u
#define PWR_VCC      0x10u
#define IC_RESET     0x40u      /* 1: reset released */
#define IC_IOCARD    0x20u
#define EN_MEM0      0x01u
#define EN_IO0       0x40u
#define IO0_16BIT    0x01u

#define ATTR_HOST    0x000D0000u
#define ATTR_SIZE    0x1000u
#define CIS_BYTES    256u

#define SOCKETS_MAX  4u
#define SECTOR       512u

static const uint16_t k_io_bases[SOCKETS_MAX] = {
    0x1E0u, 0x1C0u, 0x1A0u, 0x180u
};

typedef struct {
    atadisk disk;
} pcc_dev;

static const jmod_services *g_svc;   /* probe time only */
static pcc_dev g_dev[SOCKETS_MAX];
static unsigned g_ndev;
static unsigned g_serve = SOCKETS_MAX;

static int serve_read(uint64_t lba, uint32_t count, void *buf);
static int serve_write(uint64_t lba, uint32_t count, const void *buf);
static blk13_dev g_blk = {
    SECTOR, 0, 1, serve_read, serve_write, 128u
};

static uint32_t g_regs;              /* the socket being brought up */

static void log_s(const char *s)
{
    if (g_svc != (const jmod_services *)0)
        g_svc->log(s);
}

static uint8_t exca_rd(uint32_t r)
{
    return *(volatile uint8_t *)(uintptr_t)(g_regs + EXCA + r);
}

static void exca_wr(uint32_t r, uint8_t v)
{
    *(volatile uint8_t *)(uintptr_t)(g_regs + EXCA + r) = v;
}

static uint32_t sock_rd(uint32_t r)
{
    return *(volatile uint32_t *)(uintptr_t)(g_regs + r);
}

static void sock_wr(uint32_t r, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)(g_regs + r) = v;
}

/* Steps 1 and 2: power, reset, READY. 0 when a 16-bit card is up. */
static int power_up(void)
{
    uint32_t present;
    unsigned i;

    if ((exca_rd(X_STATUS) & ST_CD) != ST_CD)
        return -1;                   /* no card */
    present = sock_rd(SOCK_PRESENT);
    if ((present & PS_16BIT) == 0)
        return -1;                   /* a CardBus card: not this driver's */

    exca_wr(X_CSCINT, 0);            /* no status-change interrupts */
    exca_wr(X_ENABLE, 0);
    exca_wr(X_INTCTL, 0);            /* reset held, a memory card */
    sock_wr(SOCK_CONTROL, (present & PS_3V) != 0 ? SC_VCC_3V : SC_VCC_5V);
    exca_wr(X_POWER, PWR_OE | PWR_VCC);
    io_delay_us(300000);             /* the card's supply settles */
    exca_wr(X_INTCTL, IC_RESET);
    io_delay_us(20000);
    for (i = 0; i < 200u; i++) {     /* up to 2 s */
        if ((exca_rd(X_STATUS) & ST_READY) != 0)
            return 0;
        io_delay_us(10000);
    }
    return -1;
}

static void power_down(void)
{
    exca_wr(X_ENABLE, 0);
    exca_wr(X_INTCTL, 0);
    exca_wr(X_POWER, 0);
    sock_wr(SOCK_CONTROL, 0);
}

/* Steps 3 to 5: the CIS, the configuration, the I/O window. 0 with the
 * card an ATA disk at `io`. */
static int configure(uint16_t io)
{
    volatile uint8_t *attr = (volatile uint8_t *)(uintptr_t)ATTR_HOST;
    uint8_t cis[CIS_BYTES];
    uint8_t w[6];
    pcc_cis c;
    unsigned i;

    if (pcc_mem_window(ATTR_HOST, ATTR_SIZE, 0, 1, w) != 0)
        return -1;
    for (i = 0; i < 6u; i++)
        exca_wr(X_MEM0 + i, w[i]);
    exca_wr(X_PAGE0, 0);
    exca_wr(X_ENABLE, EN_MEM0);
    io_delay_us(100);

    for (i = 0; i < CIS_BYTES; i++)
        cis[i] = attr[2u * i];       /* attribute memory: even bytes only */
    if (pcc_cis_parse(cis, sizeof cis, &c) != 0) {
        log_s("falcon-bios: PCC.MOD: the card's CIS could not be read\n");
        return -1;
    }
    if (c.funcid != FUNCID_FIXED_DISK) {
        log_s("falcon-bios: PCC.MOD: a card that is not a disk\n");
        return -1;
    }
    if (c.io_len != 0 || c.io_lines != 4u || c.config_base >= ATTR_SIZE) {
        log_s("falcon-bios: PCC.MOD: a card that offers no contiguous I/O\n");
        return -1;
    }
    attr[c.config_base] = c.index;   /* the configuration option register */
    io_delay_us(1000);

    if (pcc_io_window(io, 16u, w) != 0)
        return -1;
    for (i = 0; i < 4u; i++)
        exca_wr(X_IO0 + i, w[i]);
    exca_wr(X_IOCTL, IO0_16BIT);
    exca_wr(X_INTCTL, IC_RESET | IC_IOCARD);
    exca_wr(X_ENABLE, EN_IO0);       /* and the memory window closed */
    io_delay_us(1000);
    return 0;
}

static unsigned pcc_probe(void)
{
    unsigned nth;

    g_ndev = 0;
    for (nth = 0; nth < SOCKETS_MAX && g_ndev < SOCKETS_MAX; nth++) {
        pci_loc loc;
        uint32_t bar;
        uint32_t cmd;
        pcc_dev *d = &g_dev[g_ndev];

        if (pci_find_class(0x060700u, nth, &loc) != 0)
            break;
        bar = pci_read32(loc.bus, loc.dev, loc.fn, 0x10);
        if ((bar & 1u) != 0 || (bar & ~0xFFFu) == 0)
            continue;                /* no socket registers assigned */
        cmd = pci_read32(loc.bus, loc.dev, loc.fn, 0x04);
        pci_write32(loc.bus, loc.dev, loc.fn, 0x04,
                    (cmd & 0xFFFFu) | 0x0003u);
        g_regs = bar & ~0xFFFu;

        if (power_up() != 0)
            continue;
        if (configure(k_io_bases[g_ndev]) != 0) {
            power_down();
            continue;
        }
        memset(&d->disk, 0, sizeof d->disk);
        d->disk.base = k_io_bases[g_ndev];
        d->disk.ctrl = (uint16_t)(k_io_bases[g_ndev] + 0x0Eu);
        if (atadisk_identify(&d->disk) != 0) {
            log_s("falcon-bios: PCC.MOD: the card did not answer as ATA\n");
            power_down();
            continue;
        }
        log_s("falcon-bios: PCC.MOD: ");
        log_s(d->disk.model);
        log_s("\n");
        g_ndev++;
    }
    return g_ndev;
}

static int pcc_dev_info(unsigned idx, jmod_dev *out)
{
    if (idx >= g_ndev || out == (jmod_dev *)0)
        return -1;
    out->kind = JMOD_DEV_DISK;
    out->sector_size = SECTOR;
    out->sectors = g_dev[idx].disk.sectors;
    memcpy(out->label, g_dev[idx].disk.model, sizeof out->label);
    return 0;
}

static int pcc_read(unsigned idx, uint64_t lba, uint32_t count, void *buf)
{
    if (idx >= g_ndev || lba + count > g_dev[idx].disk.sectors)
        return -1;
    return atadisk_read(&g_dev[idx].disk, lba, count, buf);
}

static int serve_read(uint64_t lba, uint32_t count, void *buf)
{
    return pcc_read(g_serve, lba, count, buf);
}

static int serve_write(uint64_t lba, uint32_t count, const void *buf)
{
    if (g_serve >= g_ndev || lba + count > g_dev[g_serve].disk.sectors)
        return -1;
    return atadisk_write(&g_dev[g_serve].disk, lba, count, buf);
}

static int pcc_serve(unsigned idx, uint8_t drive)
{
    (void)drive;
    if (idx >= g_ndev)
        return -1;
    g_serve = idx;
    g_blk.sectors = g_dev[idx].disk.sectors;
    g_svc = (const jmod_services *)0;
    return 0;
}

static void pcc_int13(jmod_regs *r)
{
    if (g_serve >= g_ndev) {
        r->eax = (r->eax & 0xFFFF00FFu) | 0xAA00u;   /* drive not ready */
        r->flags = 1;
        return;
    }
    blk13_call(&g_blk, r);
}

static int pcc_init(const jmod_services *svc, jmod_ops *ops)
{
    g_svc = svc;
    ops->probe = pcc_probe;
    ops->dev = pcc_dev_info;
    ops->read = pcc_read;
    ops->serve = pcc_serve;
    ops->int13 = pcc_int13;
    return 0;
}

JMOD_HEADER("PCC", JMOD_SLOT_PCC, pcc_init);
