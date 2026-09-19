/* falcon-boot appliance: the survey of p2 against the manifest. */

#include "appliance.h"

#include <route.h>

#include <sign/sign.h>

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- the ISO survey ---- */

/* The survey's results, described where they are declared (appliance.h). */
manifest_record g_records[MAX_RECORDS];
size_t          g_record_n;
candidate       g_cand[MAX_CANDIDATES];
size_t          g_cand_n;
size_t          g_done_blessed;
size_t          g_windows;
size_t          g_unreadable;
char            g_manifest[MANIFEST_MAX];

void load_manifest(void)
{
    int fd = open(MANIFEST_PATH, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    size_t count = 0;

    g_record_n = 0;
    if (fd < 0) {
        /* DESIGN Section 8.1: an absent manifest is "nothing is blessed",
         * never an error. */
        say("manifest: none yet on this stick\n");
        return;
    }
    n = read(fd, g_manifest, sizeof(g_manifest));
    (void)close(fd);
    if (n <= 0) {
        say("manifest: empty\n");
        return;
    }
    (void)manifest_parse(g_manifest, (size_t)n, g_records, MAX_RECORDS,
                         &count);
    g_record_n = (count > MAX_RECORDS) ? MAX_RECORDS : count;
    say("manifest: %zu blessed record(s)\n", g_record_n);
}

static int ends_with_iso(const char *name)
{
    size_t n = strlen(name);

    return n > 4u && strcasecmp(name + n - 4u, ".iso") == 0;
}

/* Appends to the bless queue. */
static void add_to(candidate *q, size_t *n, const char *rel, uint64_t size,
                   manifest_bless st, family_id fam, sgn_payload_shape shape)
{
    if (*n >= MAX_CANDIDATES) {
        say("note: more than %u images to sign; the rest are not listed\n",
            (unsigned)MAX_CANDIDATES);
        return;
    }
    if (snprintf(q[*n].rel, sizeof(q[*n].rel), "%s", rel) >=
            (int)sizeof(q[*n].rel)) {
        say("note: skipping '%s': its path is too long for a manifest record\n",
            rel);
        return;
    }
    q[*n].size = size;
    q[*n].state = st;
    q[*n].fam = fam;
    q[*n].shape = shape;
    q[*n].selected = 0;
    (*n)++;
}

/* The family of one ISO, read the way `falcon-boot-sign` reads it: indexed by
 * falcon-core's ISO9660/UDF readers (falcon-cli/src/sign/isoprobe.c), then
 * falcon-core/src/media/family.c fingerprinted over that. Same reader and
 * same table as the tool this program is about to hand the file to, so the
 * survey cannot queue an image the tool will then refuse.
 * No mount and no loop device: falcon-cli/src/sign/isoprobe.c says in as many
 * words why a tool that reads untrusted media should not be mounting it.
 *
 * Returns 0 and *out on success; -1 when nothing here could read the file as
 * an ISO at all, with the reason already in the sign tool's error buffer. */
static int classify(const char *abs, family_id *out)
{
    iso_view *v = NULL;
    family_probe probe;

    if (iso_view_open(abs, &v) != SGN_OK)
        return -1;
    iso_view_family_probe(v, &probe);
    *out = family_detect(&probe)->id;
    iso_view_close(v);
    return 0;
}

/* One file's whole survey, split out because a payload and an ISO answer the
 * two questions below differently and neither answer belongs in the walk.
 *
 * A payload is not read as an ISO: there is nothing in a PE or a FAT image
 * for the ISO readers to index, so classify() would report it unreadable and drop a
 * perfectly blessable file. Its family is decided inside the tool, from the
 * volumes inside the image, and only to refuse the families that boot
 * something after the payload (sgn_payload_refusal).
 *
 * Nor is a payload looked up in the manifest. It has no record by design
 * (DESIGN Section 7.2), so a lookup could only find one somebody wrote by
 * hand, and honouring that would be treating an editable file as evidence
 * about a signature. Whether it is already blessed is asked of the PE itself,
 * later, where the certificate to ask with exists (payload_prefilter). */
void survey_file(const char *abs, const char *rel, uint64_t size)
{
    sgn_payload_shape shape = sgn_payload_shape_of(rel);
    const manifest_record *rec = NULL;
    manifest_bless state = MANIFEST_BLESS_NONE;
    family_id fam = FAMILY_UNKNOWN;
    appl_job job;

    if (shape != SGN_PAYLOAD_NONE) {
        job = appl_route_payload(APPL_OURS_UNKNOWN);
    } else {
        state = manifest_lookup(g_records, g_record_n, rel, size, &rec);
        /* A prepared record an older appliance left names a patched boot.wim,
         * not a wrap; it is no evidence this ISO was blessed. */
        if (rec != NULL && rec->kind != MANIFEST_KIND_BLESSED)
            state = MANIFEST_BLESS_NONE;
        if (classify(abs, &fam) != 0) {
            /* Not a refusal to proceed, and not a silent drop either: an
             * unreadable file named .iso is not offered, because the tool
             * would fail on it further in and with less to say about why. */
            say("  unreadable  /%s\n              %s\n", rel, sgn_error());
            g_unreadable++;
            return;
        }
        job = appl_route(state, fam);
    }

    switch (job) {
    case APPL_JOB_DONE_BLESSED:
        say("  blessed     /%s (already; skipping)\n", rel);
        g_done_blessed++;
        break;
    case APPL_JOB_BOOTS_AS_IS:
        say("  windows     /%s\n"
            "              Windows media boots as it is; nothing to bless\n",
            rel);
        g_windows++;
        break;
    case APPL_JOB_SIGN:
    default:
        add_to(g_cand, &g_cand_n, rel, size, state, fam, shape);
        break;
    }
}

/* Recursive walk of p2 looking for what the loader's own scan lists: *.iso,
 * *.img and *.efi (falcon-efi/src/efi/main.c). `rel` is the path so far
 * relative to the p2 root, which is the form the manifest records and the form
 * `sign` embeds in the signed cmdline. */
void walk(const char *abs, const char *rel, unsigned depth)
{
    DIR *d = opendir(abs);
    struct dirent *e;

    if (d == NULL)
        return;

    while ((e = readdir(d)) != NULL) {
        char child_abs[PATH_MAX];
        char child_rel[MANIFEST_PATH_MAX];
        struct stat st;

        if (e->d_name[0] == '.')
            continue;
        if (depth == 0 && strcasecmp(e->d_name, RESERVED_DIR) == 0)
            continue;   /* our own staging directory, never a candidate */
        if (snprintf(child_abs, sizeof(child_abs), "%s/%s", abs, e->d_name) >=
                (int)sizeof(child_abs))
            continue;
        if (snprintf(child_rel, sizeof(child_rel), "%s%s%s", rel,
                     rel[0] != '\0' ? "/" : "", e->d_name) >=
                (int)sizeof(child_rel))
            continue;
        if (lstat(child_abs, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            if (depth + 1u < WALK_MAX_DEPTH)
                walk(child_abs, child_rel, depth + 1u);
            continue;
        }
        if (!S_ISREG(st.st_mode))
            continue;
        if (!ends_with_iso(e->d_name) &&
            sgn_payload_shape_of(e->d_name) == SGN_PAYLOAD_NONE)
            continue;

        survey_file(child_abs, child_rel, (uint64_t)st.st_size);
    }
    (void)closedir(d);
}

void payload_prefilter(void)
{
    uint8_t *der = NULL;
    size_t der_len = 0;
    size_t kept = 0;
    size_t i;

    if (sgn_payload_cert_der(CERT_PATH, &der, &der_len) != SGN_OK) {
        say("note: %s could not be read as a certificate (%s), so no payload\n"
            "      can be recognised as already carrying it.\n",
            CERT_PATH, sgn_error());
        return;
    }

    for (i = 0; i < g_cand_n; i++) {
        appl_ours ours = APPL_OURS_UNKNOWN;

        if (g_cand[i].shape == SGN_PAYLOAD_EFI) {
            char abs[PATH_MAX];

            (void)snprintf(abs, sizeof(abs), "%s/%.*s", P2_MNT,
                           (int)(sizeof(g_cand[i].rel) - 1u), g_cand[i].rel);
            ours = sgn_payload_signed_with(abs, der, der_len) ? APPL_OURS_YES
                                                              : APPL_OURS_NO;
        }
        if (g_cand[i].shape != SGN_PAYLOAD_NONE &&
            appl_route_payload(ours) == APPL_JOB_DONE_BLESSED) {
            say("  blessed     /%s (already; this stick's certificate is in "
                "it)\n", g_cand[i].rel);
            g_done_blessed++;
            continue;
        }
        g_cand[kept++] = g_cand[i];
    }
    g_cand_n = kept;
    free(der);
}
