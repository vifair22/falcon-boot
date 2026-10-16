/* falcon-boot mkstick: what `mkstick update` does to a disk, decided from
 * what is on it (docs/DESIGN.md Section 12.14). The pure half, host-tested:
 * update.c reads the disk into an upd_facts and acts on the upd_plan.
 *
 * Five kinds of install, told apart by their records and a few files:
 *
 *   a stick        a `boot` record, a `library` and a `shelf` sharing its
 *                  install ID, and no STATION.INI. Its UEFI loader is
 *                  shim's next stage in p1's EFI/BOOT; its BIOS stages are
 *                  there only when `mkstick bios` put them on.
 *   a BIOS station a `boot` record whose partition holds STATION.INI;
 *                  station-legacy when *.MOD is there too.
 *   a UEFI station an ESP `boot` record with stage 3 at 0 and a loader in
 *                  EFI/falcon.
 *   both stations  on one disk: each half is updated.
 *
 * Anything else is refused with what was found, since writing stages onto
 * a disk that is not falcon's own is what `mkstick bios` is for.
 */

#ifndef FALCON_MKSTICK_UPDATECORE_H
#define FALCON_MKSTICK_UPDATECORE_H

#include <mkstick/stationcore.h>

typedef struct {
    /* A `boot` record with a stage 3 sector: a stick's p1 or a BIOS
     * station's partition. */
    int boot;
    /* An ESP whose `boot` record has stage 3 at 0: the UEFI station's. */
    int esp_boot;
    /* A `library` and a `shelf` carrying boot's install ID: a stick. */
    int library;
    int shelf;
    /* On boot's partition: STATION.INI, and station-legacy's drivers. */
    int station_ini;
    int mods;
    /* LBA 0 names a stage 2: falcon-bios is installed on this disk. */
    int bios_installed;
    /* p1 has shim and the loader shim names (a stick's UEFI half). */
    int stick_loader;
    /* The ESP has EFI/falcon and its loader (the UEFI station). */
    int station_loader;
    /* The UEFI station's STATION.INI has [totp]. */
    int totp;
    /* The UEFI station's EFI/falcon has shim; its ESP has
     * EFI/BOOT/BOOTX64.EFI, and that is falcon's jumper. */
    int station_shim;
    int jump_present;
    int jump_ours;
    /* Without shim, the loader is still grubx64.efi (STN_LOADER_OLD). */
    int station_old_name;
} upd_facts;

typedef enum {
    UPD_REFUSE = 0,
    UPD_STICK,
    UPD_STATION
} upd_kind;

typedef struct {
    upd_kind    kind;
    int         bios;           /* rewrite stages 1-3 */
    int         legacy;         /* station-legacy's stage 3, and its drivers */
    int         uefi_stick;     /* re-sign p1's loader */
    int         uefi_station;   /* re-sign EFI/falcon's loader */
    int         totp;           /* reseal after the new loader */
    /* With uefi_station: EFI/BOOT/BOOTX64.EFI, as install decides it
     * (stn_jump_plan). */
    stn_jump_action jump;
    /* A station without shim from before the jumper: the jumper written,
     * the entry made to name it, the loader renamed falconx64.efi when it
     * is grubx64.efi (rename). PCR 4 then measures one image more, so a
     * [totp] is not resealed but re-enrolled (totp_reenroll, totp 0). */
    int         migrate;
    int         rename;
    int         totp_reenroll;
    const char *why;            /* UPD_REFUSE: what was found instead */
} upd_plan;

/* Fills p from f. Always sets p->kind; p->why only when it refuses. */
void upd_plan_make(const upd_facts *f, upd_plan *p);

#endif /* FALCON_MKSTICK_UPDATECORE_H */
