/* falcon-bios: the next-boot marker, taken off the boot disk (DESIGN Section
 * 6.3.1). See marker.c.
 */

#ifndef FALCON_BIOS_MARKER_H
#define FALCON_BIOS_MARKER_H

#include <stddef.h>

#include <start/bios.h>

typedef enum {
    MARKER_NONE = 0,  /* no marker on the disk */
    MARKER_VALUE,     /* a fresh marker; its value is in out */
    MARKER_BAD,       /* a marker that holds nothing usable */
    MARKER_STALE,     /* set longer ago than the window, or the clock went back */
    MARKER_NO_CLOCK,  /* the RTC did not answer, so it could not be dated */
    MARKER_STUCK      /* it could not be cleared, so it is not acted on */
} marker_state;

/* Reads the marker sector and, if it is one, CLEARS it, before anything else
 * is decided about it. Every state but NONE and STUCK means the sector is
 * zero now. Only MARKER_VALUE fills out (NUL-terminated); it is still only a
 * name, matched against the survey by the caller. */
marker_state marker_take(const bios_disk *d, char *out, size_t cap);

#endif /* FALCON_BIOS_MARKER_H */
