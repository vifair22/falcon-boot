/* falcon-boot appliance: the bless itself. */

#include "appliance.h"

#include <sign/sign.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

static ssize_t read_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    size_t len = 0;

    if (fd < 0)
        return -1;
    for (;;) {
        ssize_t n = read(fd, buf + len, cap - 1u - len);

        if (n < 0) {
            (void)close(fd);
            return -1;
        }
        if (n == 0)
            break;
        len += (size_t)n;
        if (len + 1u >= cap)
            break;
    }
    (void)close(fd);
    buf[len] = '\0';
    return (ssize_t)len;
}

/* The confirmation prompt. Echoed on purpose: the user is meant to see the
 * word they are typing, because typing it is the whole ceremony. The echo is
 * the terminal's own, so it appears on the console being typed at and not on
 * the others; only what this program prints goes everywhere. */

/* ---- the bless (DESIGN Section 7.5) ----
 *
 * Everything that needs the key lives in here, and nothing calls it unless
 * the survey found an image that needs one. Failures report and RETURN rather
 * than rebooting: the summary, and the wait for Enter over it, are still owed
 * to the user. Whatever was opened on p3 is closed again on every return
 * (sign_all does the work between the two).
 */
shelf_keys g_keys = { -1, -1, "", "" };

static void sign_all(const stick_parts *parts, int confirmed,
                     size_t *signed_n, size_t *failed_n, size_t *skipped_n);

/* One attempt with pass. 1 when p3 opened, 0 when the passphrase was
 * wrong, -1 when p3 cannot be opened at all (said why). */
static int try_unlock(const char *pass, size_t len)
{
    int rc = shelf_unlock(pass, len);

    if (rc == SHELF_OK)
        return 1;
    if (rc == SHELF_WRONG)
        return 0;
    say("ERROR: p3's volume could not be read after unlocking it.\n");
    return -1;
}

void bless_half(const stick_parts *parts, int confirmed, size_t *signed_n,
                size_t *failed_n, size_t *skipped_n)
{
    char pass[PASS_MAX];
    int opened = 0;
    int tries;
    int rc;

    /* ---- unlock p3 ----
     *
     * In process, with falcon-core's LUKS2 (shelf.c): the passphrase never
     * leaves this program's memory and is wiped after each attempt.
     *
     * First attempt: the credential the loader delivered. The user already
     * typed the passphrase once to reach the chooser, and the Section 5.1
     * machinery hands it to every UKI the loader launches, this one
     * included; asking again would be the double prompt the whole handoff
     * design exists to remove. The interactive loop below stays as the
     * fallback for a missing or stale credential (a buffer-load fallback
     * launch delivers none), so a failed credential costs one message, not
     * the session. */
    rc = shelf_open(parts->p3);
    if (rc != SHELF_OK) {
        say("ERROR: p3 (%s) is not a LUKS2 volume this appliance can open;\n"
            "       nothing was unlocked.\n", parts->p3);
        *skipped_n = g_cand_n;
        return;
    }
    {
        ssize_t clen = read_file("/.extra/credentials/" CRED_FILE,
                                 pass, sizeof(pass));

        if (clen > 0) {
            say("unlocking p3 with the passphrase from the loader...\n");
            opened = try_unlock(pass, (size_t)clen);
            if (opened == 0)
                say("the delivered credential did not open p3; "
                    "falling back to the prompt.\n");
        }
        explicit_bzero(pass, sizeof(pass));
    }
    for (tries = 1; opened == 0 && tries <= UNLOCK_TRIES; tries++) {
        ssize_t len;

        say("passphrase for p3 (attempt %d of %d): ", tries, UNLOCK_TRIES);
        len = read_secret(pass, sizeof(pass));
        if (len <= 0) {
            say("no passphrase given; nothing was unlocked.\n");
            explicit_bzero(pass, sizeof(pass));
            shelf_close();
            *skipped_n = g_cand_n;
            return;
        }
        opened = try_unlock(pass, (size_t)len);
        explicit_bzero(pass, sizeof(pass));
        if (opened == 0)
            say("wrong passphrase.\n");
    }
    if (opened != 1) {
        if (opened == 0)
            say("\nout of attempts. p3 stays locked and no key was read.\n");
        shelf_close();
        *skipped_n = g_cand_n;
        return;
    }
    say("p3 unlocked (read-only).\n");

    if (shelf_keypair(&g_keys) != SHELF_OK) {
        say("ERROR: no MOK keypair on p3 (falcon-mok.key / falcon-mok.crt).\n"
            "       A stick built with --p3-plain has no on-stick key, so it\n"
            "       cannot bless anything here; sign from a host instead.\n");
        shelf_close();
        *skipped_n = g_cand_n;
        return;
    }
    say("MOK keypair found on p3.\n");

    sign_all(parts, confirmed, signed_n, failed_n, skipped_n);
    shelf_keys_close(&g_keys);
    shelf_close();
}

