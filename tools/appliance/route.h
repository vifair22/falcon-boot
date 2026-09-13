/* falcon-boot: what the signing appliance owes one ISO it found on p2
 * (docs/DESIGN.md Sections 6.3.1 and 7.5).
 *
 * The appliance's library is not one queue. A Windows image is *prepared*,
 * which signs nothing and needs no key (Section 6.3); everything else is
 * *blessed*, which needs the MOK private key off p3 and the user's per-image
 * vouch. Deciding which of those an ISO wants is three inputs and no I/O, so
 * it lives here and is tested in test/appliance_route_test.c rather than
 * being read out of a booting machine.
 *
 * The split is the same one src/bless draws between bless.c and blesscore.c,
 * and the same one src/cli/sign draws between prepare.c and prepcore.c: the
 * judgement apart from the subprocesses that act on it.
 */

#ifndef FALCON_APPLIANCE_ROUTE_H
#define FALCON_APPLIANCE_ROUTE_H

#include <family/family.h>
#include <manifest/manifest.h>

typedef enum {
    APPL_JOB_PREPARE = 0,       /* Windows media with no usable record */
    APPL_JOB_SIGN,              /* everything else with no usable record */
    APPL_JOB_DONE_PREPARED,     /* prepared already, and still current */
    APPL_JOB_DONE_BLESSED,      /* blessed already, and still current */
} appl_job;

/* state and kind come from one manifest_lookup over the p2 manifest (kind is
 * read off the matched record, and is meaningless when state is NONE); fam
 * from src/family/family.c over the ISO itself.
 *
 * The rule, stated once so the two halves cannot drift:
 *
 *   - No record, or a record whose size no longer agrees, means do the work.
 *     Which work is the family's business and nothing else's.
 *   - A current record counts as done only when its kind is the kind this
 *     family would produce. A prepared record on Linux media, or a blessed
 *     record naming a UKI wrapped out of Windows media, describes an artifact
 *     that cannot boot what it claims to; the ISO is treated as unhandled and
 *     the run replaces the record. This is the appliance-side twin of
 *     prep_lookup's PREP_STATE_OTHER_KIND.
 *
 * Note what is deliberately absent: the checksum. Freshness is path plus size
 * here exactly as it is in the loader (DESIGN Section 8.1), so a stick that
 * has not changed costs a manifest read rather than a hash of every ISO on
 * it. The checksum still governs correctness downstream, because it is what
 * names the /falcon/prepared/<sha256>/ folder a prepared record points at,
 * and `falcon-boot-sign prepare` re-checks that folder before it agrees an
 * ISO needs nothing. */
appl_job appl_route(manifest_bless state, manifest_kind kind, family_id fam);

/* Does this family's media get prepared rather than blessed? One predicate,
 * so "Windows is the prepared one" is written down once. */
int appl_family_is_prepared(family_id fam);

#endif /* FALCON_APPLIANCE_ROUTE_H */
