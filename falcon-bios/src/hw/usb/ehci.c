/* falcon jump's EHCI.MOD: the Enhanced Host Controller, USB 2.0 (DESIGN
 * Section 13.4), as a usbhc for msc.c.
 *
 * On every chipset from about 2002 beside one or more UHCI or OHCI
 * companions, which run the full- and low-speed devices on the same
 * physical ports; QEMU's usb-ehci.
 *
 * ONE QUEUE HEAD, ALONE ON THE ASYNCHRONOUS LIST, the head of reclamation
 * and linked to itself. A transfer is a chain of queue transfer descriptors
 * hung from its overlay and polled until the last is retired; the schedule
 * is paused while the queue head is edited for the next endpoint, which is
 * the one change to it a controller does not see coherently. No periodic
 * schedule, no interrupts. The structures are sized for the 64-bit layout
 * with its high address words zero, so they serve both kinds of controller.
 *
 * TAKING THE CONTROLLER FROM THE BIOS goes through PCI, not the controller:
 * the USBLEGSUP extended capability's OS-owned semaphore is set and the
 * BIOS-owned one waited out, and the legacy SMIs switched off (USBLEGCTLSTS),
 * as Linux's ehci-hcd does.
 *
 * A DEVICE THAT IS NOT HIGH SPEED does not enable on an EHCI port after a
 * reset. The port is handed to its companion controller (the port owner
 * bit), and UHCI.MOD or OHCI.MOD, tried after this one, finds it there
 * (the USB route's order, jump/routes.c). So is a port holding a hub or a
 * keyboard, once msc.c has read what it is (`release`): this controller has
 * no interrupt transfers, and a keyboard behind a high-speed hub would need
 * split transactions, where on the companion the hub runs at full speed
 * and its keyboard is reached as one on a root port is.
 */

#include <hw/usb/ehci.h>
#include <hw/modrt.h>
#include <hw/pci.h>

#include <lib/string.h>

/* Capability registers. */
#define CAP_LENGTH   0x00u
#define CAP_HCSPARAMS 0x04u
#define CAP_HCCPARAMS 0x08u

/* Operational registers, from the base plus CAPLENGTH. */
#define OP_CMD       0x00u
#define OP_STS       0x04u
#define OP_INTR      0x08u
#define OP_SEGMENT   0x10u
#define OP_ASYNC     0x18u
#define OP_CONFIG    0x40u
#define OP_PORT      0x44u

#define CMD_RS       (1u << 0)
#define CMD_HCRESET  (1u << 1)
#define CMD_ASE      (1u << 5)
#define CMD_ITC8     (8u << 16)
#define STS_HALTED   (1u << 12)
#define STS_ASS      (1u << 15)

#define HCS_NPORTS(v) ((v) & 0xFu)
#define HCS_PPC      (1u << 4)
#define HCC_EECP(v)  (((v) >> 8) & 0xFFu)

#define PORT_CCS     (1u << 0)
#define PORT_CSC     (1u << 1)
#define PORT_PE      (1u << 2)
#define PORT_PEC     (1u << 3)
#define PORT_OCC     (1u << 5)
#define PORT_PR      (1u << 8)
#define PORT_LS_K    (1u << 10)      /* line status K: a low-speed device */
#define PORT_LS_MASK (3u << 10)
#define PORT_PP      (1u << 12)
#define PORT_OWNER   (1u << 13)
#define PORT_W1C     (PORT_CSC | PORT_PEC | PORT_OCC)

#define LEGSUP_BIOS  (1u << 16)
#define LEGSUP_OS    (1u << 24)

#define LINK_T       1u
#define LINK_QH      (1u << 1)

#define TOK_ACTIVE   (1u << 7)
#define TOK_HALTED   (1u << 6)
#define TOK_ERRORS   0x7Cu           /* halted .. missed microframe */
#define TOK_OUT      (0u << 8)
#define TOK_IN       (1u << 8)
#define TOK_SETUP    (2u << 8)
#define TOK_CERR3    (3u << 10)
#define TOK_BYTES(v) (((v) >> 16) & 0x7FFFu)
#define TOK_TOGGLE   (1u << 31)

