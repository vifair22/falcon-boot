/*
 * falcon-pcc: a PC Card bridge with a CompactFlash card in it, for falcon
 * jump's rig (docs/DESIGN.md Section 13.9).
 *
 * QEMU models no PC Card bridge on x86, and falcon jump's PCC.MOD has to be
 * proven against one before it meets a ThinkPad A21m's TI PCI-1450. This is
 * that bridge as the A21m's PCI bus shows it, one socket per instance, so
 * the A21m's two sockets are two functions of one device
 * (`-device falcon-pcc,addr=7.0,multifunction=on,drive=cf
 * -device falcon-pcc,addr=7.1`, the second an empty socket):
 *
 *   - a CardBus bridge's PCI header (type 2) with the PCI-1450's IDs and
 *     class and the A21m's subsystem: BAR0 the socket registers, the
 *     capabilities pointer and secondary status, the bus numbers and the
 *     CardBus latency timer, two memory and two I/O windows, the interrupt
 *     pin (INTA on function 0, INTB on function 1), bridge control, the
 *     legacy 16-bit I/O base, TI's registers at 0x80-0x9F and the power
 *     management capability at 0xA0. Every default is the A21m's, read from
 *     its config space under Linux. What is writable is stored and read
 *     back; the CardBus windows forward nothing, since no CardBus card is
 *     modelled, and the bus numbers are presets (`cardbus-bus`,
 *     `subordinate-bus`; by default the A21m's BIOS's, 2-5 and 6-9);
 *   - the CardBus socket registers (event, mask, present state, force,
 *     control) and ExCA, the i82365SL's register set at +0x800: interface
 *     status, power, interrupt and general control (reset, card type, the
 *     IREQ and CSC routing), card status change and its mask, window
 *     enables, memory window 0 and I/O window 0, and TI's page register for
 *     the memory window;
 *   - the function's PCI interrupt: raised for a socket event its mask
 *     enables (a forced one too, which is how yenta's probe tests the
 *     line), for an ExCA card status change its mask enables and the ExCA
 *     routing sends to PCI, and for the card's IREQ (its ATA interrupt)
 *     while bridge control routes IREQ to PCI. TI's interrupt mode
 *     registers (serial or parallel, the multifunction routing) are stored
 *     and change nothing; routing either interrupt to an ISA IRQ is not
 *     modelled;
 *   - the windows as real mappings, moved and switched as those registers
 *     are written: memory window 0 onto the card's attribute memory (its
 *     CIS, and the configuration option register at 0x200), I/O window 0
 *     onto its ATA task file once the card is configured for I/O. A 32-bit
 *     access through the I/O window reaches the card as two 16-bit ones at
 *     consecutive addresses, as the bridge splits it;
 *   - with `shadow-roms=on`, a chipset that shadows every 16 KB segment of
 *     0xC0000-0xEFFFF an option ROM touches (the PAM granularity of the
 *     A21m's 440BX): the bridge does not see memory window 0's accesses
 *     there, and they reach whatever the segment holds;
 *   - the card, when `drive=` names its image (without one the socket is
 *     empty: card detect says so and nothing else answers): a CIS with the
 *     CompactFlash standard's entries (memory mapped, contiguous I/O,
 *     primary IDE) and the A21m's SanDisk's manufacturer identification,
 *     and ATA PIO over the drive: IDENTIFY DEVICE, READ SECTORS, WRITE
 *     SECTORS, LBA28, INTRQ as the PIO protocols assert it, and with
 *     `lba48=on` the 48-bit feature set (READ and WRITE SECTORS EXT, the
 *     task file's registers as two-deep FIFOs, HOB reads), which a card
 *     past 128 GiB needs. Off, a card larger than that says 2^28 - 1
 *     sectors, as a card without the feature set would.
 *
 * A model, so it is exactly as right as its reading of the specifications
 * and of the A21m's registers; where the silicon is looser or stricter,
 * only the A21m says. It is test equipment, built into a QEMU for the rig
 * by build-qemu.sh, and never part of falcon.
 *
 * GPL-2.0-or-later, as QEMU is.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/resettable.h"
/* QEMU 11 moved these four; 10.0, which the coverage harness's Debian runs,
 * has them at the older paths. build-qemu.sh builds the release on PATH, so
 * both have to compile. */
#if __has_include("hw/core/qdev-properties.h")
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#else
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#endif
#include "system/block-backend.h"
#if __has_include("system/address-spaces.h")
#include "system/address-spaces.h"
#include "system/memory.h"
#else
#include "exec/address-spaces.h"
#include "exec/memory.h"
#endif

#define TYPE_FALCON_PCC "falcon-pcc"
OBJECT_DECLARE_SIMPLE_TYPE(FalconPccState, FALCON_PCC)

/* The type-2 header past BAR0, and the PCI-1450's own registers. */
#define CB_CAP_PTR         0x14
#define CB_SEC_STATUS      0x16
#define CB_PRIMARY_BUS     0x18
#define CB_CARDBUS_BUS     0x19
#define CB_SUBORDINATE_BUS 0x1A
#define CB_LATENCY         0x1B
#define CB_MEM_BASE0       0x1C
#define CB_IO_BASE0        0x2C
#define CB_BRIDGE_CONTROL  0x3E
#define CB_SUBSYS_VENDOR   0x40
#define CB_LEGACY_BASE     0x44
#define TI_SYSTEM_CONTROL  0x80
#define TI_MFUNC_ROUTING   0x8C
#define TI_RETRY_STATUS    0x90
#define TI_CARD_CONTROL    0x91
#define TI_DEVICE_CONTROL  0x92
#define TI_DIAGNOSTIC      0x93
#define TI_PM_CAP          0xA0

