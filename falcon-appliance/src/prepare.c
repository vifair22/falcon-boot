/* falcon-boot appliance: the preparation half. */

#include "appliance.h"

#include <sign/sign.h>

#include <limits.h>
#include <stdio.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* ---- the preparation half (DESIGN Sections 6.3, 6.3.1 and 7.7) ----
 *
 * The batch is the unit of WORK and the gate is the unit of CONSENT, and both
 * of those are in the design at once. Section 6.3.1 wants the appliance to
 * prepare every unprepared Windows ISO in one boot, so dropping three images
 * costs one reboot rather than three. Section 7.7 wants patching to pass
 * through the same review-and-yes as signing, because it is the same act of
 * authorising a modification to media the user did not produce: "what the
 * review shows differs; that it is shown, and that the user says yes to it,
 * does not."
 *
 * So the whole batch is released by ONE gate, and the gate is the bless one:
 * nothing pre-marked, the user marks images, every path on screen at the
 * moment of confirmation, a typed "yes" over the review.
 *
 * Regression it fixes, recorded because the reasoning that lost it was
 * plausible: the earlier version treated "preparation vouches for nothing, it
 * holds no key and the platform verifies nothing it touches" as a reason to
 * skip the gate, and read the loader's prepare-offer as the confirmation. On
 * real hardware 2026-09-13 that meant an appliance that booted and immediately
 * began hashing two multi-gigabyte Windows ISOs, having asked nothing and said
 * nothing about the cost. Needing no key is not the same thing as needing no
 * consent.
 */

/* Room left on p2, in bytes; 0 when it cannot be asked. Used only to make a
 * failure legible, never to decide anything: a threshold here would be a
 * guess about an image whose size is not known until wimlib has rewritten
 * it, and prepare's own ENOSPC is the honest answer. */
static uint64_t p2_free_bytes(void)
{
    struct statvfs s;

    if (statvfs(P2_MNT, &s) != 0)
        return 0;
    return (uint64_t)s.f_bavail * (uint64_t)s.f_frsize;
}

/* Below this, a full stick is the likeliest explanation of a failure and is
 * worth naming: DESIGN Section 6.3 measures preparation at about 1.2 GB of
 * scratch plus the roughly 570 MB patched image it leaves behind. */
#define PREP_TIGHT_BYTES ((uint64_t)2u << 30)

/* Whether this image can prepare at all. A build that dropped one of these
 * would otherwise fail per ISO with a message about the ISO; missing 7z is
 * worse still, because Windows media is UDF-only and without a UDF reader it
 * classifies as an unknown Linux image and lands in the wrong queue.
 *
 * PID 1 in an initramfs inherits no environment, so these lookups run against
 * sgn_have_program's own "/usr/bin:/bin" fallback rather than against a PATH.
 * That is where mk-appliance.sh stages both programs, and it is the head of
 * the PATH every child gets (g_child_env), so the two agree; if the staging
 * layout ever moves, this and that list move together. */
int prep_tools_present(void)
{
    int ok = 1;

    if (!sgn_have_program(WIM_TOOL)) {
        say("WARNING: %s is not in this appliance image, so no Windows ISO\n"
            "         can be prepared on this stick.\n", WIM_TOOL);
        ok = 0;
    }
    if (!iso_udf_reader_ok(NULL, 0)) {
        say("WARNING: %s cannot read UDF in this appliance image. Windows\n"
            "         install media keeps its files in the UDF namespace\n"
            "         only, so such an ISO reads as empty and is listed\n"
            "         below as unsigned Linux media rather than prepared.\n",
            Z_TOOL);
        ok = 0;
    }
    if (access(WINJUMP_PATH, R_OK) != 0) {
        say("WARNING: %s is missing, so preparation has nothing to put in\n"
            "         place of WinPE's shell.\n", WINJUMP_PATH);
        ok = 0;
    }
    if (!ok)
        say("         Rebuild the appliance with falcon-appliance/"
            "mk-appliance.sh.\n\n");
    return ok;
}

/* How often the wait for one prepare says it is still going. The hash of a
 * multi-gigabyte ISO is the longest stretch the tool itself has nothing to
 * print during, and a console that has been silent for four minutes is what
 * a hang looks like. Half a minute is often enough to read as alive and rare
 * enough not to bury the tool's own lines. */
#define PREP_BEAT_S 30u

