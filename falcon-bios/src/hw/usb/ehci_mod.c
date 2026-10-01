/* falcon jump's EHCI.MOD (DESIGN Section 13.4): USB mass storage on the
 * machine's EHCI controllers, high-speed devices only; a full- or low-speed
 * one is handed to the port's companion, for UHCI.MOD or OHCI.MOD. The
 * storage half is msc.c, shared with the other controllers' modules.
 */

#include <hw/jmodhdr.h>
#include <hw/usb/msc.h>
#include <hw/usb/ehci.h>

static int ehci_mod_init(const jmod_services *svc, jmod_ops *ops)
{
    return msc_init(&ehci_hc, svc, ops);
}

JMOD_HEADER("EHCI", JMOD_SLOT_EHCI, ehci_mod_init);