#define BC_IREQ_ISA        0x0080  /* bridge control: IREQ to ISA, not PCI */
#define SC_SOCACTIVE       0x00002000u

/* CardBus socket registers. */
#define SOCK_EVENT     0x00
#define SOCK_MASK      0x04
#define SOCK_PRESENT   0x08
#define SOCK_FORCE     0x0C
#define SOCK_CONTROL   0x10
#define EV_CSTSCHG     (1u << 0)
#define EV_CD          (3u << 1)
#define EV_PWRCYCLE    (1u << 3)
#define EV_ALL         0x0Fu
#define PS_CD          (3u << 1)
#define PS_PWRCYCLE    (1u << 3)
#define PS_16BIT       (1u << 4)
#define PS_5V          (1u << 10)
#define PS_3V          (1u << 11)
#define PS_5VSOCKET    (1u << 28)
#define PS_3VSOCKET    (1u << 29)

/* ExCA, at BAR0 + 0x800. */
#define EXCA           0x800
#define X_STATUS       0x01
#define X_POWER        0x02
#define X_INTCTL       0x03
#define X_CSC          0x04
#define X_CSCINT       0x05
#define X_ENABLE       0x06
#define X_IOCTL        0x07
#define X_IO0          0x08
#define X_MEM0         0x10
#define X_PAGE0        0x40

#define ST_BVD         0x03      /* both battery lines: fine, no status change */
#define ST_CD          0x0C      /* both card detects */
#define ST_READY       0x20
#define ST_POWERON     0x40
#define IC_RESET       0x40      /* 1: reset released */
#define IC_IOCARD      0x20
#define IC_CSC_PCI     0x10      /* TI: card status change to PCI, not ISA */
#define CSC_READY      0x04
#define EN_MEM0        0x01
#define EN_IO0         0x40

#define CONFIG_BASE    0x200
#define ATTR_SPAN      0x1000

#define ROM_AREA       0xC0000u
#define ROM_AREA_END   0xF0000u
#define PAM_SEG        0x4000u

/* ATA status. */
#define A_BSY  0x80
#define A_DRDY 0x40
#define A_DRQ  0x08
#define A_ERR  0x01
#define DC_NIEN 0x02
#define DC_SRST 0x04
#define DC_HOB  0x80

struct FalconPccState {
    PCIDevice parent_obj;
    MemoryRegion bar;
    MemoryRegion attr;
    MemoryRegion io;
    BlockBackend *blk;
    uint64_t sectors;

    uint32_t sock_event;
    uint32_t sock_mask;
    uint32_t sock_control;
    uint8_t volts;          /* 0: a card taking 5 V or 3.3 V; 3: 3.3 V only */
    uint8_t cardbus_bus;    /* the properties; 0xFF: the A21m's, by function */
    uint8_t subordinate_bus;
    uint8_t exca[256];
    uint8_t csc;            /* ExCA card status change, cleared by a read */
    bool was_powered;
    bool was_running;
    uint8_t cor;

    uint8_t cis[128];
    unsigned cis_len;

    /* ATA */
    uint8_t feature;
    uint8_t count;
    uint8_t lba[3];
    uint8_t hob_count;      /* what the previous write to each register was */
    uint8_t hob_lba[3];
    bool lba48;             /* the property: IDENTIFY says 48-bit */
    bool shadow_roms;       /* the property: see the head of the file */
    bool roms_scanned;
    uint16_t shadowed;      /* bit n: segment ROM_AREA + n * PAM_SEG */
    uint8_t devctl;         /* bit 7: HOB, reads return the previous byte */
    uint8_t device;
    uint8_t status;
    uint8_t error;
    bool intrq;             /* pending until the status register is read */
    uint8_t buf[512];
    unsigned pos;
    unsigned remain;
    bool writing;
    uint64_t cur;
};

static bool has_card(FalconPccState *s)
{
    return s->blk != NULL;
}

/* ---- the card's CIS ---- */

static void cis_put(FalconPccState *s, const uint8_t *b, unsigned n)
{
    memcpy(s->cis + s->cis_len, b, n);
    s->cis_len += n;
}

static void cis_build(FalconPccState *s)
{
    /* A CompactFlash card's, trimmed to what a host reads: device info,
     * MANFID (SanDisk, CFA: the A21m's card's, and the pair Linux's
     * pata_pcmcia binds by without a userspace acknowledgement of the
     * function ID), FUNCID fixed disk, CONFIG (registers at 0x200, last
     * index 3), and the CF standard's entries: 0 memory mapped (default), 1
     * contiguous I/O on 16 address lines' worth (4 decoded), 2 the primary
     * IDE range. */
    static const uint8_t device[] = { 0x01, 0x03, 0xD9, 0x01, 0xFF };
    static const uint8_t manfid[] = { 0x20, 0x04, 0x45, 0x00, 0x01, 0x04 };
    static const uint8_t funcid[] = { 0x21, 0x02, 0x04, 0x01 };
    static const uint8_t config[] = { 0x1A, 0x05, 0x01, 0x03, 0x00, 0x02,
                                      0x0F };
    static const uint8_t e0[] = { 0x1B, 0x09, 0xC0, 0xC0, 0x21, 0x21, 0xB5,
                                  0x1E, 0x4D, 0x08, 0x00 };
    static const uint8_t e1[] = { 0x1B, 0x07, 0x01, 0x09, 0x21, 0xB5, 0x1E,
                                  0x4D, 0x64 };
    static const uint8_t e2[] = { 0x1B, 0x0E, 0x02, 0x09, 0x21, 0xB5, 0x1E,
                                  0x4D, 0xEA, 0x61, 0xF0, 0x01, 0x07, 0xF6,
                                  0x03, 0x01 };
    static const uint8_t end[] = { 0xFF };

    s->cis_len = 0;
    cis_put(s, device, sizeof device);
    cis_put(s, manfid, sizeof manfid);
    cis_put(s, funcid, sizeof funcid);
    cis_put(s, config, sizeof config);
    cis_put(s, e0, sizeof e0);
    cis_put(s, e1, sizeof e1);
    cis_put(s, e2, sizeof e2);
    cis_put(s, end, sizeof end);
}

