/* falcon jump's USB modules: storage and keyboards over a usbhc. See msc.c. */

#ifndef FALCON_BIOS_MSC_H
#define FALCON_BIOS_MSC_H

#include <jump/jmod.h>
#include <hw/usb/usbhc.h>

/* A USB module's init: fills `ops` for the sticks and keyboards on `hc`'s
 * ports. */
int msc_init(const usbhc *hc, const jmod_services *svc, jmod_ops *ops);

#endif /* FALCON_BIOS_MSC_H */