#define QH_EPS_HIGH  (2u << 12)
#define QH_DTC       (1u << 14)
#define QH_HEAD      (1u << 15)
#define QH_MULT1     (1u << 30)

#define EHCI_MAX     4u
#define PORTS_MAX    15u
#define QTD_MAX      8u
#define QTD_BYTES    0x4000u         /* 16 KB a qTD: four whole pages */
#define WAIT_XFER    500000u         /* 10 us steps: 5 s */

typedef struct {
    volatile uint32_t next;
    volatile uint32_t alt;
    volatile uint32_t token;
    volatile uint32_t buf[5];
    volatile uint32_t buf_hi[5];
    uint32_t pad[3];
} ehci_qtd;

typedef struct {
    volatile uint32_t link;
    volatile uint32_t chars;
    volatile uint32_t caps;
    volatile uint32_t cur;
    /* The overlay: the qTD the controller is working on. */
    volatile uint32_t next;
    volatile uint32_t alt;
    volatile uint32_t token;
    volatile uint32_t buf[5];
    volatile uint32_t buf_hi[5];
    uint32_t pad[7];
} ehci_qh;

static ehci_qh  g_qhs[EHCI_MAX] __attribute__((aligned(32)));
static ehci_qtd g_qtd[QTD_MAX] __attribute__((aligned(32)));
static volatile uint8_t *g_ops[EHCI_MAX];
static unsigned g_nports[EHCI_MAX];
static unsigned g_nctl;

static volatile uint8_t *g_op;
static unsigned g_cur;

static uint32_t phys(const volatile void *p)
{
    return (uint32_t)(uintptr_t)p;
}

static uint32_t rd(uint32_t reg)
{
    return *(volatile uint32_t *)(g_op + reg);
}

static void wr(uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(g_op + reg) = v;
}

static int use(unsigned port, unsigned *local)
{
    unsigned c;

    for (c = 0; c < g_nctl; c++) {
        if (port < g_nports[c]) {
            g_op = g_ops[c];
            g_cur = c;
            *local = port;
            return 0;
        }
        port -= g_nports[c];
    }
    return -1;
}

/* The BIOS's hold on the controller, through USBLEGSUP. */
static void take_from_bios(const pci_loc *loc, uint32_t eecp)
{
    unsigned hops = 0;

    while (eecp >= 0x40u && hops++ < 16u) {
        uint32_t cap = pci_read32(loc->bus, loc->dev, loc->fn,
                                  (uint8_t)eecp);
        unsigned i;

        if ((cap & 0xFFu) == 1u) {
            pci_write32(loc->bus, loc->dev, loc->fn, (uint8_t)eecp,
                        cap | LEGSUP_OS);
            for (i = 0; i < 100000u; i++) {
                cap = pci_read32(loc->bus, loc->dev, loc->fn, (uint8_t)eecp);
                if ((cap & LEGSUP_BIOS) == 0)
                    break;
                io_delay_us(10);
            }
            /* The SMIs off, and their pending bits cleared. */
            pci_write32(loc->bus, loc->dev, loc->fn, (uint8_t)(eecp + 4u),
                        0xE0000000u);
            return;
        }
        eecp = (cap >> 8) & 0xFFu;
    }
}