/* ---- power, interrupts ---- */

/* VCC as the socket control register holds it: 0x20 is 5 V, 0x30 3.3 V.
 * A card that takes only 3.3 V gets nothing at 5 V; the bridge refuses the
 * request, as the PCI-1450 does. Power is VCC alone: the ExCA output
 * enable (the power register's bit 7) is what lets the card answer, and a
 * host applies it after it has seen the socket powered. */
static bool powered(FalconPccState *s)
{
    uint32_t vcc = s->sock_control & 0x70;

    if (vcc != 0x20 && vcc != 0x30)
        return false;
    if (has_card(s) && s->volts == 3 && vcc != 0x30)
        return false;
    return true;
}

static bool running(FalconPccState *s)
{
    return has_card(s) && powered(s) && (s->exca[X_POWER] & 0x80) != 0 &&
           (s->exca[X_INTCTL] & IC_RESET) != 0;
}

static bool configured(FalconPccState *s)
{
    return running(s) && (s->cor & 0x3F) != 0;
}

/* The function's PCI interrupt is the OR of the three sources. */
static void update_irq(FalconPccState *s)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint16_t bctl = pci_get_word(pdev->config + CB_BRIDGE_CONTROL);
    uint32_t ev = s->sock_event, mask = s->sock_mask;
    bool level = false;

    if (((ev & EV_CSTSCHG) && (mask & EV_CSTSCHG)) ||
        ((ev & EV_CD) && (mask & EV_CD)) ||
        ((ev & EV_PWRCYCLE) && (mask & EV_PWRCYCLE)))
        level = true;
    if ((s->csc & s->exca[X_CSCINT] & 0x0F) != 0 &&
        (s->exca[X_INTCTL] & IC_CSC_PCI) != 0)
        level = true;
    if (s->intrq && (s->devctl & DC_NIEN) == 0 && configured(s) &&
        (s->exca[X_INTCTL] & IC_IOCARD) != 0 && (bctl & BC_IREQ_ISA) == 0)
        level = true;
    pci_set_irq(pdev, level ? 1 : 0);
}

/* After anything that can change the socket's power or the card's reset:
 * the events the bridge reports for them. */
static void note_state(FalconPccState *s)
{
    bool p = powered(s), r = running(s);

    if (p != s->was_powered)
        s->sock_event |= EV_PWRCYCLE;
    if (r != s->was_running)
        s->csc |= CSC_READY;
    s->was_powered = p;
    s->was_running = r;
    update_irq(s);
}

/* ---- window decoding ---- */

/* The segments an option ROM touches, read the first time the window
 * opens: by then POST has placed every ROM, and the window has not yet
 * covered any header. */
static void scan_roms(FalconPccState *s)
{
    uint32_t a;

    for (a = ROM_AREA; a < ROM_AREA_END; a += 0x800) {
        uint8_t h[3];
        uint32_t end;
        uint32_t seg;

        address_space_read(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                           h, sizeof h);
        if (h[0] != 0x55 || h[1] != 0xAA)
            continue;
        end = a + (h[2] != 0 ? (uint32_t)h[2] * 512u : 1u);
        for (seg = a & ~(PAM_SEG - 1); seg < end && seg < ROM_AREA_END;
             seg += PAM_SEG)
            s->shadowed |= (uint16_t)(1u << ((seg - ROM_AREA) / PAM_SEG));
    }
    s->roms_scanned = true;
}

static bool shadowed(FalconPccState *s, uint32_t start, uint32_t stop)
{
    uint32_t seg;

    for (seg = start & ~(PAM_SEG - 1); seg <= stop; seg += PAM_SEG) {
        if (seg >= ROM_AREA && seg < ROM_AREA_END &&
            (s->shadowed & (1u << ((seg - ROM_AREA) / PAM_SEG))) != 0)
            return true;
    }
    return false;
}

