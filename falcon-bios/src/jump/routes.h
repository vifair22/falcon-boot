/* falcon-bios: jump's routes through its drivers (DESIGN Sections 13.6 and
 * 12.10), for every app that offers them: falcon jump, off its floppy, and
 * station-legacy, off its station partition.
 *
 * CDR (the disc in an IDE CD drive, through ATA.MOD), USB (a falcon stick on
 * a USB port, through EHCI.MOD, UHCI.MOD or OHCI.MOD) and PCC (a
 * CompactFlash card in a PC Card slot, through PCC.MOD). Each app keeps its
 * own menu and pages; this is what happens between "booting:" and the page
 * that says why it came back: the driver read off the app's own FAT volume,
 * the device found, and the medium handed over with the driver resident
 * behind the reverse thunk.
 *
 * A route is offered, as jump always offered it, when its driver is on the
 * volume and its controller is on the PCI bus (DESIGN Section 13.2): a PCI
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
 * taken back but the keyboards a USB route keeps up (below), and r saying
 * why. */
void jroute_boot(jroute k, boot_result *r);

/* ---- the keyboards a USB route leaves up (DESIGN Section 13.6) ----
 *
 * A USB route that found keyboards keeps them when it comes back: the
 * modules stay loaded, stage 3 polls them while it waits for a key
 * (bios_key_poll, in start/bios.h), and a later handover decides what
 * becomes of them. */

/* For a handover to something that reads the keyboard through the BIOS
 * with no module of its own resident (a disk's boot code, a floppy
 * emulation disc): the keyboard block behind INT 16h, the module region
 * carved out of the map. 0 when it is installed, 1 when no keyboards are
 * up, -1 when the machine would not give up the memory (said on serial;
 * the handover goes on without them). */
int jroute_kbd_install(void);

/* Takes back what jroute_kbd_install installed, for a handover that came
 * back. The keyboards stay up. */
void jroute_kbd_remove(void);

/* Lets the keyboards go before a handover that has no use for them
 * (memtest86+, which drives the keyboard itself and tests the memory the
 * modules sit in; INT 18h; a system on another disk), said on serial. */
void jroute_kbd_release(void);

#endif /* FALCON_BIOS_ROUTES_H */
