/* falcon jump's USB modules: USB mass storage over whatever host controller
 * the module was linked with (DESIGN Section 14.4 and 14.6).
 *
 * Enumeration of the root ports, then SCSI over Bulk-Only Transport, then
 * the module's operations and its resident INT 13h (hw/blk13.c). The host
 * controller is a usbhc (usbhc.h): UHCI.MOD links uhci.c, and OHCI and EHCI
 * modules link the same file with theirs. Root ports only; a hub is a later
 * change.
 *
 * Like every module, what the resident reaches uses nothing of the core
 * (jump/jmod.h): the services table is dropped when serve() is called.
 */

#include <hw/usb/msc.h>
#include <hw/usb/usbcore.h>
#include <hw/scsi/scsi.h>
#include <hw/blk13.h>
#include <hw/modrt.h>

#include <lib/string.h>

#define DEVS_MAX 4u
#define SECTOR   512u
/* The most one READ(10) or WRITE(10) carries: 16 KB, what a UHCI bulk call
 * moves before it loops. */
#define MAX_COUNT 32u

typedef struct {
    uint8_t    addr;
    uint8_t    mps0;
    int        low;
    usb_msc_if m;
    uint8_t    toggle_in;
    uint8_t    toggle_out;
    uint64_t   sectors;
    char       label[41];
} msc_dev;

static const usbhc *g_hc;
static const jmod_services *g_svc;   /* probe time only */
static msc_dev g_dev[DEVS_MAX];
static unsigned g_ndev;
static unsigned g_serve = DEVS_MAX;
static uint32_t g_tag;

static int serve_read(uint64_t lba, uint32_t count, void *buf);
static int serve_write(uint64_t lba, uint32_t count, const void *buf);
static blk13_dev g_blk = {
    SECTOR, 0, 1, serve_read, serve_write, MAX_COUNT
};

static void log_s(const char *s)
{
    if (g_svc != (const jmod_services *)0)
        g_svc->log(s);
}

static int clear_halt(msc_dev *d, uint8_t ep)
{
    uint8_t setup[USB_SETUP_LEN];

    usb_setup_clear_halt(setup, ep);
    if ((ep & 0x80u) != 0)
        d->toggle_in = 0;
    else
        d->toggle_out = 0;
    return g_hc->control(d->addr, d->mps0, d->low, setup, (void *)0, 0);
}

/* Bulk-Only's reset recovery: the class reset, then both endpoints' halts
 * cleared. */
static void reset_recovery(msc_dev *d)
{
    uint8_t setup[USB_SETUP_LEN];

    usb_setup_bot_reset(setup, d->m.iface);
    (void)g_hc->control(d->addr, d->mps0, d->low, setup, (void *)0, 0);
    (void)clear_halt(d, d->m.ep_in);
    (void)clear_halt(d, d->m.ep_out);
}

static int bulk(msc_dev *d, int in, void *buf, uint32_t len, uint32_t *done)
{
    return g_hc->bulk(d->addr, in ? d->m.ep_in : d->m.ep_out,
                      in ? d->m.mps_in : d->m.mps_out, buf, len, done,
                      in ? &d->toggle_in : &d->toggle_out);
}

/* One SCSI command over Bulk-Only: the wrapper, the data, the status. 0 on
 * success, 1 when the device says the command failed (a REQUEST SENSE is
 * owed), -1 on a transport failure, after which the device has been reset. */
static int command(msc_dev *d, const uint8_t *cdb, size_t cdb_len, int in,
                   void *data, uint32_t len)
{
    static uint8_t cbw[BOT_CBW_LEN];
    static uint8_t csw[BOT_CSW_LEN];
    uint32_t done;
    uint32_t tag = ++g_tag;
    int rc;
    int status;

    if (bot_cbw(cbw, tag, len, in, 0, cdb, cdb_len) == 0)
        return -1;
    rc = bulk(d, 0, cbw, BOT_CBW_LEN, &done);
    if (rc != USBHC_OK) {
        reset_recovery(d);
        return -1;
    }
    if (len != 0) {
        rc = bulk(d, in, data, len, &done);
        if (rc == USBHC_STALL)
            (void)clear_halt(d, in ? d->m.ep_in : d->m.ep_out);
        else if (rc != USBHC_OK) {
            reset_recovery(d);
            return -1;
        }
    }
    rc = bulk(d, 1, csw, BOT_CSW_LEN, &done);
    if (rc == USBHC_STALL) {
        (void)clear_halt(d, d->m.ep_in);
        rc = bulk(d, 1, csw, BOT_CSW_LEN, &done);
    }
    status = rc == USBHC_OK ? bot_csw_check(csw, done, tag, (uint32_t *)0)
                            : -1;
    if (status < 0 || status == BOT_STATUS_PHASE) {
        reset_recovery(d);
        return -1;
    }
    return status == BOT_STATUS_PASSED ? 0 : 1;
}

