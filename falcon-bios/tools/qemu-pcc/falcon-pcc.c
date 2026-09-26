/*
 * falcon-pcc: a PC Card bridge with a CompactFlash card in it, for falcon
 * jump's rig (docs/DESIGN.md Section 14.9).
 *
 * QEMU models no PC Card bridge on x86, and falcon jump's PCC.MOD has to be
 * proven against one before it meets a ThinkPad A21m's TI PCI-1450. This is
 * that bridge's register interface as a CardBus bridge's 16-bit side
 * presents it, with one CompactFlash card inserted:
 *
 *   - a PCI function with the PCI-1450's IDs and class (0x0607), BAR0 the
 *     CardBus socket registers, the ExCA registers at +0x800;
 *   - ExCA, the i82365SL's register set: interface status, power, interrupt
 *     and general control (reset, card type), window enables, memory window
 *     0 and I/O window 0, and TI's page register for the memory window;
 *   - the windows as real mappings, moved and switched as those registers
 *     are written: memory window 0 onto the card's attribute memory (its
 *     CIS, and the configuration option register at 0x200), I/O window 0
 *     onto its ATA task file once the card is configured for I/O;
 *   - the card: a CIS with the CompactFlash standard's entries (memory
 *     mapped, contiguous I/O, primary IDE), and ATA PIO over a drive:
 *     IDENTIFY DEVICE, READ SECTORS, WRITE SECTORS, LBA28.
 *
 * A model, so it is exactly as right as its reading of the specifications;
 * where the silicon is looser or stricter, only the A21m says. It is test
 * equipment, built into a QEMU for the rig by build-qemu.sh, and never part
 * of falcon.
 *
 * GPL-2.0-or-later, as QEMU is.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/block-backend.h"
#include "system/address-spaces.h"
#include "system/memory.h"

#define TYPE_FALCON_PCC "falcon-pcc"
OBJECT_DECLARE_SIMPLE_TYPE(FalconPccState, FALCON_PCC)

/* CardBus socket registers. */
#define SOCK_PRESENT   0x08
#define SOCK_CONTROL   0x10
#define PS_16BIT       (1u << 4)
#define PS_5V          (1u << 10)
#define PS_3V          (1u << 11)

/* ExCA, at BAR0 + 0x800. */
#define EXCA           0x800
#define X_STATUS       0x01
#define X_POWER        0x02
#define X_INTCTL       0x03
#define X_ENABLE       0x06
#define X_IOCTL        0x07
#define X_IO0          0x08
#define X_MEM0         0x10
#define X_PAGE0        0x40

#define ST_CD          0x0C      /* both card detects */
#define ST_READY       0x20
#define ST_POWERON     0x40
#define IC_RESET       0x40      /* 1: reset released */
#define IC_IOCARD      0x20
#define EN_MEM0        0x01
#define EN_IO0         0x40

#define CONFIG_BASE    0x200
#define ATTR_SPAN      0x1000

/* ATA status. */
#define A_BSY  0x80
#define A_DRDY 0x40
#define A_DRQ  0x08
#define A_ERR  0x01

struct FalconPccState {
    PCIDevice parent_obj;
    MemoryRegion bar;
    MemoryRegion attr;
    MemoryRegion io;
    BlockBackend *blk;
    uint64_t sectors;

    uint32_t sock_control;
    uint8_t volts;          /* 0: a card taking 5 V or 3.3 V; 3: 3.3 V only */
    uint8_t exca[256];
    uint8_t cor;

    uint8_t cis[128];
    unsigned cis_len;

    /* ATA */
    uint8_t feature;
    uint8_t count;
    uint8_t lba[3];
    uint8_t device;
    uint8_t status;
    uint8_t error;
    uint8_t buf[512];
    unsigned pos;
    unsigned remain;
    bool writing;
    uint32_t cur;
};

/* ---- the card's CIS ---- */

static void cis_put(FalconPccState *s, const uint8_t *b, unsigned n)
{
    memcpy(s->cis + s->cis_len, b, n);
    s->cis_len += n;
}

static void cis_build(FalconPccState *s)
{
    /* A CompactFlash card's, trimmed to what a host reads: device info,
     * FUNCID fixed disk, CONFIG (registers at 0x200, last index 3), and the
     * CF standard's entries: 0 memory mapped (default), 1 contiguous I/O on
     * 16 address lines' worth (4 decoded), 2 the primary IDE range. */
    static const uint8_t device[] = { 0x01, 0x03, 0xD9, 0x01, 0xFF };
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
    cis_put(s, funcid, sizeof funcid);
    cis_put(s, config, sizeof config);
    cis_put(s, e0, sizeof e0);
    cis_put(s, e1, sizeof e1);
    cis_put(s, e2, sizeof e2);
    cis_put(s, end, sizeof end);
}

