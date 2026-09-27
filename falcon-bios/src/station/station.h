/* falcon-bios: the BIOS station (docs/DESIGN.md Section 13), as a module
 * the station apps run. See station.c.
 *
 * apps/station/ runs it as it is; apps/station-legacy/ adds jump's routes
 * and DevLink recovery (Section 13.10) through the two calls below. What an
 * app does not link it cannot run: the hooks are how station-legacy's rows
 * reach the list without the plain station linking a byte of them.
 */

#ifndef FALCON_BIOS_STATION_H
#define FALCON_BIOS_STATION_H

#include <stdint.h>

#include <result.h>
#include <fs/fs.h>
#include <media/strows.h>

typedef struct {
    const char     *name;      /* for serial: "station" */
    const uint16_t *title;     /* the banner: u"falcon-boot " and a version */
    /* The app's own rows, added after the ones the station finds (13.3)
     * and before memtest and STATION.INI, which then applies to them as to
     * any found row. `vol` is the station partition, mounted, or NULL. May
     * be NULL. */
    void (*rows)(strows *rows, fs_vol *vol);
    /* Boots one of those rows (STROW_ROUTE). Nonzero when a screen is
     * waiting for its key. May be NULL. */
    int  (*boot)(const strow *w);
} station_app;

/* The station: the disks, the station partition, the rows, the notices,
 * next boot and the menu. Does not return. */
void station_main(uint8_t drive, const station_app *app);

/* From an app's rows hook: the station partition's /uki as a plaintext
 * shelf of its own, every UKI in it a row labelled by its .osrel name.
 * The number of rows added. */
int station_add_own_shelf(void);

/* The page a route leaves when it comes back: the label, "<route> failed
 * while <stage>", the advice and the key line, as every station route
 * draws it (docs/notes/station-rework.md section 11). */
void station_failed(const uint16_t *label, const char *route,
                    const boot_result *r);

#endif /* FALCON_BIOS_STATION_H */
