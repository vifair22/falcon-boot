/* falcon-boot: what the signing appliance owes one file it found on p2
 * (docs/DESIGN.md Sections 6.3.1, 7.2 and 7.5).
 *
 * The appliance's library is not one queue. A Windows image is *prepared*,
 * which signs nothing and needs no key (Section 6.3); everything else is
 * *blessed*, which needs the MOK private key off p3 and the user's per-image
 * vouch. Deciding which of those an ISO wants is three inputs and no I/O, so
 * it lives here and is tested in test/appliance_route_test.c rather than
 * being read out of a booting machine.
 *
 * A bare EFI payload (.efi, .img) is a third thing the survey meets and only
 * a second thing it can do: payloads are blessed and never prepared, and what
 * "already done" means for one is a different question with a different
 * answer. That is appl_route_payload below.
 *
 * The split is the same one src/bless draws between bless.c and blesscore.c,
 * and the same one src/cli/sign draws between prepare.c and prepcore.c: the
 * judgement apart from the subprocesses that act on it.
 */

#ifndef FALCON_APPLIANCE_ROUTE_H
#define FALCON_APPLIANCE_ROUTE_H

#include <cli/sign/payload.h>
#include <iso/family.h>
#include <bless/manifest.h>

typedef enum {
    APPL_JOB_PREPARE = 0,       /* Windows media with no usable record */
    APPL_JOB_SIGN,              /* everything else with no usable record */
    APPL_JOB_DONE_PREPARED,     /* prepared already, and still current */
    APPL_JOB_DONE_BLESSED,      /* blessed already, and still current */
} appl_job;

/* state and kind come from one manifest_lookup over the p2 manifest (kind is
 * read off the matched record, and is meaningless when state is NONE); fam
 * from src/iso/family.c over the ISO itself.
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

/* Whether a bare payload already carries the certificate this stick signs
 * with. UNKNOWN is not a failure: that certificate lives on p3, and the
 * survey walks p2 while p3 is still locked, so the question has an answer
 * only once the bless half has opened it (and, for a payload inside a .img,
 * only from inside the image, where the tool answers it per volume). */
typedef enum {
    APPL_OURS_UNKNOWN = -1,
    APPL_OURS_NO      = 0,
    APPL_OURS_YES     = 1
} appl_ours;

/* What the appliance owes one bare EFI payload (DESIGN Section 7.2).
 *
 * Two things separate this from appl_route above, and both are the same fact
 * seen twice: a payload has no manifest record, because the file IS the
 * artifact (src/cli/sign/payload.h says why writing one would be a second,
 * editable source of truth about it).
 *
 *   - Nothing prepares a payload. Preparation rewrites boot.wim inside
 *     Windows install media; there is no such thing inside a PE, and the
 *     family that would have asked for it is refused by the tool with a
 *     message rather than routed here.
 *   - "Already done" is therefore not freshness. The only sense in which a
 *     payload is blessed is that our exact certificate is already in its
 *     certificate table, which is also the tool's own idempotence rule
 *     (sgn_payload_signed_with), so the queue and the tool cannot disagree
 *     about a file.
 *
 * UNKNOWN answers SIGN. Offering a payload that turns out to carry our
 * certificate costs one line in the listing and a tool run that signs
 * nothing; hiding one that does not carry it costs a boot. */
appl_job appl_route_payload(appl_ours ours);

#endif /* FALCON_APPLIANCE_ROUTE_H */
