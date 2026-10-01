/* falcon jump's OHCI.MOD: the Open Host Controller, USB 1.1's other
 * controller (DESIGN Section 13.4), as a usbhc for msc.c and hid.c.
 *
 * What the chipsets that are not Intel's or VIA's put on the board (SiS, ALi,
 * OPTi), and most add-in cards (NEC); QEMU's pci-ohci.
 *
 * Registers are memory mapped (BAR0), which with paging off is an ordinary
 * pointer. One endpoint descriptor for control and one for bulk, each put on
 * its list for one transfer at a time and polled until the controller has
 * retired its transfer descriptors; no interrupts, no periodic list. A
 * keyboard's interrupt endpoint goes on the bulk list the same way, for a
 * few frames at a time, and is taken off again with nothing pending. Unlike
 * UHCI, a transfer descriptor carries up to a page of data, not one packet,
 * and the controller keeps a bulk endpoint's data toggle in the endpoint
 * descriptor itself.
 *
 * TAKING THE CONTROLLER FROM THE BIOS. A BIOS that runs this controller from
 * SMM sets InterruptRouting; an ownership change request (OCR) asks it to
 * let go, and the controller's legacy emulation (HceControl, at 0x100 when
 * the revision register says it exists) is switched off, as Linux's
 * ohci-hcd does.
 */

#include <hw/usb/ohci.h>
#include <hw/modrt.h>
#include <hw/pci.h>

#include <lib/string.h>

#define HC_REV      0x00u
#define HC_CONTROL  0x04u
#define HC_CMDSTAT  0x08u
#define HC_INTSTAT  0x0Cu
#define HC_INTDIS   0x14u
#define HC_HCCA     0x18u
#define HC_CTRLHEAD 0x20u
#define HC_CTRLCUR  0x24u
#define HC_BULKHEAD 0x28u
#define HC_BULKCUR  0x2Cu
#define HC_FMINT    0x34u
#define HC_PERSTART 0x40u
#define HC_RHDA     0x48u
#define HC_RHSTAT   0x50u
#define HC_RHPORT   0x54u
#define HC_LEGACY   0x100u

#define REV_LEGACY  0x100u

#define CTL_CBSR4   0x003u
#define CTL_CLE     0x010u
#define CTL_BLE     0x020u
#define CTL_OPER    0x080u
#define CTL_HCFS    0x0C0u
#define CTL_IR      0x100u

#define CMD_HCR     0x1u
#define CMD_CLF     0x2u
#define CMD_BLF     0x4u
#define CMD_OCR     0x8u

#define RHDA_NDP    0x0FFu
#define RHDA_NPS    (1u << 9)
#define RHDA_POTPGT(v) (((v) >> 24) & 0xFFu)
#define RHS_LPSC    (1u << 16)

#define PORT_CCS    (1u << 0)
#define PORT_PES    (1u << 1)
#define PORT_PRS    (1u << 4)
#define PORT_PPS    (1u << 8)
#define PORT_LSDA   (1u << 9)
#define PORT_PRSC   (1u << 20)
#define PORT_CHANGES 0x001F0000u

#define ED_SKIP     (1u << 14)
#define ED_LOW      (1u << 13)
#define ED_HALTED   0x1u
#define ED_CARRY    0x2u

#define TD_ROUND    (1u << 18)
#define TD_SETUP    (0u << 19)
#define TD_OUT      (1u << 19)
#define TD_IN       (2u << 19)
#define TD_NOINT    (7u << 21)
#define TD_T_DATA0  (2u << 24)
#define TD_T_DATA1  (3u << 24)
#define TD_CC(v)    ((v) >> 28)
#define TD_CC_NONE  0xFu
#define CC_STALL    4u
#define CC_UNDERRUN 9u

#define OHCI_MAX    4u
#define PORTS_MAX   15u
#define TD_MAX      32u
#define WAIT_XFER   500000u      /* 10 us steps: 5 s */
#define WAIT_INTR   300u         /* 10 us steps: three frames */
#define FRAME_US    1000u

typedef struct {
    volatile uint32_t ctl;
    volatile uint32_t tail;
    volatile uint32_t head;
    volatile uint32_t next;
} ohci_ed;

typedef struct {
    volatile uint32_t ctl;
    volatile uint32_t cbp;
    volatile uint32_t next;
    volatile uint32_t be;
} ohci_td;