static int sense_key(msc_dev *d)
{
    uint8_t cdb[SCSI_CDB_MAX];
    uint8_t data[18];
    size_t n = scsi_cdb_request_sense(cdb, (uint8_t)sizeof data);

    if (command(d, cdb, n, 1, data, sizeof data) != 0)
        return -1;
    return scsi_parse_sense(data, sizeof data);
}

/* A command that may meet a stick still starting up: a UNIT ATTENTION is
 * taken and retried, a NOT READY waited out for up to ten seconds. */
static int command_retry(msc_dev *d, const uint8_t *cdb, size_t cdb_len,
                         int in, void *data, uint32_t len)
{
    unsigned tries;

    for (tries = 0; tries < 20u; tries++) {
        int rc = command(d, cdb, cdb_len, in, data, len);
        int key;

        if (rc == 0)
            return 0;
        key = sense_key(d);
        if (key == (int)SENSE_UNIT_ATTENTION)
            continue;
        if (key != (int)SENSE_NOT_READY && rc != -1)
            return -1;
        io_delay_us(500000u);
    }
    return -1;
}

static void label_from_inquiry(msc_dev *d, const uint8_t *q)
{
    size_t n = 0;
    size_t i;

    /* Vendor (8-15) and product (16-31), with one space between. */
    for (i = 8; i < 32u && n + 1u < sizeof d->label; i++) {
        uint8_t c = q[i];

        if (i == 16u && n > 0 && d->label[n - 1] != ' ')
            d->label[n++] = ' ';
        if (c >= (uint8_t)' ' && c <= (uint8_t)'~')
            d->label[n++] = (char)c;
        else
            d->label[n++] = ' ';
    }
    while (n > 0 && d->label[n - 1] == ' ')
        n--;
    d->label[n] = '\0';
}

/* The device on `port`, as address port + 1. 0 when it is a mass storage
 * device this module can read. */
static int enumerate(unsigned port, msc_dev *d)
{
    static uint8_t buf[256];
    uint8_t setup[USB_SETUP_LEN];
    usb_device_desc dd;
    uint16_t total;
    uint8_t cdb[SCSI_CDB_MAX];
    size_t n;
    uint32_t block;

    memset(d, 0, sizeof *d);
    if (g_hc->port_reset(port, &d->low) != 1)
        return -1;

    /* Its first eight bytes at address 0, which is where it answers until
     * it is given one; endpoint 0's packet size is in them. */
    usb_setup_get_descriptor(setup, USB_DESC_DEVICE, 0, 8u);
    if (g_hc->control(0, 8u, d->low, setup, buf, 8u) != USBHC_OK ||
        usb_parse_device(buf, 8u, &dd) != 0)
        return -1;
    d->mps0 = dd.max_packet0;
    usb_setup_set_address(setup, (uint8_t)(port + 1u));
    if (g_hc->control(0, d->mps0, d->low, setup, (void *)0, 0) != USBHC_OK)
        return -1;
    d->addr = (uint8_t)(port + 1u);
    io_delay_us(10000);              /* SET_ADDRESS recovery */

    usb_setup_get_descriptor(setup, USB_DESC_CONFIG, 0, 9u);
    if (g_hc->control(d->addr, d->mps0, d->low, setup, buf, 9u) != USBHC_OK)
        return -1;
    total = usb_config_total(buf, 9u);
    if (total < 9u)
        return -1;
    if (total > sizeof buf)
        total = (uint16_t)sizeof buf;
    usb_setup_get_descriptor(setup, USB_DESC_CONFIG, 0, total);
    if (g_hc->control(d->addr, d->mps0, d->low, setup, buf, total) !=
            USBHC_OK ||
        usb_find_msc(buf, total, &d->m) != 0) {
        log_s("falcon-bios: USB: a device that is not mass storage\n");
        return -1;
    }
    usb_setup_set_config(setup, d->m.config);
    if (g_hc->control(d->addr, d->mps0, d->low, setup, (void *)0, 0) !=
        USBHC_OK)
        return -1;

    n = scsi_cdb_inquiry(cdb, 36u);
    if (command_retry(d, cdb, n, 1, buf, 36u) != 0)
        return -1;
    label_from_inquiry(d, buf);
    n = scsi_cdb_test_unit_ready(cdb);
    (void)command_retry(d, cdb, n, 0, (void *)0, 0);
    n = scsi_cdb_read_capacity(cdb);
    if (command_retry(d, cdb, n, 1, buf, 8u) != 0 ||
        scsi_parse_capacity(buf, &d->sectors, &block) != 0)
        return -1;
    if (block != SECTOR) {
        log_s("falcon-bios: USB: a stick whose blocks are not 512 bytes\n");
        return -1;
    }
    return 0;
}