static int start_one(const pci_loc *loc, unsigned c)
{
    uint32_t bar = pci_read32(loc->bus, loc->dev, loc->fn, 0x10);
    volatile uint8_t *base;
    uint32_t cmd;
    uint32_t hcs;
    unsigned i;
    ehci_qh *qh = &g_qhs[c];

    /* A memory BAR; a 64-bit one only with its high half zero, since paging
     * is off and nothing above 4 GiB can be reached. */
    if ((bar & 1u) != 0 || (bar & ~0xFu) == 0 ||
        ((bar & 0x6u) == 0x4u &&
         pci_read32(loc->bus, loc->dev, loc->fn, 0x14) != 0))
        return -1;
    base = (volatile uint8_t *)(uintptr_t)(bar & ~0xFu);
    cmd = pci_read32(loc->bus, loc->dev, loc->fn, 0x04);
    pci_write32(loc->bus, loc->dev, loc->fn, 0x04, (cmd & 0xFFFFu) | 0x0006u);
    take_from_bios(loc, HCC_EECP(*(volatile uint32_t *)(base +
                                                         CAP_HCCPARAMS)));

    g_op = base + base[CAP_LENGTH];
    g_ops[c] = g_op;
    hcs = *(volatile uint32_t *)(base + CAP_HCSPARAMS);
    g_nports[c] = HCS_NPORTS(hcs);
    if (g_nports[c] > PORTS_MAX)
        g_nports[c] = PORTS_MAX;

    wr(OP_CMD, rd(OP_CMD) & ~CMD_RS);
    for (i = 0; i < 1000u && (rd(OP_STS) & STS_HALTED) == 0; i++)
        io_delay_us(10);
    wr(OP_CMD, CMD_HCRESET);
    for (i = 0; i < 100000u && (rd(OP_CMD) & CMD_HCRESET) != 0; i++)
        io_delay_us(10);

    memset((void *)qh, 0, sizeof *qh);
    qh->link = phys(qh) | LINK_QH;
    qh->chars = QH_HEAD;
    qh->caps = QH_MULT1;
    qh->next = LINK_T;
    qh->alt = LINK_T;
    qh->token = TOK_HALTED;          /* nothing to do until a transfer */

    wr(OP_INTR, 0);
    wr(OP_SEGMENT, 0);
    wr(OP_ASYNC, phys(qh));
    wr(OP_CMD, CMD_ITC8 | CMD_RS);
    wr(OP_CONFIG, 1);                /* every port to this controller */
    io_delay_us(5000);

    if ((hcs & HCS_PPC) != 0) {
        for (i = 0; i < g_nports[c]; i++) {
            uint32_t v = rd(OP_PORT + i * 4u);

            wr(OP_PORT + i * 4u, (v & ~PORT_W1C) | PORT_PP);
        }
        io_delay_us(20000);
    }
    return 0;
}

static unsigned ehci_start(void)
{
    pci_loc loc;
    unsigned nth;
    unsigned ports = 0;

    g_nctl = 0;
    for (nth = 0; nth < EHCI_MAX; nth++) {
        if (pci_find_class(0x0C0320u, nth, &loc) != 0)
            break;
        if (start_one(&loc, g_nctl) == 0)
            ports += g_nports[g_nctl++];
    }
    return ports;
}

static void release(uint32_t reg)
{
    uint32_t v = rd(reg);

    wr(reg, (v & ~PORT_W1C) | PORT_OWNER);
}

static int ehci_port_reset(unsigned port, int *low)
{
    unsigned p;
    uint32_t reg;
    uint32_t v;
    unsigned i;

    if (use(port, &p) != 0)
        return -1;
    reg = OP_PORT + p * 4u;
    v = rd(reg);
    if ((v & PORT_OWNER) != 0 || (v & PORT_CCS) == 0)
        return 0;
    /* A low-speed device shows before any reset, and belongs to the
     * companion outright. */
    if ((v & PORT_LS_MASK) == PORT_LS_K) {
        release(reg);
        return 0;
    }
    wr(reg, ((v & ~PORT_W1C) & ~PORT_PE) | PORT_PR);
    io_delay_us(50000);
    wr(reg, rd(reg) & ~(PORT_W1C | PORT_PR));
    for (i = 0; i < 1000u && (rd(reg) & PORT_PR) != 0; i++)
        io_delay_us(10);
    v = rd(reg);
    if ((v & PORT_PE) == 0) {
        /* Full speed: the companion's. */
        release(reg);
        return 0;
    }
    *low = 0;
    io_delay_us(10000);              /* reset recovery */
    return 1;
}

