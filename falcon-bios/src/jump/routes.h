/* falcon-bios: jump's routes through its drivers (DESIGN Sections 14.6 and
 * 13.10), for every app that offers them: falcon jump, off its floppy, and
 * station-legacy, off its station partition.
 *
 * CDR (the disc in an IDE CD drive, through ATA.MOD), USB (a falcon stick on
 * a USB root port, through EHCI.MOD, UHCI.MOD or OHCI.MOD) and PCC (a
 * CompactFlash card in a PC Card slot, through PCC.MOD). Each app keeps its
 * own menu and pages; this is what happens between "booting:" and the page
 * that says why it came back: the driver read off the app's own FAT volume,
 * the device found, and the medium handed over with the driver resident
 * behind the reverse thunk.
 *
 * A route is offered, as jump always offered it, when its driver is on the
 * volume and its controller is on the PCI bus (DESIGN Section 14.2): a PCI
 * class scan and a directory lookup, nothing loaded to find out.
 */

#ifndef FALCON_BIOS_ROUTES_H
#define FALCON_BIOS_ROUTES_H

#include <result.h>
#include <fs/fs.h>

typedef enum {
    JROUTE_CD = 0,     /* CDR: ATA.MOD, an IDE controller */
    JROUTE_USB,        /* USB: a host controller module and its controller */
    JROUTE_CF,         /* PCC: PCC.MOD, a CardBus bridge */
    JROUTE_COUNT
} jroute;

/* Which routes `vol` can offer on this machine: the modules at its root,
 * the controllers on the bus, said on serial. `medium` names the volume in
 * the sentences a failed load gives ("floppy", "disk"). Call once, before
 * anything else here. */
void jroute_init(fs_vol *vol, const char *medium);

/* Whether the route has its driver and its controller. */
int jroute_offered(jroute k);

/* The route's code for the screen's corner ("CDR", "USB", "PCC"). */
const char *jroute_code(jroute k);

/* Boots the route, writing its own lines into the page the caller started.
 * Returns only when it could not hand over, with everything it installed
 * taken back and r saying why. */
void jroute_boot(jroute k, boot_result *r);

#endif /* FALCON_BIOS_ROUTES_H */
