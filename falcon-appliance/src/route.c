/* falcon-boot: the appliance's per-file routing decision. See route.h. */

#include <route.h>

appl_job appl_route(manifest_bless state, family_id fam)
{
    /* Two families the appliance is owed nothing for, for two different
     * reasons.
     *
     * Windows, because the loader patches it as it boots it (Section 6.4).
     *
     * ESXi, because blessing does not apply: a UKI wraps a Linux kernel and
     * its initramfs and an ESXi medium has neither, which is why the sign
     * tool has no kernel path for it either. Queueing it would put a job in
     * front of the user that can only ever fail. What ESXi needs is Secure
     * Boot off, and that is not something the appliance can hand it.
     *
     * Everything else is blessed, BSD included, though what is signed there
     * is the medium's own loader rather than a wrap (M7 part 2). */
    if (fam == FAMILY_WINDOWS || fam == FAMILY_ESXI)
        return APPL_JOB_BOOTS_AS_IS;
    return (state == MANIFEST_BLESS_FRESH) ? APPL_JOB_DONE_BLESSED
                                           : APPL_JOB_SIGN;
}

appl_job appl_route_payload(appl_ours ours)
{
    return (ours == APPL_OURS_YES) ? APPL_JOB_DONE_BLESSED : APPL_JOB_SIGN;
}