/* The async schedule stopped or started, and waited for. */
static void async(int on)
{
    unsigned i;
    uint32_t cmd = rd(OP_CMD);

    wr(OP_CMD, on ? (cmd | CMD_ASE) : (cmd & ~CMD_ASE));
    for (i = 0; i < 100000u; i++) {
        if (((rd(OP_STS) & STS_ASS) != 0) == (on != 0))
            return;
        io_delay_us(10);
    }
}

static void qtd_fill(ehci_qtd *t, ehci_qtd *next, uint32_t pid,
                     uint8_t toggle, void *buf, uint32_t len)
{
    uint32_t a = phys(buf);
    unsigned i;

    memset((void *)t, 0, sizeof *t);
    t->next = next != (ehci_qtd *)0 ? phys(next) : LINK_T;
    t->alt = LINK_T;
    t->token = TOK_ACTIVE | TOK_CERR3 | pid | (len << 16) |
               (toggle ? TOK_TOGGLE : 0u);
    if (len != 0) {
        t->buf[0] = a;
        for (i = 1; i < 5u; i++)
            t->buf[i] = (a & ~0xFFFu) + i * 0x1000u;
    }
}

/* Runs g_qtd[0..n) on `qh` and waits. *done the bytes the qTDs moved; a
 * short IN whose alternate pointer ends the chain ends the transfer, and
 * one that points on (a control transfer's data stage, to its status stage)
 * does not. *last the index of the last qTD the controller retired, whose
 * token carries the data toggle the next transfer starts with. */
static int run(ehci_qh *qh, unsigned n, const uint32_t *lens, uint32_t *done,
               unsigned *last)
{
    uint32_t steps = WAIT_XFER;
    unsigned i = 0;
    int rc = USBHC_ERR;

    *done = 0;
    *last = n;
    qh->cur = 0;
    qh->alt = LINK_T;
    qh->next = phys(&g_qtd[0]);
    qh->token = 0;
    async(1);

    while (steps-- > 0) {
        uint32_t tok = g_qtd[i].token;

        if ((tok & TOK_ACTIVE) != 0) {
            if ((qh->token & TOK_HALTED) != 0)
                break;
            io_delay_us(10);
            continue;
        }
        if ((tok & TOK_HALTED) != 0) {
            rc = (tok & (TOK_ERRORS & ~TOK_HALTED)) == 0 ? USBHC_STALL
                                                          : USBHC_ERR;
            goto out;
        }
        *done += lens[i] - TOK_BYTES(tok);
        *last = i;
        if (TOK_BYTES(tok) != 0 && (tok & (3u << 8)) == TOK_IN &&
            g_qtd[i].alt == LINK_T) {
            rc = USBHC_OK;
            goto out;
        }
        if (TOK_BYTES(tok) != 0 && g_qtd[i].alt != LINK_T) {
            /* Short, and pointed on: the controller skips to alt. */
            i = (unsigned)(((uint32_t)(g_qtd[i].alt - phys(&g_qtd[0]))) /
                           sizeof g_qtd[0]);
        } else {
            i++;
        }
        if (i >= n) {
            rc = USBHC_OK;
            goto out;
        }
    }
    if ((qh->token & TOK_HALTED) != 0)
        rc = (qh->token & (TOK_ERRORS & ~TOK_HALTED)) == 0 ? USBHC_STALL
                                                            : USBHC_ERR;
out:
    async(0);
    qh->next = LINK_T;
    qh->token = TOK_HALTED;
    return rc;
}

static void qh_endpoint(ehci_qh *qh, uint8_t addr, uint8_t ep, uint16_t mps)
{
    qh->chars = QH_HEAD | QH_DTC | QH_EPS_HIGH | (uint32_t)(addr & 0x7Fu) |
                ((uint32_t)(ep & 0x0Fu) << 8) | ((uint32_t)mps << 16);
    qh->caps = QH_MULT1;
}

