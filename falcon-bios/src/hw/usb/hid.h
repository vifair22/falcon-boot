/* falcon jump's USB modules: boot-protocol keyboards. See hid.c. */

#ifndef FALCON_BIOS_HID_H
#define FALCON_BIOS_HID_H

#include <hw/usb/usbhc.h>
#include <hw/usb/usbcore.h>

/* The keyboard at `addr`, configured, taken into the boot protocol and kept
 * for hid_poll. 0, or -1 when there is no room for another. */
int hid_attach(const usbhc *hc, unsigned root, uint8_t addr, uint8_t mps0,
               int low, const usb_kbd_if *k);

/* How many keyboards hid_attach kept. */
unsigned hid_count(void);

/* Each keyboard's report, if it has one, as keys in the BIOS's ring and
 * the held keys in its shift flags; and the held key's repeat. */
void hid_poll(void);

#endif /* FALCON_BIOS_HID_H */
