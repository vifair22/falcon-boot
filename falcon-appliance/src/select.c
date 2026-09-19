/* falcon-boot appliance: the pending list and the gate it passes. */

#include "appliance.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ---- the pending list and the selection ceremony ----
 *
 * DESIGN Section 7.5 (select-then-sign, 2026-09-13): the user marks the images
 * one at a time, the whole marked set is shown back once, and only a typed
 * "yes" over that reviewed set starts any work. The listing is grouped by
 * folder so it reads like the loader's own folder-view menu (Section 6 stage
 * 1); the grouping is presentation only, everything below still works on the
 * full p2-relative path. */

/* The file name half of a p2-relative path. */
static const char *base_of(const char *rel)
{
    const char *slash = strrchr(rel, '/');

    return (slash != NULL) ? slash + 1 : rel;
}

/* The folder half, "" for an image sitting at the p2 root. */
static void dir_of(char *out, size_t cap, const char *rel)
{
    const char *slash = strrchr(rel, '/');
    size_t n = (slash != NULL) ? (size_t)(slash - rel) : 0u;

    if (n >= cap)
        n = cap - 1u;
    memcpy(out, rel, n);
    out[n] = '\0';
}

/* Path order, with the folder compared before the file name so every folder's
 * images come out contiguous and the root's come first ("" sorts before any
 * folder name). That is the order the numbers, the review and the signing run
 * all use, so the number next to an image never moves under the user. */
int cand_cmp(const void *a, const void *b)
{
    const candidate *x = (const candidate *)a;
    const candidate *y = (const candidate *)b;
    char dx[MANIFEST_PATH_MAX];
    char dy[MANIFEST_PATH_MAX];
    int c;

    dir_of(dx, sizeof(dx), x->rel);
    dir_of(dy, sizeof(dy), y->rel);
    c = strcmp(dx, dy);
    if (c != 0)
        return c;
    return strcmp(base_of(x->rel), base_of(y->rel));
}

size_t selected_count(const candidate *q, size_t n)
{
    size_t i;
    size_t sel = 0;

    for (i = 0; i < n; i++) {
        if (q[i].selected)
            sel++;
    }
    return sel;
}

/* A size a human reads at a glance. KiB below a mebibyte, because the library
 * now holds bare payloads as well as images and a 900 KiB Shell.efi listed as
 * "0 MiB" reads as an empty file. */
static void size_str(char *out, size_t cap, uint64_t bytes)
{
    if (bytes < MIB)
        (void)snprintf(out, cap, "%llu KiB",
                       (unsigned long long)(bytes / 1024u));
    else
        (void)snprintf(out, cap, "%llu MiB",
                       (unsigned long long)(bytes / MIB));
}

/* One image's line. Printed on its own after a toggle so a dumb serial console
 * does not have to redraw the whole list for every keystroke. */
static void print_item(const candidate *q, size_t i)
{
    char size[32];

    size_str(size, sizeof(size), q[i].size);
    say("    [%c] %2zu  %s  (%s)\n", q[i].selected ? 'x' : ' ',
        i + 1u, base_of(q[i].rel), size);
    if (q[i].state == MANIFEST_BLESS_STALE)
        say("           blessed earlier; the file has changed since\n");
}

static void print_list(const candidate *q, size_t n)
{
    char dir[MANIFEST_PATH_MAX];
    char cur[MANIFEST_PATH_MAX];
    size_t i;
    int have_cur = 0;

    cur[0] = '\0';
    say("\npending unsigned images:\n\n");
    for (i = 0; i < n; i++) {
        dir_of(dir, sizeof(dir), q[i].rel);
        if (!have_cur || strcmp(dir, cur) != 0) {
            /* "/" for the root, "Linux/" for a folder: one shape, and the
             * trailing slash is what says "this is a folder, not an image". */
            say("  %s/\n", dir);
            (void)snprintf(cur, sizeof(cur), "%s", dir);
            have_cur = 1;
        }
        print_item(q, i);
    }
}

/* A bare 1-based image number, and nothing else. Returns 1 and the 0-based
 * index for a number in range, -1 for a number that names no image, 0 for
 * anything that is not a number. Deliberately strict: "2 " or "2y" is a typo,
 * and guessing what a typo meant is not a thing to do in front of a signing
 * key. */
static int parse_index(const char *s, size_t n, size_t *out)
{
    unsigned long v = 0;
    size_t i;

    if (s[0] == '\0')
        return 0;
    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = v * 10u + (unsigned long)(s[i] - '0');
        if (v > (unsigned long)n)
            return -1;   /* keeps the accumulator from running away too */
    }
    if (v == 0u)
        return -1;
    *out = (size_t)v - 1u;
    return 1;
}

/* What blessing this payload will write to, in the sign tool's own words and
 * before the yes that releases it (DESIGN Section 7.2).
 *
 * The words are the tool's rather than ours, and that is the point. --dry-run
 * is the bless about to be confirmed minus the writes: it opens the image the
 * same way and names every file inside it that would be written to, partition
 * by partition. A sentence composed here would be a second description of what
 * falcon-cli/src/sign/payload.c does, free to drift from it, and it could not
 * name what is inside a .img without a second copy of that volume walk.
 *
 * The output is echoed through this program rather than left to the child's
 * inherited console, because this is the statement the whole gate turns on
 * and it has to arrive on the console the user is answering from.
 *
 * A refusal (a family whose boot runs something after the payload) surfaces
 * here too, which is the right moment for it: before anything is vouched
 * for. */
