/* falcon jump's PCC.MOD: a CompactFlash card in a PC Card slot, as a disk
 * (DESIGN Section 13.4; the CF route of 13.1).
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
 * The attribute memory window goes below a megabyte, in the option-ROM
 * area, at a 4 KB no option ROM claims, found when the probe runs: no fixed
 * address there is free on every machine (the A21m shadows a mini-PCI
 * card's boot ROM at 0xD0000, where PC Card drivers have put it since DOS).
 * It is only open while the CIS is read, and closed before the machine is
 * handed on. A card's I/O window is the first of 0x1E0, 0x1C0, 0x1A0 and
 * 0x180 (sixteen ports each) that nothing on the machine answers at: a
 * free ISA range reads 0xFF on every port; a third IDE channel at 0x1E8,
 * or the card in the socket before, does not.
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
#define PWR_VCC_5V   0x10u      /* the VCC field, bits 4:3 */
#define PWR_VCC_3V   0x18u
#define IC_RESET     0x40u      /* 1: reset released */
#define IC_IOCARD    0x20u
#define EN_MEM0      0x01u
#define EN_IO0       0x40u
#define IO0_16BIT    0x01u

#define ATTR_CHECK   16u     /* even bytes compared before and after */
#define CIS_BYTES    256u

#define SOCKETS_MAX  4u
#define SECTOR       512u

static const uint16_t k_io_bases[SOCKETS_MAX] = {
    0x1E0u, 0x1C0u, 0x1A0u, 0x180u
};