static void remap(FalconPccState *s)
{
    const uint8_t *x = s->exca;
    bool mem = (x[X_ENABLE] & EN_MEM0) != 0;
    bool io = (x[X_ENABLE] & EN_IO0) != 0 && (x[X_INTCTL] & IC_IOCARD) != 0;
    uint32_t start = ((uint32_t)x[X_PAGE0] << 24) |
                     ((uint32_t)(x[X_MEM0 + 1] & 0x0F) << 20) |
                     ((uint32_t)x[X_MEM0] << 12);
    uint32_t stop = ((uint32_t)x[X_PAGE0] << 24) |
                    ((uint32_t)(x[X_MEM0 + 3] & 0x0F) << 20) |
                    ((uint32_t)x[X_MEM0 + 2] << 12) | 0xFFF;

    if (mem && s->shadow_roms) {
        if (!s->roms_scanned)
            scan_roms(s);
        if (shadowed(s, start, stop))
            mem = false;
    }
    memory_region_transaction_begin();
    if (mem) {
        memory_region_set_size(&s->attr, stop >= start ? stop - start + 1
                                                        : 0x1000);
        memory_region_set_address(&s->attr, start);
    }
    memory_region_set_enabled(&s->attr, mem);
    if (io) {
        uint16_t io_start = (uint16_t)(x[X_IO0] | (x[X_IO0 + 1] << 8));
        uint16_t io_stop = (uint16_t)(x[X_IO0 + 2] | (x[X_IO0 + 3] << 8));

        memory_region_set_size(&s->io, io_stop >= io_start ?
                               io_stop - io_start + 1u : 1u);
        memory_region_set_address(&s->io, io_start);
    }
    memory_region_set_enabled(&s->io, io);
    memory_region_transaction_commit();
}

/* The card address a host access through memory window 0 reaches. */
static uint32_t card_addr(FalconPccState *s, hwaddr off)
{
    const uint8_t *x = s->exca;
    uint32_t start = ((uint32_t)x[X_PAGE0] << 24) |
                     ((uint32_t)(x[X_MEM0 + 1] & 0x0F) << 20) |
                     ((uint32_t)x[X_MEM0] << 12);
    uint32_t delta = ((uint32_t)(x[X_MEM0 + 5] & 0x3F) << 20) |
                     ((uint32_t)x[X_MEM0 + 4] << 12);

    return (start + (uint32_t)off + delta) & 0x3FFFFFF;
}

/* ---- attribute memory ---- */

static uint64_t attr_read(void *opaque, hwaddr off, unsigned size)
{
    FalconPccState *s = opaque;
    uint32_t a = card_addr(s, off);

    if (!running(s) || (s->exca[X_MEM0 + 5] & 0x40) == 0 || (a & 1) != 0)
        return 0xFF;
    if (a == CONFIG_BASE)
        return s->cor;
    if (a / 2 < s->cis_len)
        return s->cis[a / 2];
    return 0xFF;
}

static void attr_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    FalconPccState *s = opaque;
    uint32_t a = card_addr(s, off);

    if (!running(s) || (s->exca[X_MEM0 + 5] & 0x40) == 0)
        return;
    if (a == CONFIG_BASE) {
        s->cor = (uint8_t)v;
        remap(s);
        update_irq(s);
    }
}

static const MemoryRegionOps attr_ops = {
    .read = attr_read,
    .write = attr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 1 },
};

/* ---- ATA ---- */

static void ata_str(uint16_t *w, unsigned first, unsigned words,
                    const char *s)
{
    unsigned i;

    for (i = 0; i < words * 2; i++) {
        char c = *s ? *s++ : ' ';
        unsigned wi = first + i / 2;

        if (i % 2 == 0)
            w[wi] = (uint16_t)((uint8_t)c << 8);
        else
            w[wi] |= (uint8_t)c;
    }
}

static void ata_identify(FalconPccState *s)
{
    uint16_t w[256];
    unsigned i;

    memset(w, 0, sizeof w);
    w[0] = 0x848A;                       /* CompactFlash */
    w[1] = 16383;
    w[3] = 16;
    w[6] = 63;
    ata_str(w, 10, 10, "FALCONRIG0001");
    ata_str(w, 23, 4, "1.0");
    ata_str(w, 27, 20, "FALCON RIG CF");
    w[49] = 1u << 9;                     /* LBA */
    {
        uint64_t n28 = s->sectors > 0x0FFFFFFF ? 0x0FFFFFFF : s->sectors;

        w[60] = (uint16_t)(n28 & 0xFFFF);
        w[61] = (uint16_t)(n28 >> 16);
    }
    if (s->lba48) {
        w[83] = (1u << 14) | (1u << 10);  /* valid; 48-bit supported */
        w[86] = 1u << 10;                  /* 48-bit enabled */
        w[100] = (uint16_t)(s->sectors & 0xFFFF);
        w[101] = (uint16_t)(s->sectors >> 16);
        w[102] = (uint16_t)(s->sectors >> 32);
        w[103] = (uint16_t)(s->sectors >> 48);
    }
    for (i = 0; i < 256; i++) {
        s->buf[i * 2] = (uint8_t)(w[i] & 0xFF);
        s->buf[i * 2 + 1] = (uint8_t)(w[i] >> 8);
    }
}

/* The command is done, or a block is ready: INTRQ, as every PIO protocol
 * asserts it, until the host reads the status register. */
static void ata_done(FalconPccState *s, uint8_t status)
{
    s->status = status;
    s->intrq = true;
    update_irq(s);
}

static void ata_abort(FalconPccState *s)
{
    s->error = 0x04;
    s->remain = 0;
    ata_done(s, A_DRDY | A_ERR);
}

static bool ata_fetch(FalconPccState *s)
{
    if (s->cur >= s->sectors ||
        blk_pread(s->blk, (int64_t)s->cur * 512, 512, s->buf, 0) < 0) {
        ata_abort(s);
        return false;
    }
    s->pos = 0;
    ata_done(s, A_DRDY | A_DRQ);
    return true;
}