/* Prepares the marked Windows images, in path order, reporting each. One
 * image failing is that image's problem and the batch carries on: the reboot
 * this run ends in is the expensive part, and abandoning the rest of the
 * queue would charge the user another one per bad ISO.
 *
 * Runs only over what the gate confirmed; an unmarked image is a skip, and is
 * counted as one so the summary accounts for every image the survey found. */
static void prepare_batch(size_t *prepared_n, size_t *failed_n,
                          size_t *skipped_n)
{
    size_t sel_n = selected_count(g_prep, g_prep_n);
    size_t done_n = 0;
    size_t i;

    say("\nFALCON-APPLIANCE-PREP-SELECTED %zu\n", sel_n);

    for (i = 0; i < g_prep_n; i++) {
        char abs[PATH_MAX];
        char rel_arg[MANIFEST_PATH_MAX + 2u];
        uint64_t before;
        int rc;

        if (!g_prep[i].selected) {
            (*skipped_n)++;
            continue;
        }
        done_n++;

        /* The precision restates what add_to already guaranteed (the path
         * fits the field, terminator included), spelled out so the bound is
         * one the compiler checks rather than one a reader has to trust. */
        (void)snprintf(abs, sizeof(abs), "%s/%.*s", P2_MNT,
                       (int)(sizeof(g_prep[i].rel) - 1u), g_prep[i].rel);
        (void)snprintf(rel_arg, sizeof(rel_arg), "/%.*s",
                       (int)(sizeof(g_prep[i].rel) - 1u), g_prep[i].rel);

        say("\n[%zu/%zu] /%s  (%llu MiB)\n", done_n, sel_n, g_prep[i].rel,
            (unsigned long long)(g_prep[i].size / MIB));
        if (g_prep[i].state == MANIFEST_BLESS_STALE)
            say("      %s\n", g_prep_ui.stale_note);
        /* Named phase by phase, because the two are minutes apart in cost and
         * a user watching a still console needs to know which one it is in.
         * The tool announces the repack; the hash it announces once and then
         * has nothing to say for, which is what the beat below covers. */
        say("      hashing %llu MiB, then repacking boot.wim; the tool's own\n"
            "      progress follows. The hash is the long quiet part.\n",
            (unsigned long long)(g_prep[i].size / MIB));

        before = p2_free_bytes();
        {
            char *argv[] = {
                (char *)SIGN_TOOL, (char *)"prepare",
                abs,
                (char *)"--stick", (char *)P2_MNT,
                (char *)"--iso-path", rel_arg,
                NULL
            };

            rc = run(argv, NULL, 0, PREP_BEAT_S, NULL);
        }
        if (rc == 0) {
            say("      FALCON-APPLIANCE-PREPARED %s\n", g_prep[i].rel);
            (*prepared_n)++;
            continue;
        }
        say("      FALCON-APPLIANCE-PREP-FAILED %s (exit %d)\n",
            g_prep[i].rel, rc);
        if (before < PREP_TIGHT_BYTES)
            say("      p2 had %llu MiB free when this started, which is the\n"
                "      first thing to check: one prepared image costs about\n"
                "      600 MiB there and the run needs roughly 1.2 GiB more\n"
                "      while it works.\n",
                (unsigned long long)(before / MIB));
        (*failed_n)++;
    }
}

/* The gate, then the batch. Nothing here reads a key or touches p3, and the
 * gate runs before the bless half for the same reason the batch does: an
 * auto-prep boot must cost no passphrase (DESIGN Section 6.3.1). */
void prepare_half(size_t *prepared_n, size_t *failed_n,
                         size_t *skipped_n)
{
    say("\n%zu Windows image%s on p2 %s not prepared yet. Preparing signs\n"
        "nothing and needs no key, so p3 stays locked for it.\n",
        g_prep_n, (g_prep_n == 1u) ? "" : "s",
        (g_prep_n == 1u) ? "is" : "are");
    say("p2 has %llu MiB free.\n",
        (unsigned long long)(p2_free_bytes() / MIB));
    say("\nFALCON-APPLIANCE-PREP-PENDING %zu\n", g_prep_n);

    if (!choose(g_prep, g_prep_n, &g_prep_ui)) {
        *skipped_n = g_prep_n;
        return;
    }
    prepare_batch(prepared_n, failed_n, skipped_n);
}