typedef struct {
    atadisk disk;
    uint32_t regs;                   /* its socket registers, for release */
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

/* "falcon-bios: PCC.MOD: <what> 0x<v><tail>". A socket that yields no disk
 * says which step it stopped at and what the register said, on serial: on
 * a real bridge the reason is otherwise a guess. */
static void log_vt(const char *what, uint32_t v, const char *tail)
{
    if (g_svc == (const jmod_services *)0)
        return;
    g_svc->log("falcon-bios: PCC.MOD: ");
    g_svc->log(what);
    g_svc->log(" ");
    g_svc->log_hex(v);
    g_svc->log(tail);
}

static void log_v(const char *what, uint32_t v)
{
    log_vt(what, v, "\n");
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

    if ((exca_rd(X_STATUS) & ST_CD) != ST_CD) {
        log_v("no card detected, ExCA status", exca_rd(X_STATUS));
        return -1;
    }
    present = sock_rd(SOCK_PRESENT);
    log_v("a card, socket present state", present);
    if ((present & PS_16BIT) == 0)
        return -1;                   /* a CardBus card: not this driver's */

    exca_wr(X_CSCINT, 0);            /* no status-change interrupts */
    exca_wr(X_ENABLE, 0);
    exca_wr(X_INTCTL, 0);            /* reset held, a memory card */
    /* The same voltage in both registers. A TI bridge takes the ExCA VCC
     * field as a request too: the A21m's PCI-1450, asked for 3.3 V through
     * the socket and 5 V through ExCA, powered a 3.3 V card with neither. */
    if ((present & PS_3V) != 0) {
        sock_wr(SOCK_CONTROL, SC_VCC_3V);
        exca_wr(X_POWER, PWR_OE | PWR_VCC_3V);
    } else {
        sock_wr(SOCK_CONTROL, SC_VCC_5V);
        exca_wr(X_POWER, PWR_OE | PWR_VCC_5V);
    }
    io_delay_us(300000);             /* the card's supply settles */
    exca_wr(X_INTCTL, IC_RESET);
    io_delay_us(20000);
    for (i = 0; i < 200u; i++) {     /* up to 2 s */
        if ((exca_rd(X_STATUS) & ST_READY) != 0)
            return 0;
        io_delay_us(10000);
    }
    log_v("the card never became ready, ExCA status", exca_rd(X_STATUS));
    log_v("socket present state", sock_rd(SOCK_PRESENT));
    return -1;
}

static void power_down(void)
{
    exca_wr(X_ENABLE, 0);
    exca_wr(X_INTCTL, 0);
    exca_wr(X_POWER, 0);
    sock_wr(SOCK_CONTROL, 0);
}

/* The first I/O window base nothing answers at (pcc_io_free). A card
 * served from an earlier socket answers at its own base, so it is passed
 * over with the rest. 0 when none is free. */
static uint16_t io_pick(void)
{
    unsigned b;

    for (b = 0; b < SOCKETS_MAX; b++) {
        uint16_t base = k_io_bases[b];
        uint8_t ports[PCC_IO_PORTS];
        unsigned i;

        for (i = 0; i < PCC_IO_PORTS; i++)
            ports[i] = inb((uint16_t)(base + i));
        if (pcc_io_free(ports))
            return base;
        log_vt("I/O at", base, " answers already\n");
    }
    log_s("falcon-bios: PCC.MOD: no free I/O window for the card\n");
    return 0;
}

/* The option-ROM area's headers (pcccore.h), read once per probe before
 * any window is open. */
static uint8_t g_roms[3u * PCC_ROM_SLOTS];

static void read_roms(void)
{
    const volatile uint8_t *area =
        (const volatile uint8_t *)(uintptr_t)PCC_ROM_BASE;
    unsigned i;

    for (i = 0; i < PCC_ROM_SLOTS; i++) {
        const volatile uint8_t *h = area + i * PCC_ROM_SLOT;

        g_roms[3u * i] = h[0];
        g_roms[3u * i + 1u] = h[1];
        g_roms[3u * i + 2u] = h[2];
        if (h[0] == 0x55u && h[1] == 0xAAu &&
            g_svc != (const jmod_services *)0) {
            g_svc->log("falcon-bios: PCC.MOD: an option ROM at ");
            g_svc->log_hex(PCC_ROM_BASE + i * PCC_ROM_SLOT);
            g_svc->log(", bytes ");
            g_svc->log_hex((uint64_t)h[2] * 512u);
            g_svc->log("\n");
        }
    }
}

/* Step 3: memory window 0 onto the card's attribute memory, at the first
 * free 4 KB of the option-ROM area where the bridge is what answers. A free
 * page can still be decoded by something else first (the chipset shadows
 * in segments larger than a ROM), which shows as the window's addresses
 * reading what they held before it opened; the next free page is tried
 * then. The host address, or 0 when no page answers. */
static uint32_t open_attr(void)
{
    int32_t a;

    for (a = pcc_attr_pick(g_roms, 0); a >= 0;
         a = pcc_attr_pick(g_roms, (uint32_t)a + PCC_WIN_SIZE)) {
        volatile uint8_t *attr = (volatile uint8_t *)(uintptr_t)(uint32_t)a;
        uint8_t before[ATTR_CHECK];
        uint8_t w[6];
        unsigned i;
        int changed = 0;

        if (pcc_mem_window((uint32_t)a, PCC_WIN_SIZE, 0, 1, w) != 0)
            return 0;
        for (i = 0; i < ATTR_CHECK; i++)
            before[i] = attr[2u * i];
        for (i = 0; i < 6u; i++)
            exca_wr(X_MEM0 + i, w[i]);
        exca_wr(X_PAGE0, 0);
        exca_wr(X_ENABLE, EN_MEM0);
        io_delay_us(100);
        for (i = 0; i < ATTR_CHECK; i++)
            if (attr[2u * i] != before[i])
                changed = 1;
        if (changed) {
            log_v("attribute memory window at", (uint32_t)a);
            return (uint32_t)a;
        }
        log_v("the window reads what was there before it opened, at",
              (uint32_t)a);
        exca_wr(X_ENABLE, 0);
    }
    log_s("falcon-bios: PCC.MOD: no free address in the option-ROM area "
          "answers as the card\n");
    return 0;
}

/* Steps 3 to 5: the CIS, the configuration, the I/O window. 0 with the
 * card an ATA disk at `io`. */
static int configure(uint16_t io)
{
    volatile uint8_t *attr;
    uint8_t cis[CIS_BYTES];
    uint8_t w[6];
    pcc_cis c;
    unsigned i;
    uint32_t host = open_attr();

    if (host == 0)
        return -1;
    attr = (volatile uint8_t *)(uintptr_t)host;
    for (i = 0; i < CIS_BYTES; i++)
        cis[i] = attr[2u * i];       /* attribute memory: even bytes only */
    if (pcc_cis_parse(cis, sizeof cis, &c) != 0) {
        /* Which of two failures this is, bytes that never arrived (0xFF or
         * 0x00 throughout) or tuples the parser does not take, is told by
         * the bytes themselves: the first sixty-four even bytes, then the
         * first sixteen odd ones, go to serial. */
        log_s("falcon-bios: PCC.MOD: the card's CIS could not be read\n");
        for (i = 0; i < 64u; i += 4u)
            log_v("CIS bytes", ((uint32_t)cis[i] << 24) |
                               ((uint32_t)cis[i + 1u] << 16) |
                               ((uint32_t)cis[i + 2u] << 8) | cis[i + 3u]);
        for (i = 0; i < 16u; i += 4u)
            log_v("odd bytes",
                  ((uint32_t)attr[2u * i + 1u] << 24) | ((uint32_t)attr[2u * i + 3u] << 16) |
                  ((uint32_t)attr[2u * i + 5u] << 8) | attr[2u * i + 7u]);
        return -1;
    }
    if (c.funcid != FUNCID_FIXED_DISK) {
        log_s("falcon-bios: PCC.MOD: a card that is not a disk\n");
        return -1;
    }
    if (c.io_len != 0 || c.io_lines != 4u || c.config_base >= PCC_WIN_SIZE) {
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
    log_v("I/O window at", io);
    return 0;
}

/* ---- a BAR0 the BIOS left unassigned ---- */

#define RANGES_MAX 64u
static pcc_range g_ranges[RANGES_MAX];

static int add_range(unsigned *n, const pcc_range *r)
{
    if (*n == RANGES_MAX)
        return -1;
    g_ranges[(*n)++] = *r;
    return 0;
}

/* The BAR at `reg` of the function at `loc` as a range, when it is a
 * memory BAR assigned below 4 GB: its length by the size probe (all ones
 * written, the bits that stay clear are the size, the value put back).
 * The dwords the BAR took, 1 or 2, so the caller steps past a 64-bit one's
 * upper half; *got says whether a range came of it. */
static unsigned bar_range(const pci_loc *loc, uint8_t reg, pcc_range *out,
                          int *got)
{
    uint32_t v = pci_read32(loc->bus, loc->dev, loc->fn, reg);
    uint64_t base = v & ~0xFu;
    unsigned words = 1;
    uint32_t m;

    *got = 0;
    if ((v & 1u) != 0)
        return 1;                    /* I/O */
    if ((v & 6u) == 4u) {            /* 64-bit: the upper half next */
        words = 2;
        base |= (uint64_t)pci_read32(loc->bus, loc->dev, loc->fn,
                                     (uint8_t)(reg + 4u)) << 32;
    }
    pci_write32(loc->bus, loc->dev, loc->fn, reg, 0xFFFFFFFFu);
    m = pci_read32(loc->bus, loc->dev, loc->fn, reg) & ~0xFu;
    pci_write32(loc->bus, loc->dev, loc->fn, reg, v);
    if (m == 0 || base == 0 || base >= 0x100000000ull)
        return words;                /* not implemented, unassigned, high */
    out->base = (uint32_t)base;
    out->len = ~m + 1u;
    *got = 1;
    return words;
}

/* A bridge's memory window [base, limit]; none when the limit is below the
 * base, which is how a BIOS leaves a window it did not open. */
static int window_range(uint32_t base, uint32_t limit, pcc_range *out)
{
    if (base == 0 || limit < base)
        return 0;
    out->base = base;
    out->len = (uint32_t)((uint64_t)limit - base + 1u);
    return 1;
}

/* Every memory range a function on the bus decodes: its memory BARs, and
 * the windows of a PCI-to-PCI (header type 1) or CardBus (type 2) bridge.
 * The count, or -1 when the table is too small for the bus. */
static int collect_ranges(void)
{
    unsigned nth;
    unsigned n = 0;

    for (nth = 0; ; nth++) {
        pci_loc loc;
        uint32_t hdr;
        unsigned nbars;
        uint8_t reg;
        uint32_t w;
        pcc_range r;

        if (pci_find_class_mask(0, 0, nth, &loc) != 0)
            break;
        hdr = (pci_read32(loc.bus, loc.dev, loc.fn, 0x0C) >> 16) & 0x7Fu;
        nbars = hdr == 0 ? 6u : hdr == 1u ? 2u : hdr == 2u ? 1u : 0;
        for (reg = 0x10; reg < 0x10u + 4u * nbars; ) {
            int got;
            unsigned words = bar_range(&loc, reg, &r, &got);

            if (got && add_range(&n, &r) != 0)
                return -1;
            reg = (uint8_t)(reg + 4u * words);
        }
        if (hdr == 1u) {             /* 1 MB units; prefetchable may be 64-bit */
            w = pci_read32(loc.bus, loc.dev, loc.fn, 0x20);
            if (window_range((w & 0xFFF0u) << 16, (w & 0xFFF00000u) | 0xFFFFFu,
                             &r) && add_range(&n, &r) != 0)
                return -1;
            w = pci_read32(loc.bus, loc.dev, loc.fn, 0x24);
            if (pci_read32(loc.bus, loc.dev, loc.fn, 0x28) == 0 &&
                window_range((w & 0xFFF0u) << 16, (w & 0xFFF00000u) | 0xFFFFFu,
                             &r) && add_range(&n, &r) != 0)
                return -1;
        } else if (hdr == 2u) {      /* 4 KB units */
            if (window_range(pci_read32(loc.bus, loc.dev, loc.fn, 0x1C) & ~0xFFFu,
                             pci_read32(loc.bus, loc.dev, loc.fn, 0x20) | 0xFFFu,
                             &r) && add_range(&n, &r) != 0)
                return -1;
            if (window_range(pci_read32(loc.bus, loc.dev, loc.fn, 0x24) & ~0xFFFu,
                             pci_read32(loc.bus, loc.dev, loc.fn, 0x28) | 0xFFFu,
                             &r) && add_range(&n, &r) != 0)
                return -1;
        }
    }
    return (int)n;
}

/* BAR0 left at 0 by a BIOS that does not know CardBus: a 4 KB page above
 * RAM and every range on the bus (pcc_bar_pick), written and read back.
 * The page, or 0 when there is none or the function does not take it. */
static uint32_t assign_bar0(const pci_loc *loc)
{
    int n = collect_ranges();
    uint32_t page;
    uint32_t rd;

    if (n < 0) {
        log_s("falcon-bios: PCC.MOD: more memory ranges on the bus than the "
              "table holds; the socket registers stay unassigned\n");
        return 0;
    }
    page = pcc_bar_pick(g_ranges, (unsigned)n,
                        g_svc != (const jmod_services *)0 ? g_svc->mem_top : 0);
    if (page == 0) {
        log_s("falcon-bios: PCC.MOD: no free page for the socket registers\n");
        return 0;
    }
    pci_write32(loc->bus, loc->dev, loc->fn, 0x10, page);
    rd = pci_read32(loc->bus, loc->dev, loc->fn, 0x10);
    if ((rd & ~0xFu) != page) {
        log_v("the socket registers did not take their page, BAR0", rd);
        return 0;
    }
    log_v("socket registers assigned at", page);
    return page;
}

static unsigned pcc_probe(void)
{
    unsigned nth;
    int roms_read = 0;

    g_ndev = 0;
    for (nth = 0; nth < SOCKETS_MAX && g_ndev < SOCKETS_MAX; nth++) {
        pci_loc loc;
        uint32_t bar;
        uint32_t cmd;
        uint16_t io;
        pcc_dev *d = &g_dev[g_ndev];

        if (pci_find_class(0x060700u, nth, &loc) != 0)
            break;
        bar = pci_read32(loc.bus, loc.dev, loc.fn, 0x10);
        log_v("CardBus bridge at bus/dev/fn",
              ((uint32_t)loc.bus << 16) | ((uint32_t)loc.dev << 8) | loc.fn);
        log_v("its socket registers (BAR0)", bar);
        if ((bar & 1u) != 0)
            continue;                /* an I/O range: not socket registers */
        if ((bar & ~0xFFFu) == 0) {
            bar = assign_bar0(&loc);
            if (bar == 0)
                continue;
        }
        if (!roms_read) {
            read_roms();
            roms_read = 1;
        }
        cmd = pci_read32(loc.bus, loc.dev, loc.fn, 0x04);
        pci_write32(loc.bus, loc.dev, loc.fn, 0x04,
                    (cmd & 0xFFFFu) | 0x0003u);
        g_regs = bar & ~0xFFFu;

        if (power_up() != 0)
            continue;
        io = io_pick();
        if (io == 0 || configure(io) != 0) {
            power_down();
            continue;
        }
        memset(&d->disk, 0, sizeof d->disk);
        d->disk.base = io;
        d->disk.ctrl = (uint16_t)(io + 0x0Eu);
        d->regs = g_regs;
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

static void pcc_release(void)
{
    unsigned i;

    for (i = 0; i < g_ndev; i++) {
        g_regs = g_dev[i].regs;
        power_down();
    }
    g_ndev = 0;
    g_serve = SOCKETS_MAX;
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
    ops->release = pcc_release;
    return 0;
}

JMOD_HEADER("PCC", JMOD_SLOT_PCC, pcc_init);
