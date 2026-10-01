/* falcon jump's USB modules: what a host controller driver offers the
 * device half (msc.c, hub.c, hid.c), so one msc.c serves UHCI, OHCI and
 * EHCI, each linked into its own module (DESIGN Section 13.4).
 */

#ifndef FALCON_BIOS_USBHC_H
#define FALCON_BIOS_USBHC_H

#include <stdint.h>

#define USBHC_OK     0
#define USBHC_ERR   (-1)
#define USBHC_STALL (-2)
#define USBHC_NAK   (-3)     /* an interrupt IN with nothing to report */

typedef struct {
    /* Finds and starts the controllers of this kind; how many root ports
     * they have between them, numbered across all of them, or 0. */
    unsigned (*start)(void);
    /* Resets root port `port` and enables it: 1 with *low set when a device
     * is there, 0 when none is, -1 on a port that will not enable. */
    int (*port_reset)(unsigned port, int *low);
    /* A control transfer to `addr`, on the controller root port `port`
     * belongs to (a device behind a hub goes by its hub's root port):
     * setup, `len` bytes of data in the direction the setup says, status. */
    int (*control)(unsigned port, uint8_t addr, uint8_t mps0, int low,
                   const uint8_t setup[8], void *data, uint16_t len);
    /* A bulk transfer on endpoint `ep` (0x80 set for IN): *toggle is the
     * endpoint's data toggle, carried across calls; *done the bytes moved,
     * which a short IN packet ends early. */
    int (*bulk)(unsigned port, uint8_t addr, uint8_t ep, uint16_t mps,
                void *data, uint32_t len, uint32_t *done, uint8_t *toggle);
    /* One interrupt IN packet of up to `len` bytes (at most `mps`) on
     * endpoint `ep`, with the low-speed marking when `low`: USBHC_NAK when
     * the device has had nothing to send for a few frames, after which no
     * transfer is left on the schedule. *toggle as for bulk. 0 on EHCI,
     * which would need split transactions for every keyboard there is. */
    int (*intr)(unsigned port, uint8_t addr, uint8_t ep, uint16_t mps,
                int low, void *data, uint32_t len, uint32_t *done,
                uint8_t *toggle);
    /* Gives root port `port` to its companion controller (EHCI's port
     * owner bit), where UHCI.MOD or OHCI.MOD finds the device after this
     * module; 0 on a controller that has no companion. */
    void (*release)(unsigned port);
} usbhc;

#endif /* FALCON_BIOS_USBHC_H */
