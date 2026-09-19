/* falcon-boot: the appliance's per-file routing decision. See route.h. */

#include <route.h>

appl_job appl_route(manifest_bless state, family_id fam)
{
    if (fam == FAMILY_WINDOWS)
        return APPL_JOB_BOOTS_AS_IS;
    return (state == MANIFEST_BLESS_FRESH) ? APPL_JOB_DONE_BLESSED
                                           : APPL_JOB_SIGN;
}

appl_job appl_route_payload(appl_ours ours)
{
    return (ours == APPL_OURS_YES) ? APPL_JOB_DONE_BLESSED : APPL_JOB_SIGN;
}
