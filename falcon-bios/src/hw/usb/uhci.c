/* falcon jump's UHCI.MOD: the Universal Host Controller, USB 1.1's Intel
 * controller (DESIGN Section 14.4), as a usbhc for msc.c.
 *
 * The ThinkPad A21m's PIIX4 is one, and so is QEMU's piix3-usb-uhci; every
 * Intel and VIA chipset of the era has one or more.
 *
 * EACH CONTROLLER'S SCHEDULE IS ONE QUEUE HEAD. Every entry of its frame list
 * points at it, and a transfer is a chain of transfer descriptors hung from its
 * element pointer and polled until the controller has retired them. One
 * transfer at a time, no interrupts, no periodic traffic: all a bootloader's
 * INT 13h call needs, and nothing that could run while the resident is not
 * being called. The frame list, the queue head and the descriptors are in
 * this module's .bss, which with paging off is where the controller reads
 * them.
 *
 * TAKING THE CONTROLLER FROM THE BIOS. A BIOS with USB keyboard or disk
 * support runs this controller from its SMI handler. Writing 0x8F00 to the
 * legacy support register (PCI 0xC0) turns that off and clears what it left
 * pending, as Linux's uhci-hcd does; after it, the BIOS's USB keyboard is
 * gone, which is why falcon jump takes PS/2 only (Section 14.1).
 */

#include <hw/usb/uhci.h>
#include <hw/modrt.h>
#include <hw/pci.h>

#include <lib/string.h>

#define REG_CMD    0x00u
#define REG_STS    0x02u
#define REG_INTR   0x04u
#define REG_FRNUM  0x06u
#define REG_FLBASE 0x08u
#define REG_SOF    0x0Cu
#define REG_PORT   0x10u

#define CMD_RS     0x0001u
#define CMD_HCRST  0x0002u
#define CMD_GRST   0x0004u
#define CMD_CF     0x0040u
#define CMD_MAXP   0x0080u

#define STS_HALTED 0x0020u

#define PORT_CCS   0x0001u
#define PORT_CSC   0x0002u
#define PORT_PE    0x0004u
#define PORT_PEC   0x0008u
#define PORT_LSDA  0x0100u
#define PORT_PR    0x0200u
/* Written as 1 they clear themselves, so a read-modify-write must not carry
 * them back by accident. */
#define PORT_W1C   (PORT_CSC | PORT_PEC)

#define LINK_T     0x1u
#define LINK_Q     0x2u
#define LINK_VF    0x4u

#define TD_ACTIVE  (1u << 23)
#define TD_ERRORS  (0x7Eu << 16)    /* stalled .. bitstuff */
#define TD_STALLED (1u << 22)
#define TD_LS      (1u << 26)
#define TD_CERR3   (3u << 27)

#define PID_SETUP  0x2Du
#define PID_IN     0x69u
#define PID_OUT    0xE1u

#define LEGSUP     0xC0u

/* Enough descriptors for 16 KB at 64 bytes a packet: the most one bulk call
 * moves before it loops. */
#define TD_MAX     256u

/* Controllers: a PIIX4 has one, the ICH family up to four, two ports each.
 * Root ports are numbered across all of them, two to a controller. */
#define UHCI_MAX   4u
#define UHCI_PORTS 2u

/* How long a transfer may take, in 10 us steps: 5 s. A stick that is still
 * spinning up its flash controller answers the first reads slowly. */
#define WAIT_XFER  500000u

typedef struct {
    volatile uint32_t link;
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;
    uint32_t pad[4];
} uhci_td;

typedef struct {
    volatile uint32_t link;
    volatile uint32_t element;
    uint32_t pad[2];
} uhci_qh;

static volatile uint32_t g_frames[UHCI_MAX][1024]
    __attribute__((aligned(4096)));
static uhci_qh g_qhs[UHCI_MAX] __attribute__((aligned(16)));
static uint16_t g_ios[UHCI_MAX];
static unsigned g_nctl;

/* One transfer at a time, whichever controller it is on: the descriptor
 * pool is shared, and these name the controller the current call uses. */
static uhci_td g_td[TD_MAX] __attribute__((aligned(32)));
static uint16_t g_io;
static uhci_qh *g_qhp;

static void use(unsigned port)
{
    unsigned c = port / UHCI_PORTS;

    if (c >= g_nctl)
        c = 0;
    g_io = g_ios[c];
    g_qhp = &g_qhs[c];
}

static uint32_t phys(const volatile void *p)
{
    return (uint32_t)(uintptr_t)p;
}

static void wr16(uint16_t reg, uint16_t v)
{
    outw((uint16_t)(g_io + reg), v);
}

