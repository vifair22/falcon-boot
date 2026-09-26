/* falcon jump's USB modules: mass storage over a usbhc. See msc.c. */

#ifndef FALCON_BIOS_MSC_H
#define FALCON_BIOS_MSC_H

#include <jump/jmod.h>
#include <hw/usb/usbhc.h>

/* A USB module's init: fills `ops` with mass storage over `hc`. */
int msc_init(const usbhc *hc, const jmod_services *svc, jmod_ops *ops);

#endif /* FALCON_BIOS_MSC_H */