/* The signature a device leaves in the task file after a reset: an ATA
 * device, not a packet one. */
static void ata_reset(FalconPccState *s)
{
    s->status = A_DRDY;
    s->error = 0x01;
    s->remain = 0;
    s->count = 1;
    s->lba[0] = 1;
    s->lba[1] = 0;
    s->lba[2] = 0;
    s->hob_count = 0;
    s->hob_lba[0] = s->hob_lba[1] = s->hob_lba[2] = 0;
    s->device = 0;
    s->intrq = false;
}

static void ata_command(FalconPccState *s, uint8_t cmd)
{
    uint64_t lba = s->lba[0] | (s->lba[1] << 8) | (s->lba[2] << 16) |
                   ((uint32_t)(s->device & 0x0F) << 24);
    uint64_t lba_ext = (uint64_t)s->lba[0] | ((uint64_t)s->lba[1] << 8) |
                       ((uint64_t)s->lba[2] << 16) |
                       ((uint64_t)s->hob_lba[0] << 24) |
                       ((uint64_t)s->hob_lba[1] << 32) |
                       ((uint64_t)s->hob_lba[2] << 40);
    unsigned count_ext = (unsigned)s->count | ((unsigned)s->hob_count << 8);

    s->error = 0;
    s->intrq = false;
    update_irq(s);
    switch (cmd) {
    case 0xEC:                           /* IDENTIFY DEVICE */
        ata_identify(s);
        s->pos = 0;
        s->remain = 1;
        s->writing = false;
        ata_done(s, A_DRDY | A_DRQ);
        return;
    case 0x20:
    case 0x21:                           /* READ SECTORS */
        if ((s->device & 0x40) == 0) {
            ata_abort(s);
            return;
        }
        s->cur = lba;
        s->remain = s->count ? s->count : 256;
        s->writing = false;
        ata_fetch(s);
        return;
    case 0x30:
    case 0x31:                           /* WRITE SECTORS */
        if ((s->device & 0x40) == 0 || lba >= s->sectors) {
            ata_abort(s);
            return;
        }
        s->cur = lba;
        s->remain = s->count ? s->count : 256;
        s->writing = true;
        s->pos = 0;
        s->status = A_DRDY | A_DRQ;      /* the first block: no INTRQ */
        return;
    case 0x24:                           /* READ SECTORS EXT */
        if (!s->lba48 || (s->device & 0x40) == 0) {
            ata_abort(s);
            return;
        }
        s->cur = lba_ext;
        s->remain = count_ext ? count_ext : 65536;
        s->writing = false;
        ata_fetch(s);
        return;
    case 0x34:                           /* WRITE SECTORS EXT */
        if (!s->lba48 || (s->device & 0x40) == 0 || lba_ext >= s->sectors) {
            ata_abort(s);
            return;
        }
        s->cur = lba_ext;
        s->remain = count_ext ? count_ext : 65536;
        s->writing = true;
        s->pos = 0;
        s->status = A_DRDY | A_DRQ;
        return;
    case 0xEF:                           /* SET FEATURES */
    case 0x91:                           /* INITIALIZE DEVICE PARAMETERS */
    case 0xE5:                           /* CHECK POWER MODE */
    case 0xE7:                           /* FLUSH CACHE */
        ata_done(s, A_DRDY);
        return;
    default:
        ata_abort(s);
        return;
    }
}

static void ata_word_done(FalconPccState *s)
{
    if (s->pos < 512)
        return;
    if (s->writing) {
        if (blk_pwrite(s->blk, (int64_t)s->cur * 512, 512, s->buf, 0) < 0) {
            ata_abort(s);
            return;
        }
    }
    s->cur++;
    if (--s->remain == 0) {
        if (s->writing)
            ata_done(s, A_DRDY);
        else
            s->status = A_DRDY;          /* a read ends without INTRQ */
        return;
    }
    if (s->writing) {
        s->pos = 0;
        ata_done(s, A_DRDY | A_DRQ);
    } else {
        ata_fetch(s);
    }
}

/* A device 0 only: with device 1 selected the card answers nothing, so a
 * host probing for a second device finds none. */
static uint64_t io_read(void *opaque, hwaddr off, unsigned size)
{
    FalconPccState *s = opaque;

    if (!configured(s))
        return size == 2 ? 0xFFFF : 0xFF;
    if ((s->device & 0x10) != 0)
        return 0;
    switch (off & 0xF) {
    case 0x0: {
        uint64_t v;

        if ((s->status & A_DRQ) == 0 || s->writing)
            return size == 2 ? 0xFFFF : 0xFF;
        v = s->buf[s->pos];
        if (size == 2)
            v |= (uint64_t)s->buf[s->pos + 1] << 8;
        s->pos += size;
        ata_word_done(s);
        return v;
    }
    case 0x1: return s->error;
    case 0x2: return (s->devctl & DC_HOB) ? s->hob_count : s->count;
    case 0x3: return (s->devctl & DC_HOB) ? s->hob_lba[0] : s->lba[0];
    case 0x4: return (s->devctl & DC_HOB) ? s->hob_lba[1] : s->lba[1];
    case 0x5: return (s->devctl & DC_HOB) ? s->hob_lba[2] : s->lba[2];
    case 0x6: return s->device;
    case 0x7:
        s->intrq = false;
        update_irq(s);
        return s->status;
    case 0xE: return s->status;
    default:  return 0xFF;
    }
}