static unsigned msc_probe(void)
{
    unsigned ports = g_hc->start();
    unsigned p;

    g_ndev = 0;
    for (p = 0; p < ports && g_ndev < DEVS_MAX; p++) {
        if (enumerate(p, &g_dev[g_ndev]) != 0)
            continue;
        log_s("falcon-bios: USB: ");
        log_s(g_dev[g_ndev].label);
        log_s("\n");
        g_ndev++;
    }
    return g_ndev;
}

static int msc_dev_info(unsigned idx, jmod_dev *out)
{
    if (idx >= g_ndev || out == (jmod_dev *)0)
        return -1;
    out->kind = JMOD_DEV_DISK;
    out->sector_size = SECTOR;
    out->sectors = g_dev[idx].sectors;
    memcpy(out->label, g_dev[idx].label, sizeof out->label);
    return 0;
}

static int rw(unsigned idx, uint64_t lba, uint32_t count, void *buf,
              int write)
{
    uint8_t *p = buf;

    if (idx >= g_ndev || lba + count > g_dev[idx].sectors ||
        lba > 0xFFFFFFFFu)
        return -1;
    while (count > 0) {
        uint8_t cdb[SCSI_CDB_MAX];
        uint32_t n = count > MAX_COUNT ? MAX_COUNT : count;
        size_t len = write ? scsi_cdb_write10(cdb, (uint32_t)lba, (uint16_t)n)
                           : scsi_cdb_read10(cdb, (uint32_t)lba, (uint16_t)n);

        if (command_retry(&g_dev[idx], cdb, len, !write, p, n * SECTOR) != 0)
            return -1;
        p += n * SECTOR;
        lba += n;
        count -= n;
    }
    return 0;
}

static int msc_read(unsigned idx, uint64_t lba, uint32_t count, void *buf)
{
    return rw(idx, lba, count, buf, 0);
}

static int serve_read(uint64_t lba, uint32_t count, void *buf)
{
    return rw(g_serve, lba, count, buf, 0);
}

static int serve_write(uint64_t lba, uint32_t count, const void *buf)
{
    /* The one write a falcon stick's loader makes is clearing its
     * next-boot marker (DESIGN Section 6.3.1). */
    return rw(g_serve, lba, count, (void *)(uintptr_t)buf, 1);
}

static int msc_serve(unsigned idx, uint8_t drive)
{
    (void)drive;
    if (idx >= g_ndev)
        return -1;
    g_serve = idx;
    g_blk.sectors = g_dev[idx].sectors;
    g_svc = (const jmod_services *)0;
    return 0;
}

static void msc_int13(jmod_regs *r)
{
    if (g_serve >= g_ndev) {
        r->eax = (r->eax & 0xFFFF00FFu) | 0xAA00u;   /* drive not ready */
        r->flags = 1;
        return;
    }
    blk13_call(&g_blk, r);
}

int msc_init(const usbhc *hc, const jmod_services *svc, jmod_ops *ops)
{
    g_hc = hc;
    g_svc = svc;
    ops->probe = msc_probe;
    ops->dev = msc_dev_info;
    ops->read = msc_read;
    ops->serve = msc_serve;
    ops->int13 = msc_int13;
    return 0;
}