static uint8_t g_hcca[OHCI_MAX][256] __attribute__((aligned(256)));
static ohci_ed g_ed_ctrl[OHCI_MAX] __attribute__((aligned(16)));
static ohci_ed g_ed_bulk[OHCI_MAX] __attribute__((aligned(16)));
static ohci_td g_td[TD_MAX + 1u] __attribute__((aligned(16)));
static volatile uint8_t *g_bases[OHCI_MAX];
static unsigned g_nports[OHCI_MAX];
static unsigned g_nctl;

/* The controller a call uses, and its root port there. */
static volatile uint8_t *g_mm;
static unsigned g_cur;

static uint32_t phys(const volatile void *p)
{
    return (uint32_t)(uintptr_t)p;
}

static uint32_t rd(uint32_t reg)
{
    return *(volatile uint32_t *)(g_mm + reg);
}

static void wr(uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(g_mm + reg) = v;
}

/* Port `port`, numbered across every controller, as its controller and its
 * index there. -1 past the last. */
static int use(unsigned port, unsigned *local)
{
    unsigned c;

    for (c = 0; c < g_nctl; c++) {
        if (port < g_nports[c]) {
            g_mm = g_bases[c];
            g_cur = c;
            *local = port;
            return 0;
        }
        port -= g_nports[c];
    }
    return -1;
}

static int start_one(const pci_loc *loc, unsigned c)
{
    uint32_t bar = pci_read32(loc->bus, loc->dev, loc->fn, 0x10);
    uint32_t cmd;
    uint32_t fi;
    uint32_t rhda;
    unsigned i;

    if ((bar & 1u) != 0 || (bar & ~0xFu) == 0)
        return -1;                   /* not memory mapped */
    g_mm = (volatile uint8_t *)(uintptr_t)(bar & ~0xFu);
    g_bases[c] = g_mm;
    cmd = pci_read32(loc->bus, loc->dev, loc->fn, 0x04);
    pci_write32(loc->bus, loc->dev, loc->fn, 0x04, (cmd & 0xFFFFu) | 0x0006u);

    /* From the BIOS: SMM first, then the legacy emulation. */
    if ((rd(HC_CONTROL) & CTL_IR) != 0) {
        wr(HC_CMDSTAT, CMD_OCR);
        for (i = 0; i < 100000u && (rd(HC_CONTROL) & CTL_IR) != 0; i++)
            io_delay_us(10);
    }
    if ((rd(HC_REV) & REV_LEGACY) != 0)
        wr(HC_LEGACY, 0);

    fi = rd(HC_FMINT) & 0x3FFFu;
    if (fi == 0)
        fi = 11999u;
    wr(HC_INTDIS, 0xFFFFFFFFu);
    wr(HC_CMDSTAT, CMD_HCR);
    for (i = 0; i < 1000u && (rd(HC_CMDSTAT) & CMD_HCR) != 0; i++)
        io_delay_us(10);

    /* Out of reset the controller is suspended and has two milliseconds to
     * be told otherwise. */
    memset(g_hcca[c], 0, sizeof g_hcca[c]);
    g_ed_ctrl[c].ctl = ED_SKIP;
    g_ed_ctrl[c].head = 0;
    g_ed_ctrl[c].tail = 0;
    g_ed_ctrl[c].next = 0;
    g_ed_bulk[c] = g_ed_ctrl[c];
    wr(HC_HCCA, phys(g_hcca[c]));
    wr(HC_CTRLHEAD, phys(&g_ed_ctrl[c]));
    wr(HC_BULKHEAD, phys(&g_ed_bulk[c]));
    wr(HC_CTRLCUR, 0);
    wr(HC_BULKCUR, 0);
    wr(HC_INTSTAT, 0xFFFFFFFFu);
    wr(HC_FMINT, ((rd(HC_FMINT) & 0x80000000u) ^ 0x80000000u) |
                 (((6u * (fi - 210u)) / 7u) << 16) | fi);
    wr(HC_PERSTART, fi * 9u / 10u);
    wr(HC_CONTROL, CTL_OPER | CTL_CBSR4 | CTL_CLE | CTL_BLE);

    rhda = rd(HC_RHDA);
    g_nports[c] = rhda & RHDA_NDP;
    if (g_nports[c] > PORTS_MAX)
        g_nports[c] = PORTS_MAX;
    if ((rhda & RHDA_NPS) == 0) {
        wr(HC_RHSTAT, RHS_LPSC);
        for (i = 0; i < g_nports[c]; i++)
            wr(HC_RHPORT + i * 4u, PORT_PPS);
    }
    io_delay_us((RHDA_POTPGT(rhda) * 2u + 20u) * 1000u);
    return 0;
}

