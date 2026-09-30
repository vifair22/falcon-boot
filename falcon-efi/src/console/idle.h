/* falcon-boot: the idle power-off (docs/DESIGN.md Section 7.9).
 *
 * Purpose. A loader left at a menu or the passphrase prompt would otherwise
 * show the same screen until someone came, and a panel shown one screen
 * for days keeps it. So every wait for a key also waits on this module's
 * timer, and when a page has stood for IDLE_SECONDS with no key the
 * machine is powered off (EfiResetShutdown), as Windows does at its
 * BitLocker prompt. Nothing is booted and nothing is decided: an idle
 * machine goes off rather than on.
 *
 * What counts as activity: a page starting to wait (menu_run and its
 * kin, the passphrase prompt, a "press any key" page), each key read, and
 * each second of a countdown, which ends in a boot of its own and must not
 * be cut short by a longer idle time. The status lines' refresh (the TOTP
 * code) is not: a changing code on an unwatched screen is still unwatched.
 * A page starting to wait counts because the timer outlives what the
 * loader launches: memtest run for an hour and quit would otherwise come
 * back to a timer long fired.
 *
 * Before the power-off the hook runs (the station's is totp_wipe), and the
 * prompt wipes what has been typed. Should the firmware return from
 * ResetSystem, the wait goes on with the timer started again.
 *
 * Interactions. console/menu.c and console/prompt.c wait on idle_event()
 * beside the keyboard; the apps' wait_key is idle_wait_key; the station
 * takes `poweroff` from STATION.INI (media/stationini.h).
 */

#ifndef FALCON_EFI_IDLE_H
#define FALCON_EFI_IDLE_H

#include <efi/uefi.h>

/* Starts the timer, or restarts it with a new time; 0 turns it off. */
void idle_start(EFI_SYSTEM_TABLE *st, UINT32 seconds);

/* Runs before the power-off. */
void idle_set_hook(void (*fn)(void));

/* A page, a key or a countdown's second: the idle time starts again. */
void idle_touch(void);

/* The timer's event, to wait on beside ConIn->WaitForKey; NULL when off. */
EFI_EVENT idle_event(void);

/* The idle time is up: the hook, then the power-off. Returns only when
 * the firmware did not power off, with the timer started again. */
void idle_expired(void);

/* A page's wait for one key: WaitForKey or the timer, the power-off on
 * the timer. Starts the idle time itself. */
EFI_STATUS idle_wait_key(EFI_SYSTEM_TABLE *st, EFI_INPUT_KEY *key);

#endif /* FALCON_EFI_IDLE_H */