static void payload_disclosure(const candidate *c)
{
    char abs[PATH_MAX];

    (void)snprintf(abs, sizeof(abs), "%s/%.*s", P2_MNT,
                   (int)(sizeof(c->rel) - 1u), c->rel);
    {
        char *argv[] = {
            (char *)SIGN_TOOL, abs,
            (char *)"--key", (char *)KEY_PATH,
            (char *)"--cert", (char *)CERT_PATH,
            (char *)"--dry-run", NULL
        };

        if (run(argv, NULL, 0, "      ") != 0)
            say("      the sign tool would not act on this file; confirming\n"
                "      the set will fail on it and leave it as it is.\n");
    }
}

/* The batch review. Prints every marked path in full and takes the one word
 * that authorises the batch. 1 = confirmed, 0 = back to the list (a typo must
 * not cost the marks), -1 = the console closed. */
static int review(const candidate *q, size_t n, size_t sel_n)
{
    char answer[32];
    size_t payload_n = 0;
    size_t i;

    say("\nreview: %zu image%s marked\n\n", sel_n, (sel_n == 1u) ? "" : "s");
    for (i = 0; i < n; i++) {
        if (!q[i].selected)
            continue;
        say("  /%s\n", q[i].rel);
        if (q[i].shape != SGN_PAYLOAD_NONE) {
            payload_n++;
            payload_disclosure(&q[i]);
        }
    }
    say("\nSigning these says you trust every one of them to run on this\n"
        "machine with Secure Boot on.\n");
    /* Said over the list and under the tool's own statement about each
     * payload, because it is the difference between blessing one of these
     * and blessing an ISO: an ISO is left untouched, and these are not. */
    if (payload_n > 0)
        say("%zu of these %s a bare EFI payload, and blessing one WRITES TO\n"
            "THAT FILE: the payload boots as itself, so there is nothing\n"
            "beside it for the signature to go into. Everything that will be\n"
            "written to is named above. Blessing an ISO is not like this; it\n"
            "leaves the ISO exactly as it is.\n",
            payload_n, (payload_n == 1u) ? "is" : "are");
    say("type 'yes' to sign them, anything else to go back to the list: ");
    if (read_line(answer, sizeof(answer)) < 0)
        return -1;
    if (strcmp(answer, "yes") != 0) {
        say("not confirmed; back to the list (the marks are kept).\n");
        return 0;
    }
    return 1;
}

/* Runs the whole ceremony over the queue. Returns 1 when a reviewed set was
 * confirmed, 0 when the user left without vouching for anything. The marks
 * live in q[].selected and start clear: nothing is ever pre-marked (DESIGN
 * Section 7.5). */
int choose(candidate *q, size_t n)
{
    print_list(q, n);
    say("\nMark what you vouch for, then review it:\n"
        "  <n>       toggle image n      all, a / none, n  mark every / clear\n"
        "  list, l   reprint the list    done, d           review the marked set\n"
        "  quit, q   reboot without signing anything\n");

    for (;;) {
        char line[64];
        size_t idx;

        say("select> ");
        if (read_line(line, sizeof(line)) < 0) {
            say("\nconsole closed; nothing was signed.\n");
            return 0;
        }
        /* An empty line is "done": it is what a user leans on Enter for, and
         * on an empty selection "done" only re-prompts, so it cannot act. */
        if (line[0] == '\0' || strcasecmp(line, "done") == 0 ||
            strcasecmp(line, "d") == 0) {
            size_t sel = selected_count(q, n);
            int r;

            if (sel == 0) {
                say("nothing is marked. Mark an image by number, or 'quit' to\n"
                    "leave without signing.\n");
                continue;
            }
            r = review(q, n, sel);
            if (r > 0)
                return 1;
            if (r < 0) {
                say("\nconsole closed; nothing was signed.\n");
                return 0;
            }
            continue;
        }
        if (strcasecmp(line, "quit") == 0 || strcasecmp(line, "q") == 0) {
            say("leaving without signing anything.\n");
            return 0;
        }
        if (strcasecmp(line, "list") == 0 || strcasecmp(line, "l") == 0) {
            print_list(q, n);
            continue;
        }
        if (strcasecmp(line, "all") == 0 || strcasecmp(line, "a") == 0) {
            size_t i;

            for (i = 0; i < n; i++)
                q[i].selected = 1;
            print_list(q, n);
            say("all %zu marked; the review lists every one, read it.\n", n);
            continue;
        }
        if (strcasecmp(line, "none") == 0 || strcasecmp(line, "n") == 0) {
            size_t i;

            for (i = 0; i < n; i++)
                q[i].selected = 0;
            print_list(q, n);
            continue;
        }
        {
            int r = parse_index(line, n, &idx);

            if (r > 0) {
                q[idx].selected = !q[idx].selected;
                print_item(q, idx);
                continue;
            }
            if (r < 0) {
                say("there is no image %s; the list has %zu.\n", line, n);
                continue;
            }
        }
        say("not understood: '%s'. Type an image number, or all (a),\n"
            "none (n), list (l), done (d), quit (q).\n", line);
    }
}
