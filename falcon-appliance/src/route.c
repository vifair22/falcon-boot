/* falcon-boot: the appliance's per-file routing decision. See route.h. */

#include <route.h>

appl_job appl_route(manifest_bless state, family_id fam)
{
    /* Two families the appliance is owed nothing for. Windows the loader
     * patches as it boots it (Section 6.4). BSD boots by the memory-disk
     * route of the same section, which needs no signature while nothing is
     * enforcing, and there is no wrap to sign for when something is: a UKI
     * wraps a Linux kernel and its initramfs, and a FreeBSD medium has
     * neither. Blessing for BSD is roadmap M7 part 2, and this row is what
     * changes when it lands. */
    if (fam == FAMILY_WINDOWS || fam == FAMILY_BSD)
        return APPL_JOB_BOOTS_AS_IS;
    return (state == MANIFEST_BLESS_FRESH) ? APPL_JOB_DONE_BLESSED
                                           : APPL_JOB_SIGN;
}

appl_job appl_route_payload(appl_ours ours)
{
    return (ours == APPL_OURS_YES) ? APPL_JOB_DONE_BLESSED : APPL_JOB_SIGN;
}
