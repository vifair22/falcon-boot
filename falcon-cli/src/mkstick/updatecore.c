/* What `mkstick update` does to a disk. updatecore.h has the kinds. */

#include <mkstick/updatecore.h>

#include <string.h>

void upd_plan_make(const upd_facts *f, upd_plan *p)
{
    memset(p, 0, sizeof *p);
    p->kind = UPD_REFUSE;

    if (f->boot < 0 && f->esp_boot < 0) {
        p->why = "no falcon boot record on this disk";
        return;
    }

    /* A stick carries its library and shelf beside its boot partition; a
     * station never does, and a stick never has STATION.INI. */
    if (f->boot >= 0 && !f->station_ini && f->library >= 0 &&
        f->shelf >= 0) {
        p->kind = UPD_STICK;
        p->bios = f->bios_installed;
        p->uefi_stick = f->stick_loader;
        if (!p->bios && !p->uefi_stick) {
            p->kind = UPD_REFUSE;
            p->why = "a stick with neither a UEFI loader on p1 nor the BIOS "
                     "stages";
        }
        return;
    }

    if (f->boot >= 0 && !f->station_ini && f->esp_boot < 0) {
        p->why = "a boot record with no STATION.INI and no library or shelf "
                 "beside it: neither a stick nor a station";
        return;
    }

    p->kind = UPD_STATION;
    if (f->boot >= 0 && f->station_ini) {
        p->bios = f->bios_installed;
        p->legacy = f->mods;
    }
    if (f->esp_boot >= 0 && f->station_loader) {
        p->uefi_station = 1;
        p->totp = f->totp;
        p->jump = stn_jump_plan(f->station_shim, f->jump_present,
                                f->jump_ours);
        p->migrate = !f->station_shim && !f->jump_ours;
        p->rename = !f->station_shim && f->station_old_name;
        if (p->migrate && p->totp) {
            p->totp = 0;
            p->totp_reenroll = 1;
        }
    }
    if (!p->bios && !p->uefi_station) {
        p->kind = UPD_REFUSE;
        p->why = "a station with neither the BIOS stages in LBA 0 nor a "
                 "loader in EFI/falcon";
    }
}
