/* falcon jump's USB modules: one level of hub. See hub.c. */

#ifndef FALCON_BIOS_HUB_H
#define FALCON_BIOS_HUB_H

#include <hw/usb/usbhc.h>

/* Called for each downstream port with a device on it, after the port's
 * reset: the device answers at address 0, `low` when it is low speed. */
typedef void (*hub_child_fn)(void *ctx, unsigned root, int low);

/* The hub at `addr` on root port `root`, configured: its ports powered and
 * each one with a device reset and handed to `child`, in port order. 0, or
 * -1 when the hub would not describe itself. */
int hub_walk(const usbhc *hc, unsigned root, uint8_t addr, uint8_t mps0,
             hub_child_fn child, void *ctx);

#endif /* FALCON_BIOS_HUB_H */
