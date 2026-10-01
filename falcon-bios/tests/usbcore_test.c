/* Tests for falcon-bios/src/hw/usb/usbcore.c and hw/scsi/scsi.c: the USB
 * (devices, hubs, boot keyboards) and Bulk-Only Transport layouts falcon
 * jump's USB modules send and read (DESIGN Section 13.4), and the SCSI
 * answers a stick gives.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <hw/usb/usbcore.h>
#include <hw/scsi/scsi.h>

static void test_setup_packets(void **state)
{
    uint8_t p[USB_SETUP_LEN];
    static const uint8_t get_cfg[] = { 0x80, 0x06, 0x00, 0x02, 0, 0, 0xFF, 0 };
    static const uint8_t addr[] = { 0x00, 0x05, 0x03, 0, 0, 0, 0, 0 };
    static const uint8_t cfg[] = { 0x00, 0x09, 0x01, 0, 0, 0, 0, 0 };
    static const uint8_t halt[] = { 0x02, 0x01, 0, 0, 0x81, 0, 0, 0 };
    static const uint8_t reset[] = { 0x21, 0xFF, 0, 0, 0x01, 0, 0, 0 };

    (void)state;
    usb_setup_get_descriptor(p, USB_DESC_CONFIG, 0, 255u);
    assert_memory_equal(p, get_cfg, sizeof p);
    usb_setup_set_address(p, 3u);
    assert_memory_equal(p, addr, sizeof p);
    usb_setup_set_config(p, 1u);
    assert_memory_equal(p, cfg, sizeof p);
    usb_setup_clear_halt(p, 0x81u);
    assert_memory_equal(p, halt, sizeof p);
    usb_setup_bot_reset(p, 1u);
    assert_memory_equal(p, reset, sizeof p);
}

/* QEMU's usb-storage: full speed, max packet 64 on endpoint 0. */
static const uint8_t k_device[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x27, 0x06, 0x01, 0x00,
    0x00, 0x00, 1, 2, 3, 1
};

static void test_device(void **state)
{
    usb_device_desc d;
    uint8_t bad[18];

    (void)state;
    assert_int_equal(usb_parse_device(k_device, 8u, &d), 0);
    assert_int_equal(d.max_packet0, 64);
    assert_int_equal(d.dev_class, 0);
    assert_int_equal(usb_parse_device(k_device, sizeof k_device, &d), 0);
    assert_int_equal(d.vendor, 0x0627u);
    assert_int_equal(d.n_configs, 1);

    memcpy(bad, k_device, sizeof bad);
    bad[7] = 7;                         /* not a legal packet size */
    assert_int_equal(usb_parse_device(bad, sizeof bad, &d), -1);
    bad[7] = 64;
    bad[1] = USB_DESC_CONFIG;
    assert_int_equal(usb_parse_device(bad, sizeof bad, &d), -1);
    assert_int_equal(usb_parse_device(k_device, 7u, &d), -1);
}

/* A configuration with one interface, SCSI over Bulk-Only, in 0x81 out 0x02. */
static const uint8_t k_msc_cfg[] = {
    9, 2, 32, 0, 1, 1, 0, 0xC0, 50,
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,
    7, 5, 0x81, 0x02, 64, 0, 0,
    7, 5, 0x02, 0x02, 64, 0, 0,
};

/* A composite stick: a HID interface with an interrupt endpoint first, then
 * storage, whose OUT endpoint comes before its IN. */
static const uint8_t k_composite_cfg[] = {
    9, 2, 55, 0, 2, 3, 0, 0x80, 100,
    9, 4, 0, 0, 1, 0x03, 0x01, 0x01, 0,
    7, 5, 0x83, 0x03, 8, 0, 10,
    9, 4, 1, 0, 2, 0x08, 0x06, 0x50, 0,
    7, 5, 0x04, 0x02, 0x00, 0x02, 0,
    7, 5, 0x85, 0x02, 0x00, 0x02, 0,
    7, 0x21, 0, 0, 0, 0, 0,             /* a class descriptor, skipped */
};

