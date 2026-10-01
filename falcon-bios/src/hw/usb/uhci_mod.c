/* falcon jump's UHCI.MOD (DESIGN Section 13.4): USB mass storage on the
 * first UHCI controller, the module the core reads when the PCI scan finds
 * one. The storage half is msc.c, shared with the other controllers'
 * modules; this is only which controller.
 */

#include <hw/jmodhdr.h>
#include <hw/usb/msc.h>
#include <hw/usb/uhci.h>

static int uhci_mod_init(const jmod_services *svc, jmod_ops *ops)
{
    return msc_init(&uhci_hc, svc, ops);
}

JMOD_HEADER("UHCI", JMOD_SLOT_UHCI, uhci_mod_init);