static void io_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    FalconPccState *s = opaque;

    if (!configured(s))
        return;
    if ((s->device & 0x10) != 0 && (off & 0xF) != 0x6 && (off & 0xF) != 0xE)
        return;
    switch (off & 0xF) {
    case 0x0:
        if ((s->status & A_DRQ) == 0 || !s->writing)
            return;
        s->buf[s->pos] = (uint8_t)v;
        if (size == 2)
            s->buf[s->pos + 1] = (uint8_t)(v >> 8);
        s->pos += size;
        ata_word_done(s);
        return;
    case 0x1: s->feature = (uint8_t)v; return;
    /* Each a two-deep FIFO, as the 48-bit feature set has it: a write
     * pushes the byte before it into the HOB. */
    case 0x2: s->hob_count = s->count; s->count = (uint8_t)v; return;
    case 0x3: s->hob_lba[0] = s->lba[0]; s->lba[0] = (uint8_t)v; return;
    case 0x4: s->hob_lba[1] = s->lba[1]; s->lba[1] = (uint8_t)v; return;
    case 0x5: s->hob_lba[2] = s->lba[2]; s->lba[2] = (uint8_t)v; return;
    case 0x6: s->device = (uint8_t)v; return;
    case 0x7: ata_command(s, (uint8_t)v); return;
    case 0xE:
        s->devctl = (uint8_t)v;
        if ((v & DC_SRST) != 0)
            ata_reset(s);
        update_irq(s);
        return;
    default:
        return;
    }
}

static const MemoryRegionOps io_ops = {
    .read = io_read,
    .write = io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 2 },
};

/* ---- the bridge's BAR: socket registers and ExCA ---- */

static void card_reset(FalconPccState *s)
{
    s->cor = 0;
    ata_reset(s);
}

static uint32_t present_state(FalconPccState *s)
{
    uint32_t v = PS_5VSOCKET | PS_3VSOCKET;

    if (!has_card(s))
        return v | PS_CD;
    v |= PS_16BIT | PS_3V | (s->volts == 3 ? 0 : PS_5V);
    if (powered(s))
        v |= PS_PWRCYCLE;
    return v;
}

static uint8_t exca_read(FalconPccState *s, uint8_t r)
{
    uint8_t v;

    switch (r) {
    case 0x00:
        return 0x84;                              /* i82365SL-compatible */
    case X_STATUS:
        if (!has_card(s))
            return 0;
        return ST_BVD | ST_CD | (powered(s) ? ST_POWERON : 0) |
               (running(s) ? ST_READY : 0);
    case X_CSC:
        v = s->csc;
        s->csc = 0;
        update_irq(s);
        return v;
    default:
        return s->exca[r];
    }
}

static void exca_write(FalconPccState *s, uint8_t r, uint8_t v)
{
    uint8_t old = s->exca[r];

    s->exca[r] = v;
    if (r == X_INTCTL && (old & IC_RESET) != 0 && (v & IC_RESET) == 0)
        card_reset(s);
    /* The PCI-1450 takes the ExCA VCC field as a request too, and the
     * last one written is the one the socket applies. */
    if (r == X_POWER)
        s->sock_control = (s->sock_control & ~0x70u) |
            ((v & 0x18) == 0x10 ? 0x20u : (v & 0x18) == 0x18 ? 0x30u : 0);
    if (r == X_POWER && !powered(s))
        card_reset(s);
    remap(s);
    note_state(s);
}

static uint64_t bar_read(void *opaque, hwaddr off, unsigned size)
{
    FalconPccState *s = opaque;
    uint32_t v;
    unsigned i;

    if (off >= EXCA && off < EXCA + 0x100) {
        v = 0;
        for (i = 0; i < size && off + i < EXCA + 0x100; i++)
            v |= (uint32_t)exca_read(s, (uint8_t)(off + i - EXCA)) << (8 * i);
        return v;
    }
    switch (off & ~3u) {
    case SOCK_EVENT:   v = s->sock_event; break;
    case SOCK_MASK:    v = s->sock_mask; break;
    case SOCK_PRESENT: v = present_state(s); break;
    case SOCK_CONTROL: v = s->sock_control; break;
    default:           v = 0; break;
    }
    return (v >> (8 * (off & 3))) & (size == 4 ? 0xFFFFFFFFu :
                                     size == 2 ? 0xFFFFu : 0xFFu);
}

static void bar_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    FalconPccState *s = opaque;
    uint32_t v = (uint32_t)val << (8 * (off & 3));
    uint32_t m = (size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu)
                 << (8 * (off & 3));
    unsigned i;

    if (off >= EXCA && off < EXCA + 0x100) {
        for (i = 0; i < size && off + i < EXCA + 0x100; i++)
            exca_write(s, (uint8_t)(off + i - EXCA), (uint8_t)(val >> (8 * i)));
        return;
    }
    switch (off & ~3u) {
    case SOCK_EVENT:                              /* write 1 to clear */
        s->sock_event &= ~(v & m & EV_ALL);
        break;
    case SOCK_MASK:
        s->sock_mask = (s->sock_mask & ~m) | (v & m & EV_ALL);
        break;
    case SOCK_FORCE:
        s->sock_event |= v & m & EV_ALL;
        break;
    case SOCK_CONTROL:
        s->sock_control = ((s->sock_control & ~m) | (v & m)) & 0x77;
        /* The ExCA VCC field reads back what the socket was asked for. */
        s->exca[X_POWER] = (uint8_t)((s->exca[X_POWER] & ~0x18) |
            ((s->sock_control & 0x70) == 0x20 ? 0x10 :
             (s->sock_control & 0x70) == 0x30 ? 0x18 : 0));
        if (!powered(s))
            card_reset(s);
        remap(s);
        break;
    default:
        return;
    }
    note_state(s);
}

