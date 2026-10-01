/* falcon jump's USB modules: USB and Bulk-Only Transport layouts. See
 * usbcore.h. */

#include <hw/usb/usbcore.h>

#define BOT_CBW_SIG 0x43425355u   /* "USBC" */
#define BOT_CSW_SIG 0x53425355u   /* "USBS" */

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void setup(uint8_t out[USB_SETUP_LEN], uint8_t type, uint8_t req,
                  uint16_t value, uint16_t index, uint16_t len)
{
    out[0] = type;
    out[1] = req;
    wr16(out + 2, value);
    wr16(out + 4, index);
    wr16(out + 6, len);
}

void usb_setup_get_descriptor(uint8_t out[USB_SETUP_LEN], uint8_t type,
                              uint8_t index, uint16_t len)
{
    setup(out, 0x80u, 0x06u, (uint16_t)(((unsigned)type << 8) | index), 0,
          len);
}

void usb_setup_set_address(uint8_t out[USB_SETUP_LEN], uint8_t addr)
{
    setup(out, 0x00u, 0x05u, addr, 0, 0);
}

void usb_setup_set_config(uint8_t out[USB_SETUP_LEN], uint8_t value)
{
    setup(out, 0x00u, 0x09u, value, 0, 0);
}

void usb_setup_clear_halt(uint8_t out[USB_SETUP_LEN], uint8_t ep)
{
    setup(out, 0x02u, 0x01u, 0, ep, 0);
}

void usb_setup_bot_reset(uint8_t out[USB_SETUP_LEN], uint8_t iface)
{
    setup(out, 0x21u, 0xFFu, 0, iface, 0);
}

int usb_parse_device(const uint8_t *d, size_t len, usb_device_desc *out)
{
    if (d == (const uint8_t *)0 || out == (usb_device_desc *)0 || len < 8u ||
        d[1] != USB_DESC_DEVICE || d[0] < 8u)
        return -1;
    out->dev_class = d[4];
    out->max_packet0 = d[7];
    out->vendor = len >= 12u ? rd16(d + 8) : 0;
    out->product = len >= 12u ? rd16(d + 10) : 0;
    out->n_configs = len >= 18u ? d[17] : 0;
    if (out->max_packet0 != 8u && out->max_packet0 != 16u &&
        out->max_packet0 != 32u && out->max_packet0 != 64u)
        return -1;
    return 0;
}

uint16_t usb_config_total(const uint8_t *d, size_t len)
{
    if (d == (const uint8_t *)0 || len < 9u || d[1] != USB_DESC_CONFIG ||
        d[0] < 9u)
        return 0;
    return rd16(d + 2);
}

int usb_find_msc(const uint8_t *cfg, size_t len, usb_msc_if *out)
{
    size_t at;
    int in_msc = 0;

    if (cfg == (const uint8_t *)0 || out == (usb_msc_if *)0 || len < 9u ||
        cfg[1] != USB_DESC_CONFIG)
        return -1;
    out->config = cfg[5];
    out->ep_in = 0;
    out->ep_out = 0;

    for (at = 0; at + 2u <= len;) {
        uint8_t dlen = cfg[at];
        uint8_t type = cfg[at + 1u];

        if (dlen < 2u || at + dlen > len)
            return -1;
        if (type == USB_DESC_INTERFACE && dlen >= 9u) {
            if (in_msc && out->ep_in != 0 && out->ep_out != 0)
                return 0;
            in_msc = cfg[at + 5u] == 0x08u && cfg[at + 6u] == 0x06u &&
                     cfg[at + 7u] == 0x50u;
            out->iface = cfg[at + 2u];
            out->ep_in = 0;
            out->ep_out = 0;
        } else if (type == USB_DESC_ENDPOINT && dlen >= 7u && in_msc &&
                   (cfg[at + 3u] & 0x03u) == 0x02u) {
            uint8_t ep = cfg[at + 2u];
            uint16_t mps = (uint16_t)(rd16(cfg + at + 4u) & 0x7FFu);

            if ((ep & 0x80u) != 0 && out->ep_in == 0) {
                out->ep_in = ep;
                out->mps_in = mps;
            } else if ((ep & 0x80u) == 0 && out->ep_out == 0) {
                out->ep_out = ep;
                out->mps_out = mps;
            }
        }
        at += dlen;
    }
    return (in_msc && out->ep_in != 0 && out->ep_out != 0) ? 0 : -1;
}

void usb_setup_set_protocol(uint8_t out[USB_SETUP_LEN], uint8_t iface,
                            uint8_t protocol)
{
    setup(out, 0x21u, 0x0Bu, protocol, iface, 0);
}