/* ---- window decoding ---- */

/* VCC as the socket control register holds it: 0x20 is 5 V, 0x30 3.3 V.
 * A card that takes only 3.3 V gets nothing at 5 V; the bridge refuses the
 * request, as the PCI-1450 does. */
static bool powered(FalconPccState *s)
{
    uint32_t vcc = s->sock_control & 0x70;

    if (vcc != 0x20 && vcc != 0x30)
        return false;
    if (s->volts == 3 && vcc != 0x30)
        return false;
    return (s->exca[X_POWER] & 0x80) != 0;
}

static bool running(FalconPccState *s)
{
    return powered(s) && (s->exca[X_INTCTL] & IC_RESET) != 0;
}

static void remap(FalconPccState *s)
{
    const uint8_t *x = s->exca;
    bool mem = (x[X_ENABLE] & EN_MEM0) != 0;
    bool io = (x[X_ENABLE] & EN_IO0) != 0 && (x[X_INTCTL] & IC_IOCARD) != 0;

    memory_region_transaction_begin();
    if (mem) {
        uint32_t start = ((uint32_t)x[X_PAGE0] << 24) |
                         ((uint32_t)(x[X_MEM0 + 1] & 0x0F) << 20) |
                         ((uint32_t)x[X_MEM0] << 12);
        uint32_t stop = ((uint32_t)x[X_PAGE0] << 24) |
                        ((uint32_t)(x[X_MEM0 + 3] & 0x0F) << 20) |
                        ((uint32_t)x[X_MEM0 + 2] << 12) | 0xFFF;

        memory_region_set_size(&s->attr, stop >= start ? stop - start + 1
                                                        : 0x1000);
        memory_region_set_address(&s->attr, start);
    }
    memory_region_set_enabled(&s->attr, mem);
    if (io) {
        uint16_t start = (uint16_t)(x[X_IO0] | (x[X_IO0 + 1] << 8));
        uint16_t stop = (uint16_t)(x[X_IO0 + 2] | (x[X_IO0 + 3] << 8));

        memory_region_set_size(&s->io, stop >= start ? stop - start + 1u : 1u);
        memory_region_set_address(&s->io, start);
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
    w[60] = (uint16_t)(s->sectors & 0xFFFF);
    w[61] = (uint16_t)(s->sectors >> 16);
    for (i = 0; i < 256; i++) {
        s->buf[i * 2] = (uint8_t)(w[i] & 0xFF);
        s->buf[i * 2 + 1] = (uint8_t)(w[i] >> 8);
    }
}

static void ata_abort(FalconPccState *s)
{
    s->error = 0x04;
    s->status = A_DRDY | A_ERR;
    s->remain = 0;
}

static bool ata_fetch(FalconPccState *s)
{
    if (s->cur >= s->sectors ||
        blk_pread(s->blk, (int64_t)s->cur * 512, 512, s->buf, 0) < 0) {
        ata_abort(s);
        return false;
    }
    s->pos = 0;
    s->status = A_DRDY | A_DRQ;
    return true;
}

static void ata_command(FalconPccState *s, uint8_t cmd)
{
    uint32_t lba = s->lba[0] | (s->lba[1] << 8) | (s->lba[2] << 16) |
                   ((uint32_t)(s->device & 0x0F) << 24);

    s->error = 0;
    switch (cmd) {
    case 0xEC:                           /* IDENTIFY DEVICE */
        ata_identify(s);
        s->pos = 0;
        s->remain = 1;
        s->writing = false;
        s->status = A_DRDY | A_DRQ;
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
        s->status = A_DRDY | A_DRQ;
        return;
    case 0xEF:                           /* SET FEATURES */
    case 0x91:                           /* INITIALIZE DEVICE PARAMETERS */
    case 0xE5:                           /* CHECK POWER MODE */
        s->status = A_DRDY;
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
        s->status = A_DRDY;
        return;
    }
    if (s->writing) {
        s->pos = 0;
        s->status = A_DRDY | A_DRQ;
    } else {
        ata_fetch(s);
    }
}

static uint64_t io_read(void *opaque, hwaddr off, unsigned size)
{
    FalconPccState *s = opaque;

    if (!running(s) || (s->cor & 0x3F) == 0)
        return size == 2 ? 0xFFFF : 0xFF;
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
    case 0x2: return s->count;
    case 0x3: return s->lba[0];
    case 0x4: return s->lba[1];
    case 0x5: return s->lba[2];
    case 0x6: return s->device;
    case 0x7:
    case 0xE: return s->status;
    default:  return 0xFF;
    }
}