static const MemoryRegionOps bar_ops = {
    .read = bar_read,
    .write = bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---- the PCI header and the PCI-1450's registers ---- */

/* What a write may change; everything else in the header past BAR0 and in
 * TI's space is read-only, including the registers whose bits the A21m's
 * dump does not settle. */
static void config_masks(PCIDevice *pdev)
{
    uint8_t *w = pdev->wmask, *w1c = pdev->w1cmask;

    memset(w + 0x40, 0, PCI_CONFIG_SPACE_SIZE - 0x40);
    w[PCI_LATENCY_TIMER] = 0xF8;
    pci_set_word(w1c + CB_SEC_STATUS, 0xF900);
    w[CB_PRIMARY_BUS] = 0xFF;
    w[CB_CARDBUS_BUS] = 0xFF;
    w[CB_SUBORDINATE_BUS] = 0xFF;
    w[CB_LATENCY] = 0xF8;
    pci_set_long(w + CB_MEM_BASE0, 0xFFFFF000);        /* memory 0 and 1 */
    pci_set_long(w + CB_MEM_BASE0 + 4, 0xFFFFF000);
    pci_set_long(w + CB_MEM_BASE0 + 8, 0xFFFFF000);
    pci_set_long(w + CB_MEM_BASE0 + 12, 0xFFFFF000);
    pci_set_long(w + CB_IO_BASE0, 0xFFFFFFFC);         /* I/O 0 and 1 */
    pci_set_long(w + CB_IO_BASE0 + 4, 0xFFFFFFFC);
    pci_set_long(w + CB_IO_BASE0 + 8, 0xFFFFFFFC);
    pci_set_long(w + CB_IO_BASE0 + 12, 0xFFFFFFFC);
    pci_set_word(w + CB_BRIDGE_CONTROL, 0x07EF);
    pci_set_long(w + CB_LEGACY_BASE, 0x0000FFFC);
    /* System control: all but SOCACTIVE and INTERROGATE, which are status. */
    pci_set_long(w + TI_SYSTEM_CONTROL, 0xFFFFDEFF);
    pci_set_long(w + TI_MFUNC_ROUTING, 0x0FFFFFFF);
    w[TI_RETRY_STATUS] = 0xC0;                         /* the enables */
    w1c[TI_RETRY_STATUS] = 0x3E;                       /* the expiries */
    w[TI_CARD_CONTROL] = 0xFE;
    w[TI_DEVICE_CONTROL] = 0xFF;
    w[TI_DIAGNOSTIC] = 0xFF;
    pci_set_word(w + TI_PM_CAP + 4, 0x0103);           /* PMCSR: state, PME */
    pci_set_word(w1c + TI_PM_CAP + 4, 0x8000);
}

/* The A21m's values (its config space read under Linux) for everything
 * the PCI core does not reset itself. The windows start closed; the bus
 * numbers are the presets. */
static void config_reset(FalconPccState *s)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint8_t *c = pdev->config;
    unsigned fn = PCI_FUNC(pdev->devfn);
    uint8_t cb = s->cardbus_bus != 0xFF ? s->cardbus_bus : fn == 0 ? 2 : 6;
    uint8_t sub = s->subordinate_bus != 0xFF ? s->subordinate_bus
                                             : fn == 0 ? 5 : 9;

    memset(c + CB_CAP_PTR, 0, PCI_CONFIG_SPACE_SIZE - CB_CAP_PTR);
    c[CB_CAP_PTR] = TI_PM_CAP;
    pci_set_word(c + CB_SEC_STATUS, 0x0200);           /* DEVSEL medium */
    c[CB_PRIMARY_BUS] = 0;
    c[CB_CARDBUS_BUS] = cb;
    c[CB_SUBORDINATE_BUS] = sub;
    c[CB_LATENCY] = 0xB0;
    c[PCI_INTERRUPT_PIN] = (uint8_t)(1 + fn);
    pci_set_word(c + CB_BRIDGE_CONTROL, 0x05C0);
    pci_set_word(c + CB_SUBSYS_VENDOR, 0x1014);        /* IBM */
    pci_set_word(c + CB_SUBSYS_VENDOR + 2, 0x0130);
    pci_set_long(c + CB_LEGACY_BASE, 0x00000001);
    pci_set_long(c + TI_SYSTEM_CONTROL, 0x0844D079);
    pci_set_long(c + 0x84, 0x000000C0);
    pci_set_long(c + 0x88, 0x80018080);
    pci_set_long(c + TI_MFUNC_ROUTING, 0x00001000);    /* MFUNC3: IRQSER */
    c[TI_RETRY_STATUS] = 0xC0;
    c[TI_CARD_CONTROL] = 0x02;
    c[TI_DEVICE_CONTROL] = 0x66;         /* interrupts all serialised */
    c[TI_DIAGNOSTIC] = 0x40;             /* CardBus interrupts to PCI */
    c[TI_PM_CAP] = PCI_CAP_ID_PM;
    c[TI_PM_CAP + 1] = 0;
    pci_set_word(c + TI_PM_CAP + 2, 0xFE11);
    pci_set_word(c + TI_PM_CAP + 4, 0x0000);
    c[TI_PM_CAP + 6] = 0xC0;
    c[TI_PM_CAP + 7] = 0;
}