static void test_find_msc(void **state)
{
    usb_msc_if m;
    uint8_t bad[sizeof k_msc_cfg];

    (void)state;
    assert_int_equal(usb_config_total(k_msc_cfg, 9u), 32u);
    assert_int_equal(usb_find_msc(k_msc_cfg, sizeof k_msc_cfg, &m), 0);
    assert_int_equal(m.config, 1);
    assert_int_equal(m.iface, 0);
    assert_int_equal(m.ep_in, 0x81);
    assert_int_equal(m.ep_out, 0x02);
    assert_int_equal(m.mps_in, 64);

    assert_int_equal(usb_find_msc(k_composite_cfg, sizeof k_composite_cfg,
                                  &m), 0);
    assert_int_equal(m.config, 3);
    assert_int_equal(m.iface, 1);
    assert_int_equal(m.ep_in, 0x85);
    assert_int_equal(m.ep_out, 0x04);
    assert_int_equal(m.mps_out, 512);

    /* A storage interface that speaks CBI, not Bulk-Only. */
    memcpy(bad, k_msc_cfg, sizeof bad);
    bad[16] = 0x00;
    assert_int_equal(usb_find_msc(bad, sizeof bad, &m), -1);
    /* A zero-length descriptor would loop forever unrefused. */
    memcpy(bad, k_msc_cfg, sizeof bad);
    bad[18] = 0;
    assert_int_equal(usb_find_msc(bad, sizeof bad, &m), -1);
    /* One running past the end. */
    assert_int_equal(usb_find_msc(k_msc_cfg, sizeof k_msc_cfg - 3u, &m), -1);
    assert_int_equal(usb_config_total(k_device, sizeof k_device), 0);
}

static void test_find_kbd(void **state)
{
    usb_kbd_if k;
    uint8_t bad[sizeof k_composite_cfg];

    (void)state;
    assert_int_equal(usb_find_kbd(k_composite_cfg, sizeof k_composite_cfg,
                                  &k), 0);
    assert_int_equal(k.config, 3);
    assert_int_equal(k.iface, 0);
    assert_int_equal(k.ep, 0x83);
    assert_int_equal(k.mps, 8);
    assert_int_equal(usb_find_kbd(k_msc_cfg, sizeof k_msc_cfg, &k), -1);

    /* A mouse: boot interface subclass, protocol 2. */
    memcpy(bad, k_composite_cfg, sizeof bad);
    bad[16] = 0x02;
    assert_int_equal(usb_find_kbd(bad, sizeof bad, &k), -1);
    /* A keyboard whose only endpoint is an OUT. */
    memcpy(bad, k_composite_cfg, sizeof bad);
    bad[20] = 0x03;
    assert_int_equal(usb_find_kbd(bad, sizeof bad, &k), -1);
    memcpy(bad, k_composite_cfg, sizeof bad);
    bad[18] = 0;
    assert_int_equal(usb_find_kbd(bad, sizeof bad, &k), -1);
}

/* QEMU's usb-hub: eight ports, bPwrOn2PwrGood 10 (20 ms). */
static const uint8_t k_hub[] = { 0x0A, 0x29, 8, 0x0A, 0x00, 10, 0, 0, 0xFF };

static void test_hub(void **state)
{
    static const uint8_t get_hub[] = { 0xA0, 0x06, 0x00, 0x29, 0, 0, 8, 0 };
    static const uint8_t power[] = { 0x23, 0x03, 8, 0, 3, 0, 0, 0 };
    static const uint8_t c_reset[] = { 0x23, 0x01, 20, 0, 1, 0, 0, 0 };
    static const uint8_t status[] = { 0xA3, 0x00, 0, 0, 2, 0, 4, 0 };
    static const uint8_t answer[] = { 0x03, 0x03, 0x10, 0x00 };
    uint8_t p[USB_SETUP_LEN];
    usb_hub_desc h;
    uint16_t st = 0;
    uint16_t ch = 0;
    uint8_t dev[18];

    (void)state;
    usb_setup_hub_descriptor(p, 8u);
    assert_memory_equal(p, get_hub, sizeof p);
    usb_setup_port_feature(p, 3u, HUB_PORT_POWER, 1);
    assert_memory_equal(p, power, sizeof p);
    usb_setup_port_feature(p, 1u, HUB_C_PORT_RESET, 0);
    assert_memory_equal(p, c_reset, sizeof p);
    usb_setup_port_status(p, 2u);
    assert_memory_equal(p, status, sizeof p);

    assert_int_equal(usb_parse_hub(k_hub, sizeof k_hub, &h), 0);
    assert_int_equal(h.ports, 8);
    assert_int_equal(h.power_ms, 20);
    assert_int_equal(usb_parse_hub(k_hub, 6u, &h), -1);
    assert_int_equal(usb_parse_hub(k_device, sizeof k_device, &h), -1);

    /* Connected, enabled, low speed; the reset finished. */
    assert_int_equal(usb_parse_port_status(answer, sizeof answer, &st, &ch),
                     0);
    assert_int_equal(st & HUB_PS_CONNECTION, HUB_PS_CONNECTION);
    assert_int_equal(st & HUB_PS_ENABLE, HUB_PS_ENABLE);
    assert_int_equal(st & HUB_PS_LOW, HUB_PS_LOW);
    assert_int_equal(ch & HUB_PC_RESET, HUB_PC_RESET);
    assert_int_equal(usb_parse_port_status(answer, 3u, &st, &ch), -1);

    /* A hub says so in its device descriptor. */
    memcpy(dev, k_device, sizeof dev);
    dev[4] = USB_CLASS_HUB;
    {
        usb_device_desc d;

        assert_int_equal(usb_parse_device(dev, 8u, &d), 0);
        assert_int_equal(d.dev_class, USB_CLASS_HUB);
    }
}

