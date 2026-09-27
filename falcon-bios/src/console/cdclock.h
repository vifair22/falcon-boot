/* falcon-bios: the menu countdown's clock (DESIGN Section 13.7), apart from
 * the BIOS calls that read it.
 *
 * Two clocks, and the countdown takes whichever says more time has gone.
 * The BIOS's tick count at 0040:006C moves only when IRQ0 is delivered, and
 * a machine can arrive at the menu where it never is: after Windows 2000
 * reset the rig's machine, SeaBIOS answered every INT 16h with interrupts
 * off, IRQ0 sat pending in the PIC, and a five-second countdown stood at
 * five for good. The RTC's seconds (INT 1Ah AH=02h, which reads CMOS and
 * takes no interrupt) move regardless; a machine whose RTC does not run
 * still has its ticks. Only a machine with neither waits, which is what a
 * menu with no countdown does anyway.
 *
 * The RTC is read as its seconds digit changing, not as a time: a change
 * is a second gone, whatever the encoding, and a midnight or an hour
 * rolling over is one change like any other. The first change can come
 * anywhere up to a second after the start, so the RTC alone ends a count
 * up to a second early; the ticks, where they move, are exact.
 */

#ifndef FALCON_BIOS_CDCLOCK_H
#define FALCON_BIOS_CDCLOCK_H

#include <stdint.h>

/* The tick count's value at midnight, where it goes back to 0. */
#define CDCLOCK_TICKS_PER_DAY 0x1800B0u

/* No RTC reading: the call failed. */
#define CDCLOCK_NO_RTC (-1)

typedef struct {
    uint32_t tick0;       /* the tick count at the start */
    int      sec_last;    /* the RTC's seconds when last read, or NO_RTC */
    uint32_t rtc_gone;    /* changes of it seen since the start */
} cdclock;

/* The count starts now: `ticks` is the BDA's count, `sec` the RTC's
 * seconds byte or CDCLOCK_NO_RTC. */
void cdclock_start(cdclock *c, uint32_t ticks, int sec);

/* Tenths of a second gone since cdclock_start, by whichever clock says
 * more, given the readings now. Call it as often as the loop turns; every
 * change of `sec` between two calls counts one second. */
uint32_t cdclock_tenths(cdclock *c, uint32_t ticks, int sec);

#endif /* FALCON_BIOS_CDCLOCK_H */