void usb_setup_set_idle(uint8_t out[USB_SETUP_LEN], uint8_t iface)
{
    setup(out, 0x21u, 0x0Au, 0, iface, 0);
}

void usb_setup_set_report_out(uint8_t out[USB_SETUP_LEN], uint8_t iface,
                              uint16_t len)
{
    setup(out, 0x21u, 0x09u, 0x0200u, iface, len);
}

int usb_find_kbd(const uint8_t *cfg, size_t len, usb_kbd_if *out)
{
    size_t at;
    int in_kbd = 0;

    if (cfg == (const uint8_t *)0 || out == (usb_kbd_if *)0 || len < 9u ||
        cfg[1] != USB_DESC_CONFIG)
        return -1;
    out->config = cfg[5];
    out->ep = 0;

    for (at = 0; at + 2u <= len;) {
        uint8_t dlen = cfg[at];
        uint8_t type = cfg[at + 1u];

        if (dlen < 2u || at + dlen > len)
            return -1;
        if (type == USB_DESC_INTERFACE && dlen >= 9u) {
            if (in_kbd && out->ep != 0)
                return 0;
            in_kbd = cfg[at + 5u] == 0x03u && cfg[at + 6u] == 0x01u &&
                     cfg[at + 7u] == 0x01u;
            out->iface = cfg[at + 2u];
        } else if (type == USB_DESC_ENDPOINT && dlen >= 7u && in_kbd &&
                   out->ep == 0 && (cfg[at + 3u] & 0x03u) == 0x03u &&
                   (cfg[at + 2u] & 0x80u) != 0) {
            out->ep = cfg[at + 2u];
            out->mps = (uint16_t)(rd16(cfg + at + 4u) & 0x7FFu);
        }
        at += dlen;
    }
    return (in_kbd && out->ep != 0) ? 0 : -1;
}

void usb_setup_hub_descriptor(uint8_t out[USB_SETUP_LEN], uint16_t len)
{
    setup(out, 0xA0u, 0x06u, (uint16_t)(USB_DESC_HUB << 8), 0, len);
}

void usb_setup_port_feature(uint8_t out[USB_SETUP_LEN], uint8_t port,
                            uint16_t feature, int set)
{
    setup(out, 0x23u, set ? 0x03u : 0x01u, feature, port, 0);
}

void usb_setup_port_status(uint8_t out[USB_SETUP_LEN], uint8_t port)
{
    setup(out, 0xA3u, 0x00u, 0, port, 4u);
}

int usb_parse_hub(const uint8_t *d, size_t len, usb_hub_desc *out)
{
    if (d == (const uint8_t *)0 || out == (usb_hub_desc *)0 || len < 7u ||
        d[0] < 7u || d[1] != USB_DESC_HUB)
        return -1;
    out->ports = d[2];
    out->power_ms = (uint16_t)(d[5] * 2u);
    return 0;
}

int usb_parse_port_status(const uint8_t *d, size_t len, uint16_t *status,
                          uint16_t *change)
{
    if (d == (const uint8_t *)0 || len < 4u)
        return -1;
    *status = rd16(d);
    *change = rd16(d + 2);
    return 0;
}

size_t bot_cbw(uint8_t out[BOT_CBW_LEN], uint32_t tag, uint32_t data_len,
               int in, uint8_t lun, const uint8_t *cdb, size_t cdb_len)
{
    size_t i;

    if (out == (uint8_t *)0 || cdb == (const uint8_t *)0 || cdb_len == 0 ||
        cdb_len > 16u || lun > 15u)
        return 0;
    for (i = 0; i < BOT_CBW_LEN; i++)
        out[i] = 0;
    wr32(out, BOT_CBW_SIG);
    wr32(out + 4, tag);
    wr32(out + 8, data_len);
    out[12] = in ? 0x80u : 0x00u;
    out[13] = lun;
    out[14] = (uint8_t)cdb_len;
    for (i = 0; i < cdb_len; i++)
        out[15u + i] = cdb[i];
    return BOT_CBW_LEN;
}

int bot_csw_check(const uint8_t *csw, size_t len, uint32_t tag,
                  uint32_t *residue)
{
    if (csw == (const uint8_t *)0 || len < BOT_CSW_LEN ||
        rd32(csw) != BOT_CSW_SIG || rd32(csw + 4) != tag || csw[12] > 2u)
        return -1;
    if (residue != (uint32_t *)0)
        *residue = rd32(csw + 8);
    return csw[12];
}
