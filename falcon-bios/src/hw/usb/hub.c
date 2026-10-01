/* falcon jump's USB modules: a hub's ports (DESIGN Section 13.4).
 *
 * A keyboard is as often behind a hub as on a root port, and a keyboard
 * often holds its own: a two- or three-port hub with the keyboard on one of
 * its ports, itself perhaps plugged into a desk hub. msc.c walks two levels
 * of them, calling this for each hub. A hub runs at full speed on UHCI or
 * OHCI (EHCI hands it to its companion, ehci.c), so a device behind it is
 * reached as one on a root port is, through the root port the hubs hang
 * from, with only the low-speed marking on a low-speed device's transfers.
 * No transaction translator, no split transactions, and no hub's interrupt
 * endpoint: the ports are read once, by GET_STATUS, when the module
 * probes.
 */

#include <hw/usb/hub.h>
#include <hw/usb/usbcore.h>
#include <hw/modrt.h>

/* Ports walked: a hub can declare 255, and every one costs a request. */
#define HUB_PORTS_MAX 15u
/* A connected device's debounce after power reaches its port. */
#define CONNECT_MS    100u

static uint8_t g_buf[8];

static int port_feature(const usbhc *hc, unsigned root, uint8_t addr,
                        uint8_t mps0, uint8_t port, uint16_t feature, int set)
{
    uint8_t setup[USB_SETUP_LEN];

    usb_setup_port_feature(setup, port, feature, set);
    return hc->control(root, addr, mps0, 0, setup, (void *)0, 0);
}

static int port_status(const usbhc *hc, unsigned root, uint8_t addr,
                       uint8_t mps0, uint8_t port, uint16_t *st,
                       uint16_t *ch)
{
    uint8_t setup[USB_SETUP_LEN];

    usb_setup_port_status(setup, port);
    if (hc->control(root, addr, mps0, 0, setup, g_buf, 4u) != USBHC_OK)
        return -1;
    return usb_parse_port_status(g_buf, 4u, st, ch);
}

int hub_walk(const usbhc *hc, unsigned root, uint8_t addr, uint8_t mps0,
             hub_child_fn child, void *ctx)
{
    uint8_t setup[USB_SETUP_LEN];
    usb_hub_desc h;
    unsigned n;
    unsigned p;

    usb_setup_hub_descriptor(setup, (uint16_t)sizeof g_buf);
    if (hc->control(root, addr, mps0, 0, setup, g_buf,
                    (uint16_t)sizeof g_buf) != USBHC_OK ||
        usb_parse_hub(g_buf, sizeof g_buf, &h) != 0)
        return -1;
    n = h.ports > HUB_PORTS_MAX ? HUB_PORTS_MAX : h.ports;

    for (p = 1; p <= n; p++)
        (void)port_feature(hc, root, addr, mps0, (uint8_t)p, HUB_PORT_POWER,
                           1);
    io_delay_us(((uint32_t)h.power_ms + CONNECT_MS) * 1000u);

    for (p = 1; p <= n; p++) {
        uint16_t st = 0;
        uint16_t ch = 0;
        unsigned i;

        if (port_status(hc, root, addr, mps0, (uint8_t)p, &st, &ch) != 0 ||
            (st & HUB_PS_CONNECTION) == 0)
            continue;
        if (port_feature(hc, root, addr, mps0, (uint8_t)p, HUB_PORT_RESET,
                         1) != USBHC_OK)
            continue;
        /* The hub drives the reset for 10-20 ms and says when it is over. */
        for (i = 0; i < 50u; i++) {
            io_delay_us(10000);
            if (port_status(hc, root, addr, mps0, (uint8_t)p, &st, &ch) != 0)
                break;
            if ((ch & HUB_PC_RESET) != 0 || (st & HUB_PS_RESET) == 0)
                break;
        }
        (void)port_feature(hc, root, addr, mps0, (uint8_t)p,
                           HUB_C_PORT_RESET, 0);
        (void)port_feature(hc, root, addr, mps0, (uint8_t)p,
                           HUB_C_PORT_CONNECTION, 0);
        if ((st & HUB_PS_ENABLE) == 0)
            continue;
        io_delay_us(10000);          /* reset recovery */
        child(ctx, root, (st & HUB_PS_LOW) != 0);
    }
    return 0;
}
