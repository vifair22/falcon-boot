/* falcon-boot: the appliance's per-file routing decision. See route.h. */

#include <route.h>

appl_job appl_route(manifest_bless state, family_id fam)
{
    /* One family the appliance is owed nothing for: the loader patches
     * Windows media as it boots it (Section 6.4). Everything else is
     * blessed, BSD included, though what is signed there is the medium's own
     * loader rather than a wrap: a UKI wraps a Linux kernel and its
     * initramfs, and a FreeBSD medium has neither (M7 part 2). */
    if (fam == FAMILY_WINDOWS)
        return APPL_JOB_BOOTS_AS_IS;
    return (state == MANIFEST_BLESS_FRESH) ? APPL_JOB_DONE_BLESSED
                                           : APPL_JOB_SIGN;
}

appl_job appl_route_payload(appl_ours ours)
{
    return (ours == APPL_OURS_YES) ? APPL_JOB_DONE_BLESSED : APPL_JOB_SIGN;
}
