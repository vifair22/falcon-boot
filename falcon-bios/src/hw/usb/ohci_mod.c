/* falcon jump's OHCI.MOD (DESIGN Section 13.4): USB mass storage on the
 * machine's OHCI controllers. The storage half is msc.c, shared with the
 * other controllers' modules; this is only which controller.
 */

#include <hw/jmodhdr.h>
#include <hw/usb/msc.h>
#include <hw/usb/ohci.h>

static int ohci_mod_init(const jmod_services *svc, jmod_ops *ops)
{
    return msc_init(&ohci_hc, svc, ops);
}

JMOD_HEADER("OHCI", JMOD_SLOT_OHCI, ohci_mod_init);