static uint16_t rd16(uint16_t reg)
{
    return inw((uint16_t)(g_io + reg));
}

/* Takes controller `c`'s registers from the BIOS, resets it, and runs its
 * schedule. 0 on success. */
static int start_one(const pci_loc *loc, unsigned c)
{
    uint32_t cmd;
    unsigned i;

    g_io = (uint16_t)(pci_read32(loc->bus, loc->dev, loc->fn, 0x20) &
                      0xFFE0u);
    if (g_io == 0)
        return -1;
    g_ios[c] = g_io;
    g_qhp = &g_qhs[c];
    cmd = pci_read32(loc->bus, loc->dev, loc->fn, 0x04);
    pci_write32(loc->bus, loc->dev, loc->fn, 0x04, (cmd & 0xFFFFu) | 0x0005u);
    pci_write16(loc->bus, loc->dev, loc->fn, LEGSUP, 0x8F00u);

    wr16(REG_CMD, 0);
    for (i = 0; i < 1000u && (rd16(REG_STS) & STS_HALTED) == 0; i++)
        io_delay_us(10);
    wr16(REG_CMD, CMD_GRST);
    io_delay_us(50000);
    wr16(REG_CMD, 0);
    io_delay_us(10000);
    wr16(REG_CMD, CMD_HCRST);
    for (i = 0; i < 1000u && (rd16(REG_CMD) & CMD_HCRST) != 0; i++)
        io_delay_us(10);

    g_qhp->link = LINK_T;
    g_qhp->element = LINK_T;
    for (i = 0; i < 1024u; i++)
        g_frames[c][i] = phys(g_qhp) | LINK_Q;

    wr16(REG_INTR, 0);
    outl((uint16_t)(g_io + REG_FLBASE), phys(g_frames[c]));
    wr16(REG_FRNUM, 0);
    outb((uint16_t)(g_io + REG_SOF), 0x40u);
    wr16(REG_STS, 0xFFFFu);
    wr16(REG_CMD, CMD_RS | CMD_CF | CMD_MAXP);
    return 0;
}

static unsigned uhci_start(void)
{
    pci_loc loc;
    unsigned nth;

    g_nctl = 0;
    for (nth = 0; nth < UHCI_MAX; nth++) {
        if (pci_find_class(0x0C0300u, nth, &loc) != 0)
            break;
        if (start_one(&loc, g_nctl) == 0)
            g_nctl++;
    }
    return g_nctl * UHCI_PORTS;
}

static int uhci_port_reset(unsigned port, int *low)
{
    uint16_t reg = (uint16_t)(REG_PORT + (port % UHCI_PORTS) * 2u);
    uint16_t v;
    unsigned i;

    if (port >= g_nctl * UHCI_PORTS)
        return -1;
    use(port);
    wr16(reg, PORT_PR);
    io_delay_us(50000);
    wr16(reg, 0);
    io_delay_us(1000);
    v = rd16(reg);
    if ((v & PORT_CCS) == 0)
        return 0;
    for (i = 0; i < 10u; i++) {
        v = rd16(reg);
        wr16(reg, (uint16_t)((v & ~PORT_W1C) | PORT_PE | PORT_W1C));
        io_delay_us(10000);
        if ((rd16(reg) & PORT_PE) != 0)
            break;
    }
    v = rd16(reg);
    if ((v & PORT_PE) == 0)
        return -1;
    *low = (v & PORT_LSDA) != 0;
    io_delay_us(10000);              /* reset recovery */
    return 1;
}

static void td_fill(uhci_td *t, uhci_td *next, int low, uint8_t pid,
                    uint8_t addr, uint8_t ep, uint8_t toggle, void *buf,
                    uint32_t len)
{
    uint32_t maxlen = len == 0 ? 0x7FFu : (len - 1u) & 0x7FFu;

    t->link = next != (uhci_td *)0 ? (phys(next) | LINK_VF) : LINK_T;
    t->status = TD_ACTIVE | TD_CERR3 | (low ? TD_LS : 0u);
    t->token = (maxlen << 21) | ((uint32_t)(toggle & 1u) << 19) |
               ((uint32_t)(ep & 0x0Fu) << 15) | ((uint32_t)(addr & 0x7Fu) << 8) |
               pid;
    t->buffer = len != 0 ? phys(buf) : 0;
}

/* Runs g_td[0..n) and waits for it. *done the bytes the data TDs moved; a
 * short IN packet ends the transfer there. *retired how many descriptors
 * the controller finished without error, which is what a bulk endpoint's
 * data toggle follows. */
