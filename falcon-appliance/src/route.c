/* falcon-boot: the appliance's per-file routing decision. See route.h. */

#include <route.h>

int appl_family_is_prepared(family_id fam)
{
    return fam == FAMILY_WINDOWS;
}

appl_job appl_route(manifest_bless state, manifest_kind kind, family_id fam)
{
    int prepared_family = appl_family_is_prepared(fam);

    if (state == MANIFEST_BLESS_FRESH) {
        if (prepared_family && kind == MANIFEST_KIND_PREPARED)
            return APPL_JOB_DONE_PREPARED;
        if (!prepared_family && kind == MANIFEST_KIND_BLESSED)
            return APPL_JOB_DONE_BLESSED;
        /* Current record, wrong kind for this medium: falls through to the
         * work the family actually needs, which rewrites the record. */
    }
    return prepared_family ? APPL_JOB_PREPARE : APPL_JOB_SIGN;
}

appl_job appl_route_payload(appl_ours ours)
{
    return (ours == APPL_OURS_YES) ? APPL_JOB_DONE_BLESSED : APPL_JOB_SIGN;
}