static void sign_all(const stick_parts *parts, int confirmed,
                     size_t *signed_n, size_t *failed_n, size_t *skipped_n)
{
    char dev[32];
    char dev_arg[48];
    int need_dev = 0;
    size_t i;

    dev_arg[0] = '\0';

    payload_prefilter();
    if (g_cand_n == 0) {
        say("\nnothing left to sign: every payload on p2 already carries this\n"
            "stick's certificate. FALCON-APPLIANCE-PENDING 0\n");
        return;
    }

    /* Needed only by the wrap path: it is the %DEV% the signed cmdline
     * carries, and a bare payload has no cmdline to put it in. A queue of payloads alone must not die on a volume serial
     * nothing in it would use. */
    for (i = 0; i < g_cand_n; i++)
        need_dev |= (g_cand[i].shape == SGN_PAYLOAD_NONE);
    if (need_dev) {
        if (p2_uuid(parts->p2, dev, sizeof(dev)) != 0) {
            say("ERROR: could not read p2's volume serial; the signed cmdline\n"
                "       would have no way to name the partition to boot "
                "from.\n");
            *skipped_n = g_cand_n;
            return;
        }
        (void)snprintf(dev_arg, sizeof(dev_arg), "UUID=%s", dev);
        say("p2 volume: %s\n", dev_arg);
    }

    say("\nFALCON-APPLIANCE-PENDING %zu\n", g_cand_n);

    /* ---- mark, review, confirm, then the bless ----
     *
     * DESIGN Section 7.5: the marks are the per-image vouch and the typed
     * "yes" over the reviewed set releases the batch. Nothing below runs
     * unless both happened. */
    if (confirmed) {
        /* The consent was given in the loader's menu, over exactly this
         * list (DESIGN Section 7.5): printed again here so the
         * console says what is being signed as it is signed. */
        say("\nconfirmed in the loader's menu:\n");
        for (i = 0; i < g_cand_n; i++) {
            g_cand[i].selected = 1;
            say("  /%s\n", g_cand[i].rel);
        }
    } else if (!choose(g_cand, g_cand_n)) {
        /* Nothing was vouched for, so every pending image is a skip. */
        *skipped_n = g_cand_n;
        return;
    }

    {
        size_t sel_n = selected_count(g_cand, g_cand_n);
        size_t done_n = 0;

        say("\nFALCON-APPLIANCE-SELECTED %zu\n", sel_n);

        for (i = 0; i < g_cand_n; i++) {
            char abs[PATH_MAX];
            char rel_arg[MANIFEST_PATH_MAX + 2u];
            int rc;

            if (!g_cand[i].selected) {
                (*skipped_n)++;
                continue;
            }
            done_n++;

            /* The precision restates what add_to already guaranteed (the path
             * fits the field, terminator included). It is spelled out here
             * because the bound is what keeps a p2 path from running off the
             * end of either buffer, and a bound stated in the call is one the
             * compiler checks rather than one a reader has to trust. */
            (void)snprintf(abs, sizeof(abs), "%s/%.*s", P2_MNT,
                           (int)(sizeof(g_cand[i].rel) - 1u), g_cand[i].rel);
            (void)snprintf(rel_arg, sizeof(rel_arg), "/%.*s",
                           (int)(sizeof(g_cand[i].rel) - 1u), g_cand[i].rel);

            say("\n[%zu/%zu] /%s\n", done_n, sel_n, g_cand[i].rel);
            if (g_cand[i].shape != SGN_PAYLOAD_NONE) {
                /* No --iso-path, --dev or --stub: there is no wrap to build, so
                 * the tool rejects them rather than ignoring them
                 * (falcon-cli/src/sign/main.c, bless_payload). Its output is
                 * echoed for the same reason the review echoed the dry run -
                 * this is the run that writes to the user's file, and what it
                 * did belongs on the console that asked them about it. */
                char *argv[] = {
                    (char *)SIGN_TOOL,
                    abs,
                    (char *)"--stick", (char *)P2_MNT,
                    (char *)"--key", g_keys.key,
                    (char *)"--cert", g_keys.cert,
                    NULL
                };

                say("      signing it in place...\n");
                rc = run(argv, NULL, 0, "      ");
            } else {
                char *argv[] = {
                    (char *)SIGN_TOOL,
                    abs,
                    (char *)"--stick", (char *)P2_MNT,
                    (char *)"--key", g_keys.key,
                    (char *)"--cert", g_keys.cert,
                    (char *)"--iso-path", rel_arg,
                    (char *)"--dev", dev_arg,
                    NULL
                };

                say("      signing...\n");
                rc = run(argv, NULL, 0, NULL);
            }
            /* One image failing is that image's problem: the rest of the batch
             * was vouched for too and still deserves to be signed. */
            if (rc == 0) {
                say("      FALCON-APPLIANCE-SIGNED %s\n", g_cand[i].rel);
                (*signed_n)++;
            } else {
                say("      FALCON-APPLIANCE-FAILED %s (exit %d)\n",
                    g_cand[i].rel, rc);
                (*failed_n)++;
            }
        }
    }
}
