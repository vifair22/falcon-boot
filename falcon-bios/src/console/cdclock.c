/* falcon-bios: the menu countdown's clock. See cdclock.h. */

#include <console/cdclock.h>

void cdclock_start(cdclock *c, uint32_t ticks, int sec)
{
    c->tick0 = ticks;
    c->sec_last = sec;
    c->rtc_gone = 0;
}

uint32_t cdclock_tenths(cdclock *c, uint32_t ticks, int sec)
{
    uint32_t gone;
    uint32_t by_ticks;
    uint32_t by_rtc;

    /* The count goes back to 0 at midnight; a count that is past a day's
     * is not one the BIOS wrote, and reads as nothing gone. */
    if (ticks >= c->tick0)
        gone = ticks - c->tick0;
    else if (c->tick0 < CDCLOCK_TICKS_PER_DAY)
        gone = ticks + CDCLOCK_TICKS_PER_DAY - c->tick0;
    else
        gone = 0;
    if (gone > CDCLOCK_TICKS_PER_DAY)
        gone = 0;
    /* 18.2 ticks a second: tenths are ticks x 100 / 182. */
    by_ticks = gone * 100u / 182u;

    if (sec != CDCLOCK_NO_RTC) {
        if (c->sec_last != CDCLOCK_NO_RTC && sec != c->sec_last)
            c->rtc_gone++;
        c->sec_last = sec;
    }
    by_rtc = c->rtc_gone * 10u;

    return by_ticks > by_rtc ? by_ticks : by_rtc;
}
