/* falcon jump's USB modules: what a host controller driver offers the
 * mass-storage half (msc.c), so one msc.c serves UHCI, OHCI and EHCI, each
 * linked into its own module (DESIGN Section 14.4).
 */

#ifndef FALCON_BIOS_USBHC_H
#define FALCON_BIOS_USBHC_H

#include <stdint.h>

#define USBHC_OK     0
#define USBHC_ERR   (-1)
#define USBHC_STALL (-2)

typedef struct {
    /* Finds and starts the controller; how many root ports it has, or 0. */
    unsigned (*start)(void);
    /* Resets root port `port` and enables it: 1 with *low set when a device
     * is there, 0 when none is, -1 on a port that will not enable. */
    int (*port_reset)(unsigned port, int *low);
    /* A control transfer to `addr`: setup, `len` bytes of data in the
     * direction the setup says, status. */
    int (*control)(uint8_t addr, uint8_t mps0, int low,
                   const uint8_t setup[8], void *data, uint16_t len);
    /* A bulk transfer on endpoint `ep` (0x80 set for IN): *toggle is the
     * endpoint's data toggle, carried across calls; *done the bytes moved,
     * which a short IN packet ends early. */
    int (*bulk)(uint8_t addr, uint8_t ep, uint16_t mps, void *data,
                uint32_t len, uint32_t *done, uint8_t *toggle);
} usbhc;

#endif /* FALCON_BIOS_USBHC_H */