/* SOCACTIVE in the system control register, read-only: the socket is
 * powered and its card out of reset. The A21m showed it on the function
 * whose socket Linux had brought up. */
static uint32_t falcon_pcc_config_read(PCIDevice *pdev, uint32_t addr, int len)
{
    FalconPccState *s = FALCON_PCC(pdev);
    uint32_t v = pci_default_read_config(pdev, addr, len);

    if (addr <= TI_SYSTEM_CONTROL + 1 && addr + len > TI_SYSTEM_CONTROL + 1 &&
        running(s))
        v |= (SC_SOCACTIVE >> 8) << (8 * (TI_SYSTEM_CONTROL + 1 - addr));
    return v;
}

static void falcon_pcc_config_write(PCIDevice *pdev, uint32_t addr,
                                    uint32_t val, int len)
{
    FalconPccState *s = FALCON_PCC(pdev);

    pci_default_write_config(pdev, addr, val, len);
    if (addr <= CB_BRIDGE_CONTROL && addr + len > CB_BRIDGE_CONTROL)
        update_irq(s);
}

static void falcon_pcc_reset_hold(Object *obj, ResetType type)
{
    FalconPccState *s = FALCON_PCC(obj);

    config_reset(s);
    s->sock_event = 0;
    s->sock_mask = 0;
    s->sock_control = 0;
    memset(s->exca, 0, sizeof s->exca);
    s->csc = 0;
    s->was_powered = false;
    s->was_running = false;
    s->devctl = 0;
    s->feature = 0;
    card_reset(s);
    remap(s);
    update_irq(s);
}

static void falcon_pcc_realize(PCIDevice *pdev, Error **errp)
{
    FalconPccState *s = FALCON_PCC(pdev);

    if (s->blk) {
        int64_t len;

        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0)
            return;
        len = blk_getlength(s->blk);
        if (len < 512) {
            error_setg(errp, "falcon-pcc: the card's image is too small");
            return;
        }
        s->sectors = (uint64_t)len / 512;
        if (!s->lba48 && s->sectors > 0x0FFFFFFF)
            s->sectors = 0x0FFFFFFF;
        cis_build(s);
    }

    pdev->config[PCI_HEADER_TYPE] |= PCI_HEADER_TYPE_CARDBUS;
    pci_set_word(pdev->config + PCI_STATUS,
                 PCI_STATUS_CAP_LIST | PCI_STATUS_DEVSEL_MEDIUM);
    config_masks(pdev);
    config_reset(s);
    card_reset(s);

    memory_region_init_io(&s->bar, OBJECT(s), &bar_ops, s, "falcon-pcc-bar",
                          4 * KiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);

    /* Placed where the window registers say once they are written (remap);
     * the address here is only where it waits, disabled. Its priority puts
     * it over the RAM or ROM the PAM registers map there, as a segment the
     * chipset does not shadow passes to the bridge; shadow-roms is what
     * takes that back for the segments a ROM touches. */
    memory_region_init_io(&s->attr, OBJECT(s), &attr_ops, s,
                          "falcon-pcc-attr", ATTR_SPAN);
    memory_region_add_subregion_overlap(get_system_memory(), 0xD0000,
                                        &s->attr, 10);
    memory_region_set_enabled(&s->attr, false);
    memory_region_init_io(&s->io, OBJECT(s), &io_ops, s, "falcon-pcc-io",
                          16);
    memory_region_add_subregion_overlap(get_system_io(), 0x1E0, &s->io, 10);
    memory_region_set_enabled(&s->io, false);
}

static const Property falcon_pcc_props[] = {
    DEFINE_PROP_DRIVE("drive", FalconPccState, blk),
    DEFINE_PROP_UINT8("volts", FalconPccState, volts, 0),
    DEFINE_PROP_UINT8("cardbus-bus", FalconPccState, cardbus_bus, 0xFF),
    DEFINE_PROP_UINT8("subordinate-bus", FalconPccState, subordinate_bus,
                      0xFF),
    DEFINE_PROP_BOOL("lba48", FalconPccState, lba48, false),
    DEFINE_PROP_BOOL("shadow-roms", FalconPccState, shadow_roms, false),
};

/* class_init's data is const in QEMU 11 and not in 10.0 (the two this has
 * been built against). */
#if QEMU_VERSION_MAJOR >= 11
#define FALCON_PCC_CLASS_DATA const void
#else
#define FALCON_PCC_CLASS_DATA void
#endif

static void falcon_pcc_class_init(ObjectClass *klass, FALCON_PCC_CLASS_DATA *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    k->realize = falcon_pcc_realize;
    k->config_read = falcon_pcc_config_read;
    k->config_write = falcon_pcc_config_write;
    k->vendor_id = 0x104C;               /* Texas Instruments */
    k->device_id = 0xAC1B;               /* PCI-1450 */
    k->revision = 0x03;
    k->class_id = 0x0607;                /* CardBus bridge */
    rc->phases.hold = falcon_pcc_reset_hold;
    device_class_set_props(dc, falcon_pcc_props);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->desc = "falcon rig: PC Card bridge, one socket, a CompactFlash card";
}

static const TypeInfo falcon_pcc_types[] = {
    {
        .name          = TYPE_FALCON_PCC,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(FalconPccState),
        .class_init    = falcon_pcc_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    }
};

DEFINE_TYPES(falcon_pcc_types)