static int ehci_control(unsigned port, uint8_t addr, uint8_t mps0, int low,
                        const uint8_t setup[8], void *data, uint16_t len)
{
    static uint8_t s[8];
    static uint32_t lens[3];
    int in = (setup[0] & 0x80u) != 0;
    uint32_t done;
    unsigned n = 0;
    unsigned local;
    ehci_qh *qh;

    (void)low;
    if (mps0 == 0 || use(port, &local) != 0)
        return USBHC_ERR;
    qh = &g_qhs[g_cur];
    async(0);
    qh_endpoint(qh, addr, 0, mps0);
    memcpy(s, setup, 8);
    qtd_fill(&g_qtd[n], &g_qtd[n + 1u], TOK_SETUP, 0, s, 8);
    lens[n++] = 8;
    if (len != 0) {
        qtd_fill(&g_qtd[n], &g_qtd[n + 1u], in ? TOK_IN : TOK_OUT, 1, data,
                 len);
        lens[n++] = len;
    }
    qtd_fill(&g_qtd[n], (ehci_qtd *)0,
             (in && len != 0) ? TOK_OUT : TOK_IN, 1, (void *)0, 0);
    lens[n++] = 0;
    /* A short data stage still owes its status stage. */
    if (len != 0)
        g_qtd[1].alt = phys(&g_qtd[2]);
    {
        unsigned last;

        return run(qh, n, lens, &done, &last);
    }
}

static int ehci_bulk(unsigned port, uint8_t addr, uint8_t ep, uint16_t mps,
                     void *data, uint32_t len, uint32_t *done,
                     uint8_t *toggle)
{
    static uint32_t lens[QTD_MAX];
    int in = (ep & 0x80u) != 0;
    uint8_t *p = data;
    uint32_t off = 0;
    unsigned n = 0;
    unsigned local;
    unsigned last;
    ehci_qh *qh;
    int rc;

    *done = 0;
    if (mps == 0 || use(port, &local) != 0)
        return USBHC_ERR;
    qh = &g_qhs[g_cur];
    async(0);
    qh_endpoint(qh, addr, ep, mps);
    do {
        uint32_t chunk = len - off;
        /* A qTD's five buffer pointers cover four pages whole and the part
         * of a fifth the start is into: at most 16 KB from any start. */
        if (chunk > QTD_BYTES)
            chunk = QTD_BYTES;
        qtd_fill(&g_qtd[n], (ehci_qtd *)0, in ? TOK_IN : TOK_OUT, *toggle,
                 p + off, chunk);
        if (n > 0)
            g_qtd[n - 1u].next = phys(&g_qtd[n]);
        lens[n++] = chunk;
        /* The controller flips the toggle per packet inside a qTD, and the
         * next qTD's is what this one's would be after its last packet. */
        if (chunk != 0 && ((chunk + mps - 1u) / mps) % 2u != 0)
            *toggle ^= 1u;
        off += chunk;
    } while (off < len && n < QTD_MAX);
    if (off < len)
        return USBHC_ERR;
    rc = run(qh, n, lens, done, &last);
    /* With the toggle taken from the qTDs, the controller writes back into
     * each retired one the toggle its next packet would have used: after a
     * short or failed transfer, that is where the endpoint stands. */
    if (last < n)
        *toggle = (uint8_t)(g_qtd[last].token >> 31);
    return rc;
}

static void ehci_release(unsigned port)
{
    unsigned p;

    if (use(port, &p) == 0)
        release(OP_PORT + p * 4u);
}

const usbhc ehci_hc = {
    ehci_start, ehci_port_reset, ehci_control, ehci_bulk,
    (int (*)(unsigned, uint8_t, uint8_t, uint16_t, int, void *, uint32_t,
             uint32_t *, uint8_t *))0,
    ehci_release
};
