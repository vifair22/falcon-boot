/* falcon-boot: what the signing appliance owes one file it found on p2
 * (docs/DESIGN.md Sections 7.2 and 7.5).
 *
 * The appliance blesses, and that is all it does: an image it owes work to is
 * signed with the MOK private key off p3, after the user's per-image vouch.
 * Deciding whether an ISO is owed that is two inputs and no I/O, so it lives
 * here and is tested in falcon-appliance/tests/route_test.c rather than being
 * read out of a booting machine.
 *
 * Windows install media is the one family owed nothing. The loader patches
 * its boot.wim in memory as it boots it (DESIGN Section 6.4) and its boot
 * chain is Microsoft-signed throughout, so there is no kernel to wrap and no
 * artifact for the appliance to leave on p2. The appliance used to PREPARE
 * such media (a patched boot.wim staged beside the ISO); that half is gone,
 * and a record an older appliance left for it is not counted as a bless.
 *
 * A bare EFI payload (.efi, .img) is the other thing the survey meets, and
 * what "already done" means for one is a different question with a different
 * answer. That is appl_route_payload below.
 *
 * The split is the same one falcon-efi/src/bless draws between bless.c and
 * blesscore.c: the judgement apart from the subprocesses that act on it.
 */

#ifndef FALCON_APPLIANCE_ROUTE_H
#define FALCON_APPLIANCE_ROUTE_H

#include <sign/payload.h>
#include <media/family.h>
#include <manifest/manifest.h>

typedef enum {
    APPL_JOB_SIGN = 0,          /* no usable record: bless it */
    APPL_JOB_DONE_BLESSED,      /* blessed already, and still current */
    APPL_JOB_BOOTS_AS_IS,       /* Windows media: nothing to bless */
} appl_job;

/* state comes from one manifest_lookup over the p2 manifest; fam from
 * falcon-core/src/media/family.c over the ISO itself.
 *
 * The rule, stated once:
 *
 *   - Windows media is owed nothing, whatever the manifest says about it.
 *   - Otherwise no record, or a record whose size no longer agrees, means
 *     sign it; a current record means it is done.
 *
 * Note what is deliberately absent: the checksum. Freshness is path plus size
 * here exactly as it is in the loader (DESIGN Section 8.1), so a stick that
 * has not changed costs a manifest read rather than a hash of every ISO on
 * it. */
appl_job appl_route(manifest_bless state, family_id fam);

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
 * A payload has no manifest record, because the file IS the artifact
 * (falcon-cli/src/sign/payload.h says why writing one would be a second,
 * editable source of truth about it). "Already done" is therefore not
 * freshness. The only sense in which a payload is blessed is that our exact
 * certificate is already in its certificate table, which is also the tool's
 * own idempotence rule (sgn_payload_signed_with), so the queue and the tool
 * cannot disagree about a file.
 *
 * UNKNOWN answers SIGN. Offering a payload that turns out to carry our
 * certificate costs one line in the listing and a tool run that signs
 * nothing; hiding one that does not carry it costs a boot. */
appl_job appl_route_payload(appl_ours ours);

#endif /* FALCON_APPLIANCE_ROUTE_H */