static int run(unsigned n, uint32_t *done, unsigned first_data,
               unsigned end_data, unsigned *retired)
{
    uint32_t steps = WAIT_XFER;
    unsigned i = 0;
    int rc = USBHC_ERR;

    *done = 0;
    *retired = 0;
    g_qhp->element = phys(&g_td[0]);
    while (steps-- > 0) {
        uint32_t st = g_td[i].status;
        uint32_t actual;

        if ((st & TD_ACTIVE) != 0) {
            io_delay_us(10);
            continue;
        }
        if ((st & TD_ERRORS) != 0) {
            rc = (st & TD_STALLED) != 0 ? USBHC_STALL : USBHC_ERR;
            goto out;
        }
        actual = (st + 1u) & 0x7FFu;
        *retired = i + 1u;
        if (i >= first_data && i < end_data) {
            uint32_t maxlen = ((g_td[i].token >> 21) + 1u) & 0x7FFu;

            *done += actual;
            if (actual < maxlen && (g_td[i].token & 0xFFu) == PID_IN) {
                /* Short: the rest of the data stage is not coming. A
                 * control transfer still has its status stage to run. */
                if (end_data < n) {
                    g_qhp->element = phys(&g_td[end_data]);
                    i = end_data;
                    continue;
                }
                rc = USBHC_OK;
                goto out;
            }
        }
        if (++i == n) {
            rc = USBHC_OK;
            goto out;
        }
    }
out:
    g_qhp->element = LINK_T;
    for (i = 0; i < n; i++)
        g_td[i].status &= ~TD_ACTIVE;
    return rc;
}

static int uhci_control(unsigned port, uint8_t addr, uint8_t mps0, int low,
                        const uint8_t setup[8], void *data, uint16_t len)
{
    static uint8_t s[8];
    int in = (setup[0] & 0x80u) != 0;
    uint8_t *p = data;
    uint32_t off = 0;
    uint32_t done;
    unsigned n = 0;
    uint8_t toggle = 1;

    if (mps0 == 0)
        return USBHC_ERR;
    use(port);
    memcpy(s, setup, 8);
    td_fill(&g_td[n], &g_td[n + 1u], low, PID_SETUP, addr, 0, 0, s, 8);
    n++;
    while (off < len) {
        uint32_t chunk = len - off;

        if (chunk > mps0)
            chunk = mps0;
        if (n + 2u > TD_MAX)
            return USBHC_ERR;
        td_fill(&g_td[n], &g_td[n + 1u], low, in ? PID_IN : PID_OUT, addr, 0,
                toggle, p + off, chunk);
        toggle ^= 1u;
        off += chunk;
        n++;
    }
    td_fill(&g_td[n], (uhci_td *)0, low, (in && len != 0) ? PID_OUT : PID_IN,
            addr, 0, 1, (void *)0, 0);
    n++;
    {
        unsigned retired;

        return run(n, &done, 1, n - 1u, &retired);
    }
}

static int uhci_bulk(unsigned port, uint8_t addr, uint8_t ep, uint16_t mps,
                     void *data, uint32_t len, uint32_t *done,
                     uint8_t *toggle)
{
    int in = (ep & 0x80u) != 0;
    uint8_t *p = data;
    uint32_t moved = 0;

    *done = 0;
    if (mps == 0 || mps > 64u)
        return USBHC_ERR;
    use(port);
    do {
        uint32_t off = 0;
        uint32_t got;
        unsigned n = 0;
        unsigned retired;
        uint8_t first = *toggle;
        int rc;

        while (moved + off < len && n < TD_MAX) {
            uint32_t chunk = len - moved - off;

            if (chunk > mps)
                chunk = mps;
            td_fill(&g_td[n], &g_td[n + 1u], 0, in ? PID_IN : PID_OUT, addr,
                    ep, *toggle, p + moved + off, chunk);
            *toggle ^= 1u;
            off += chunk;
            n++;
        }
        if (n == 0) {
            /* A zero-length packet. */
            td_fill(&g_td[0], (uhci_td *)0, 0, in ? PID_IN : PID_OUT, addr,
                    ep, *toggle, (void *)0, 0);
            *toggle ^= 1u;
            n = 1;
        }
        g_td[n - 1u].link = LINK_T;
        rc = run(n, &got, 0, n, &retired);
        /* The toggle follows the packets that went, not the ones built. */
        *toggle = (uint8_t)(first ^ (retired & 1u));
        moved += got;
        *done = moved;
        if (rc != USBHC_OK)
            return rc;
        if (got < off)
            break;                      /* short: the device has no more */
    } while (moved < len);
    return USBHC_OK;
}

const usbhc uhci_hc = {
    uhci_start, uhci_port_reset, uhci_control, uhci_bulk
};