static unsigned ohci_start(void)
{
    pci_loc loc;
    unsigned nth;
    unsigned ports = 0;

    g_nctl = 0;
    for (nth = 0; nth < OHCI_MAX; nth++) {
        if (pci_find_class(0x0C0310u, nth, &loc) != 0)
            break;
        if (start_one(&loc, g_nctl) == 0)
            ports += g_nports[g_nctl++];
    }
    return ports;
}

static int ohci_port_reset(unsigned port, int *low)
{
    unsigned p;
    uint32_t reg;
    uint32_t v;
    unsigned i;

    if (use(port, &p) != 0)
        return -1;
    reg = HC_RHPORT + p * 4u;
    wr(reg, PORT_CHANGES);
    if ((rd(reg) & PORT_CCS) == 0)
        return 0;
    wr(reg, PORT_PRS);
    for (i = 0; i < 20000u && (rd(reg) & PORT_PRSC) == 0; i++)
        io_delay_us(10);
    wr(reg, PORT_CHANGES);
    v = rd(reg);
    if ((v & PORT_PES) == 0)
        return -1;
    *low = (v & PORT_LSDA) != 0;
    io_delay_us(10000);              /* reset recovery */
    return 1;
}

static void td_fill(ohci_td *t, uint32_t flags, void *buf, uint32_t len)
{
    t->ctl = flags | TD_NOINT | (TD_CC_NONE << 28);
    t->cbp = len != 0 ? phys(buf) : 0;
    t->be = len != 0 ? phys(buf) + len - 1u : 0;
    t->next = phys(t + 1);
}

/* Runs g_td[0..n) on `ed` (list `list`, CLF or BLF) and waits. *done the
 * bytes moved; a short IN TD ends the transfer. The TD after the last is the
 * dummy the endpoint's tail points at. */
static int run(ohci_ed *ed, uint32_t list, unsigned n, const uint32_t *lens,
               uint32_t *done)
{
    uint32_t steps = WAIT_XFER;
    uint32_t tail = phys(&g_td[n]);
    unsigned i = 0;
    int rc = USBHC_ERR;

    memset((void *)&g_td[n], 0, sizeof g_td[n]);
    *done = 0;
    ed->tail = tail;
    ed->head = phys(&g_td[0]) | (ed->head & ED_CARRY);
    ed->ctl &= ~ED_SKIP;
    wr(HC_CMDSTAT, list);

    while (steps-- > 0) {
        uint32_t head = ed->head;
        uint32_t at = head & ~0xFu;

        /* Every TD before the one the head points at has been retired. */
        while (i < n && at != phys(&g_td[i])) {
            uint32_t cc = TD_CC(g_td[i].ctl);

            if (cc == CC_STALL) {
                rc = USBHC_STALL;
                goto out;
            }
            if (cc != 0 && cc != CC_UNDERRUN) {
                rc = USBHC_ERR;
                goto out;
            }
            if (g_td[i].cbp != 0) {
                /* Short: cbp is where the data stopped. The rest of a bulk
                 * read is not coming, so the transfer ends here; a control
                 * transfer's status stage still runs, which is an OUT. */
                *done += g_td[i].cbp - (g_td[i].be + 1u - lens[i]);
                i++;
                if (i < n && (g_td[i].ctl & (3u << 19)) == TD_IN) {
                    rc = USBHC_OK;
                    goto out;
                }
            } else {
                *done += lens[i];
                i++;
            }
        }
        if (i == n) {
            rc = USBHC_OK;
            goto out;
        }
        if ((head & ED_HALTED) != 0) {
            rc = TD_CC(g_td[i].ctl) == CC_STALL ? USBHC_STALL : USBHC_ERR;
            goto out;
        }
        io_delay_us(10);
    }
out:
    ed->ctl |= ED_SKIP;
    ed->head = tail | (ed->head & ED_CARRY);
    return rc;
}

static int ohci_control(unsigned port, uint8_t addr, uint8_t mps0, int low,
                        const uint8_t setup[8], void *data, uint16_t len)
{
    static uint8_t s[8];
    static uint32_t lens[TD_MAX];
    int in = (setup[0] & 0x80u) != 0;
    uint8_t *p = data;
    uint32_t off = 0;
    uint32_t done;
    unsigned n = 0;
    unsigned local;
    ohci_ed *ed;

    if (mps0 == 0 || use(port, &local) != 0)
        return USBHC_ERR;
    ed = &g_ed_ctrl[g_cur];
    ed->ctl = ED_SKIP | (uint32_t)(addr & 0x7Fu) | (low ? ED_LOW : 0u) |
              ((uint32_t)mps0 << 16);
    memcpy(s, setup, 8);
    td_fill(&g_td[n], TD_SETUP | TD_T_DATA0, s, 8);
    lens[n++] = 8;
    if (len != 0) {
        /* One TD for the data stage: a descriptor request is short, and a
         * TD carries up to two pages. */
        td_fill(&g_td[n], (in ? TD_IN : TD_OUT) | TD_T_DATA1 | TD_ROUND,
                p + off, len);
        lens[n++] = len;
    }
    td_fill(&g_td[n], ((in && len != 0) ? TD_OUT : TD_IN) | TD_T_DATA1, 0, 0);
    lens[n++] = 0;
    return run(ed, CMD_CLF, n, lens, &done);
}

