/* falcon jump's USB modules: the byte layouts of USB (devices, hubs, boot
 * keyboards) and of Bulk-Only Transport, apart from any host controller
 * (DESIGN Section 13.4).
 *
 * A setup packet, a descriptor, a command block wrapper: each is a layout a
 * driver must get exactly right, and gets no error back for getting wrong,
 * only a device that does not answer or a read of the wrong block. So they
 * are here, host-tested; a host controller driver (uhci.c) moves the bytes.
 */

#ifndef FALCON_BIOS_USBCORE_H
#define FALCON_BIOS_USBCORE_H

#include <stddef.h>
#include <stdint.h>

#define USB_SETUP_LEN 8u

#define USB_DESC_DEVICE    1u
#define USB_DESC_CONFIG    2u
#define USB_DESC_INTERFACE 4u
#define USB_DESC_ENDPOINT  5u
#define USB_DESC_HUB       0x29u

#define USB_CLASS_HUB      9u

/* The standard requests, as setup packets. */
void usb_setup_get_descriptor(uint8_t out[USB_SETUP_LEN], uint8_t type,
                              uint8_t index, uint16_t len);
void usb_setup_set_address(uint8_t out[USB_SETUP_LEN], uint8_t addr);
void usb_setup_set_config(uint8_t out[USB_SETUP_LEN], uint8_t value);
/* CLEAR_FEATURE(ENDPOINT_HALT) on endpoint address `ep`. */
void usb_setup_clear_halt(uint8_t out[USB_SETUP_LEN], uint8_t ep);
/* Bulk-Only Mass Storage Reset, to interface `iface`. */
void usb_setup_bot_reset(uint8_t out[USB_SETUP_LEN], uint8_t iface);

typedef struct {
    uint8_t  dev_class;      /* bDeviceClass: USB_CLASS_HUB for a hub */
    uint8_t  max_packet0;    /* endpoint 0's packet size */
    uint16_t vendor;
    uint16_t product;
    uint8_t  n_configs;
} usb_device_desc;

/* The device descriptor; at least 8 bytes give max_packet0, 18 the rest.
 * 0 on success, -1 when it is not a device descriptor. */
int usb_parse_device(const uint8_t *d, size_t len, usb_device_desc *out);

/* The configuration descriptor's wTotalLength, from its first 9 bytes. 0
 * when it is not one. */
uint16_t usb_config_total(const uint8_t *d, size_t len);

typedef struct {
    uint8_t  config;         /* bConfigurationValue */
    uint8_t  iface;          /* bInterfaceNumber */
    uint8_t  ep_in;          /* endpoint address, 0x80 set */
    uint8_t  ep_out;
    uint16_t mps_in;
    uint16_t mps_out;
} usb_msc_if;

/* Walks a whole configuration descriptor (`len` bytes of it) for a mass
 * storage interface that speaks SCSI over Bulk-Only Transport (class 8,
 * subclass 6, protocol 0x50) with one bulk IN and one bulk OUT endpoint.
 * 0 when it finds one; -1 when it does not, or the descriptor is malformed. */
int usb_find_msc(const uint8_t *cfg, size_t len, usb_msc_if *out);

/* ---- HID boot keyboards (HID 1.11 Sections 7.2 and B.1) ---- */

/* SET_PROTOCOL to interface `iface`: 0 is the boot protocol, whose 8-byte
 * report needs no report descriptor parsed. */
void usb_setup_set_protocol(uint8_t out[USB_SETUP_LEN], uint8_t iface,
                            uint8_t protocol);
/* SET_IDLE with duration 0: a report only when something changes. */
void usb_setup_set_idle(uint8_t out[USB_SETUP_LEN], uint8_t iface);

typedef struct {
    uint8_t  config;         /* bConfigurationValue */
    uint8_t  iface;
    uint8_t  ep;             /* the interrupt IN endpoint, 0x80 set */
    uint16_t mps;
} usb_kbd_if;

/* Walks a whole configuration descriptor for a boot-protocol keyboard
 * interface (class 3, subclass 1, protocol 1) and its interrupt IN
 * endpoint. 0 when it finds one; -1 when it does not, or the descriptor is
 * malformed. */
int usb_find_kbd(const uint8_t *cfg, size_t len, usb_kbd_if *out);

/* ---- hubs (USB 2.0 Section 11.24) ---- */

#define HUB_PORT_RESET        4u    /* features, for SET and CLEAR_FEATURE */
#define HUB_PORT_POWER        8u
#define HUB_C_PORT_CONNECTION 16u
#define HUB_C_PORT_ENABLE     17u
#define HUB_C_PORT_RESET      20u

#define HUB_PS_CONNECTION 0x0001u   /* wPortStatus */
#define HUB_PS_ENABLE     0x0002u
#define HUB_PS_RESET      0x0010u
#define HUB_PS_LOW        0x0200u
#define HUB_PC_RESET      0x0010u   /* wPortChange */

/* GET_DESCRIPTOR for the hub class descriptor, `len` bytes of it. */
void usb_setup_hub_descriptor(uint8_t out[USB_SETUP_LEN], uint16_t len);
/* SET_FEATURE (`set`) or CLEAR_FEATURE of `feature` on downstream port
 * `port`, numbered from 1. */
void usb_setup_port_feature(uint8_t out[USB_SETUP_LEN], uint8_t port,
                            uint16_t feature, int set);
/* GET_STATUS of downstream port `port`: four bytes back. */
void usb_setup_port_status(uint8_t out[USB_SETUP_LEN], uint8_t port);

typedef struct {
    uint8_t  ports;          /* bNbrPorts */
    uint16_t power_ms;       /* bPwrOn2PwrGood, in milliseconds */
} usb_hub_desc;

/* The hub descriptor's first seven bytes. 0, or -1 when it is not one. */
int usb_parse_hub(const uint8_t *d, size_t len, usb_hub_desc *out);

/* A port's GET_STATUS answer: wPortStatus and wPortChange. 0, or -1 when
 * it is short. */
int usb_parse_port_status(const uint8_t *d, size_t len, uint16_t *status,
                          uint16_t *change);

/* ---- Bulk-Only Transport ---- */

#define BOT_CBW_LEN 31u
#define BOT_CSW_LEN 13u

#define BOT_STATUS_PASSED 0
#define BOT_STATUS_FAILED 1
#define BOT_STATUS_PHASE  2

/* The command block wrapper for `cdb` (1-16 bytes), a data stage of
 * `data_len` bytes in the direction `in` says. Returns BOT_CBW_LEN, or 0 on
 * a bad argument. */
size_t bot_cbw(uint8_t out[BOT_CBW_LEN], uint32_t tag, uint32_t data_len,
               int in, uint8_t lun, const uint8_t *cdb, size_t cdb_len);

/* Checks a command status wrapper against the CBW's tag: its status
 * (BOT_STATUS_*) with *residue set, or -1 when it is not the CSW for that
 * command, which a driver answers with a reset. */
int bot_csw_check(const uint8_t *csw, size_t len, uint32_t tag,
                  uint32_t *residue);

#endif /* FALCON_BIOS_USBCORE_H */