static void test_hid_requests(void **state)
{
    static const uint8_t proto[] = { 0x21, 0x0B, 0, 0, 1, 0, 0, 0 };
    static const uint8_t idle[] = { 0x21, 0x0A, 0, 0, 1, 0, 0, 0 };
    uint8_t p[USB_SETUP_LEN];

    (void)state;
    usb_setup_set_protocol(p, 1u, 0u);
    assert_memory_equal(p, proto, sizeof p);
    usb_setup_set_idle(p, 1u);
    assert_memory_equal(p, idle, sizeof p);
}

static void test_bot(void **state)
{
    uint8_t cdb[SCSI_CDB_MAX];
    uint8_t cbw[BOT_CBW_LEN];
    uint8_t csw[BOT_CSW_LEN] = {
        0x55, 0x53, 0x42, 0x53, 0x07, 0, 0, 0, 0x00, 0x02, 0, 0, 0
    };
    uint32_t residue = 99;
    size_t n;

    (void)state;
    n = scsi_cdb_read10(cdb, 0x800u, 4u);
    assert_int_equal(n, 10);
    assert_int_equal(bot_cbw(cbw, 7u, 2048u, 1, 0, cdb, n), BOT_CBW_LEN);
    assert_memory_equal(cbw, "USBC", 4);
    assert_int_equal(cbw[4], 7);
    assert_int_equal(cbw[8], 0x00);
    assert_int_equal(cbw[9], 0x08);     /* 2048, little endian */
    assert_int_equal(cbw[12], 0x80);
    assert_int_equal(cbw[14], 10);
    assert_int_equal(cbw[15], SCSI_READ_10);
    assert_int_equal(cbw[20], 0x00);    /* the LBA's low byte */
    assert_int_equal(cbw[19], 0x08);
    assert_int_equal(cbw[25], 0);       /* past the CDB, zero */
    assert_int_equal(bot_cbw(cbw, 1u, 0, 0, 16u, cdb, n), 0);
    assert_int_equal(bot_cbw(cbw, 1u, 0, 0, 0, cdb, 17u), 0);

    assert_int_equal(bot_csw_check(csw, sizeof csw, 7u, &residue),
                     BOT_STATUS_PASSED);
    assert_int_equal(residue, 0x200u);
    csw[12] = BOT_STATUS_FAILED;
    assert_int_equal(bot_csw_check(csw, sizeof csw, 7u, &residue),
                     BOT_STATUS_FAILED);
    assert_int_equal(bot_csw_check(csw, sizeof csw, 8u, &residue), -1);
    csw[12] = 3;
    assert_int_equal(bot_csw_check(csw, sizeof csw, 7u, &residue), -1);
    csw[12] = 0;
    csw[0] = 0;
    assert_int_equal(bot_csw_check(csw, sizeof csw, 7u, &residue), -1);
    assert_int_equal(bot_csw_check(csw, 12u, 7u, &residue), -1);
}

/* A 16 GB stick's READ CAPACITY: 512-byte blocks. */
static void test_scsi_capacity(void **state)
{
    static const uint8_t cap[8] = { 0x01, 0xDC, 0xFF, 0xFF, 0, 0, 2, 0 };
    static const uint8_t zero[8] = { 0, 0, 0x10, 0, 0, 0, 0, 0 };
    uint64_t n = 0;
    uint32_t bs = 0;
    uint8_t cdb[SCSI_CDB_MAX];

    (void)state;
    assert_int_equal(scsi_parse_capacity(cap, &n, &bs), 0);
    assert_int_equal(n, 0x01DD0000u);
    assert_int_equal(bs, 512);
    assert_int_equal(scsi_parse_capacity(zero, &n, &bs), -1);
    assert_int_equal(scsi_cdb_inquiry(cdb, 36u), 6);
    assert_int_equal(cdb[0], SCSI_INQUIRY);
    assert_int_equal(cdb[4], 36);
    assert_int_equal(scsi_cdb_read_capacity(cdb), 10);
    assert_int_equal(scsi_cdb_write10(cdb, 2047u, 1u), 10);
    assert_int_equal(cdb[0], SCSI_WRITE_10);
    assert_int_equal(cdb[4], 0x07);
    assert_int_equal(cdb[5], 0xFF);
    assert_int_equal(cdb[8], 1);
}

int run_usbcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_setup_packets),
        cmocka_unit_test(test_device),
        cmocka_unit_test(test_find_msc),
        cmocka_unit_test(test_find_kbd),
        cmocka_unit_test(test_hub),
        cmocka_unit_test(test_hid_requests),
        cmocka_unit_test(test_bot),
        cmocka_unit_test(test_scsi_capacity),
    };

    return cmocka_run_group_tests_name("usbcore", tests, NULL, NULL);
}