static int ohci_bulk(unsigned port, uint8_t addr, uint8_t ep, uint16_t mps,
                     void *data, uint32_t len, uint32_t *done,
                     uint8_t *toggle)
{
    static uint32_t lens[TD_MAX];
    int in = (ep & 0x80u) != 0;
    uint8_t *p = data;
    uint32_t off = 0;
    unsigned n = 0;
    unsigned local;
    ohci_ed *ed;
    int rc;

    *done = 0;
    if (mps == 0 || use(port, &local) != 0)
        return USBHC_ERR;
    ed = &g_ed_bulk[g_cur];
    ed->ctl = ED_SKIP | (uint32_t)(addr & 0x7Fu) |
              ((uint32_t)(ep & 0x0Fu) << 7) | ((uint32_t)mps << 16);
    ed->head = (*toggle & 1u) ? ED_CARRY : 0u;
    do {
        uint32_t chunk = len - off;

        if (chunk > 4096u)
            chunk = 4096u;
        td_fill(&g_td[n], (in ? TD_IN : TD_OUT) | TD_ROUND, p + off, chunk);
        lens[n++] = chunk;
        off += chunk;
    } while (off < len && n < TD_MAX);
    if (off < len)
        return USBHC_ERR;            /* more than one call carries */
    rc = run(ed, CMD_BLF, n, lens, done);
    *toggle = (ed->head & ED_CARRY) != 0 ? 1u : 0u;
    return rc;
}

static int ohci_intr(unsigned port, uint8_t addr, uint8_t ep, uint16_t mps,
                     int low, void *data, uint32_t len, uint32_t *done,
                     uint8_t *toggle)
{
    uint32_t td0 = phys(&g_td[0]);
    uint32_t tail = phys(&g_td[1]);
    uint32_t steps;
    uint32_t cc;
    unsigned local;
    ohci_ed *ed;
    int rc;

    *done = 0;
    if (mps == 0 || len == 0 || len > mps || use(port, &local) != 0)
        return USBHC_ERR;
    ed = &g_ed_bulk[g_cur];
    ed->ctl = ED_SKIP | (uint32_t)(addr & 0x7Fu) |
              ((uint32_t)(ep & 0x0Fu) << 7) | (low ? ED_LOW : 0u) |
              ((uint32_t)mps << 16);
    td_fill(&g_td[0], TD_IN | TD_ROUND, data, len);
    memset((void *)&g_td[1], 0, sizeof g_td[1]);
    ed->tail = tail;
    ed->head = td0 | ((*toggle & 1u) ? ED_CARRY : 0u);
    ed->ctl &= ~ED_SKIP;
    wr(HC_CMDSTAT, CMD_BLF);
    for (steps = 0; steps < WAIT_INTR &&
                    (ed->head & ~0xFu) == td0 && (ed->head & ED_HALTED) == 0;
         steps++)
        io_delay_us(10);
    ed->ctl |= ED_SKIP;
    /* The controller finishes a transaction it had started within the
     * frame; after that it reads the skipped descriptor no further. */
    if ((ed->head & ~0xFu) == td0 && (ed->head & ED_HALTED) == 0)
        io_delay_us(FRAME_US);

    cc = TD_CC(g_td[0].ctl);
    if ((ed->head & ~0xFu) == td0 && (ed->head & ED_HALTED) == 0)
        rc = USBHC_NAK;
    else if (cc == CC_STALL)
        rc = USBHC_STALL;
    else if (cc != 0 && cc != CC_UNDERRUN)
        rc = USBHC_ERR;
    else {
        *done = g_td[0].cbp != 0 ? g_td[0].cbp - phys(data) : len;
        rc = USBHC_OK;
    }
    *toggle = (ed->head & ED_CARRY) != 0 ? 1u : 0u;
    ed->head = tail | (ed->head & ED_CARRY);
    return rc;
}

const usbhc ohci_hc = {
    ohci_start, ohci_port_reset, ohci_control, ohci_bulk, ohci_intr,
    (void (*)(unsigned))0
};