static void io_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    FalconPccState *s = opaque;

    if (!running(s) || (s->cor & 0x3F) == 0)
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
    case 0x2: s->count = (uint8_t)v; return;
    case 0x3: s->lba[0] = (uint8_t)v; return;
    case 0x4: s->lba[1] = (uint8_t)v; return;
    case 0x5: s->lba[2] = (uint8_t)v; return;
    case 0x6: s->device = (uint8_t)v; return;
    case 0x7: ata_command(s, (uint8_t)v); return;
    case 0xE:
        if ((v & 0x04) != 0) {           /* SRST */
            s->status = A_DRDY;
            s->error = 0x01;
            s->remain = 0;
        }
        return;
    default:
        return;
    }
}

static const MemoryRegionOps io_ops = {
    .read = io_read,
    .write = io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 2 },
    .impl = { .min_access_size = 1, .max_access_size = 2 },
};

/* ---- the bridge's BAR: socket registers and ExCA ---- */

static void card_reset(FalconPccState *s)
{
    s->cor = 0;
    s->status = A_DRDY;
    s->error = 0x01;
    s->remain = 0;
}

static uint64_t bar_read(void *opaque, hwaddr off, unsigned size)
{
    FalconPccState *s = opaque;

    if (off == SOCK_PRESENT && size == 4)
        return PS_16BIT | PS_3V |                /* card in, both detects low */
               (s->volts == 3 ? 0 : PS_5V);
    if (off == SOCK_CONTROL && size == 4)
        return s->sock_control;
    if (off >= EXCA && off < EXCA + 0x100 && size == 1) {
        uint8_t r = (uint8_t)(off - EXCA);

        if (r == X_STATUS)
            return ST_CD | (powered(s) ? ST_POWERON : 0) |
                   (running(s) ? ST_READY : 0);
        if (r == 0x00)
            return 0x84;                          /* i82365SL-compatible */
        return s->exca[r];
    }
    return 0;
}

static void bar_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    FalconPccState *s = opaque;

    if (off == SOCK_CONTROL && size == 4) {
        s->sock_control = (uint32_t)v & 0x77;
        /* The ExCA VCC field reads back what the socket was asked for. */
        s->exca[X_POWER] = (uint8_t)((s->exca[X_POWER] & ~0x18) |
            ((s->sock_control & 0x70) == 0x20 ? 0x10 :
             (s->sock_control & 0x70) == 0x30 ? 0x18 : 0));
        if (!powered(s))
            card_reset(s);
        remap(s);
        return;
    }
    if (off >= EXCA && off < EXCA + 0x100 && size == 1) {
        uint8_t r = (uint8_t)(off - EXCA);
        uint8_t old = s->exca[r];

        s->exca[r] = (uint8_t)v;
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
    }
}

static const MemoryRegionOps bar_ops = {
    .read = bar_read,
    .write = bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void falcon_pcc_realize(PCIDevice *pdev, Error **errp)
{
    FalconPccState *s = FALCON_PCC(pdev);
    int64_t len;

    if (!s->blk) {
        error_setg(errp, "falcon-pcc needs drive=, the CF card's image");
        return;
    }
    if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_ALL, errp) < 0)
        return;
    len = blk_getlength(s->blk);
    if (len < 512) {
        error_setg(errp, "falcon-pcc: the card's image is too small");
        return;
    }
    s->sectors = (uint64_t)len / 512;
    if (s->sectors > 0x0FFFFFFF)
        s->sectors = 0x0FFFFFFF;
    cis_build(s);
    card_reset(s);

    memory_region_init_io(&s->bar, OBJECT(s), &bar_ops, s, "falcon-pcc-bar",
                          4 * KiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);

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
};

static void falcon_pcc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = falcon_pcc_realize;
    k->vendor_id = 0x104C;               /* Texas Instruments */
    k->device_id = 0xAC1B;               /* PCI-1450 */
    k->revision = 0x03;
    k->class_id = 0x0607;                /* CardBus bridge */
    device_class_set_props(dc, falcon_pcc_props);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->desc = "falcon rig: PC Card bridge with a CompactFlash card";
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
