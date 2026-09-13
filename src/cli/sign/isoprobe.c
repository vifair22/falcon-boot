/* airies-boot-sign: reading an ISO without mounting it.
 *
 * The bless tool has to fingerprint an ISO's layout (DESIGN Section 6.2)
 * and pull one file out of it. Loop-mounting would need root, which the
 * on-stick signing appliance has but a user blessing an ISO on their own
 * laptop should not need, and which would put a mount of untrusted media
 * into a tool whose whole job is to be the careful step. So the ISO is read
 * with bsdtar, which speaks ISO9660 and Rock Ridge natively: one listing up
 * front becomes the directory tree, and members are extracted on demand.
 *
 * The listing is also what lets src/family/family.c run here unchanged. It
 * reaches the filesystem through two callbacks precisely so the same
 * detection code can fingerprint a firmware-mounted ESP in the loader and a
 * bsdtar listing on a host, and this file is the second of those two
 * bindings.
 *
 * Path convention. family.h specifies absolute, '/'-separated, ASCII,
 * no trailing slash, matched case-insensitively. bsdtar prints paths
 * relative to the image root with no leading slash, and directory entries
 * sometimes with a trailing one, so every comparison here normalises both
 * sides rather than trusting either.
 */

#include <cli/sign/sign.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A listing is a few hundred kilobytes even for a bloated 5 GB desktop ISO
 * (the kubuntu 26.04 image on this machine lists 758 entries). A megabyte
 * is room for two orders of magnitude more; past that something is wrong
 * with the image and stopping is better than growing. */
#define LISTING_MAX (4u << 20)

/* Content fingerprints read the head of a small text file (family.c asks
 * for 1 KB of a loader entry). Nothing legitimate needs more, and the cap
 * keeps a malicious ISO from making the tool buffer a gigabyte. */
#define MEMBER_MAX (4u << 20)

struct iso_view {
    char   *path;       /* the ISO itself */
    char   *listing;    /* bsdtar output, newlines replaced by NULs */
    char  **entries;    /* pointers into listing, trailing '/' trimmed */
    size_t  n_entries;
};

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* ASCII case-insensitive comparison of n bytes. The media we fingerprint
 * are case-insensitive by nature (ISO9660 and the UEFI File Protocol both),
 * and all of it is ASCII by construction. */
static int ci_eq(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (lower_ascii(a[i]) != lower_ascii(b[i]))
            return 0;
    }
    return 1;
}

/* Strips the leading separators family.h puts on every query path. */
static const char *strip_leading(const char *p)
{
    while (*p == '/')
        p++;
    return p;
}

/* ------------------------------------------------------------ open */

static int split_listing(iso_view *v)
{
    size_t count = 0;
    size_t i = 0;
    char *p = v->listing;

    for (i = 0; p[i] != '\0'; i++) {
        if (p[i] == '\n')
            count++;
    }
    count++;    /* a final line with no newline */

    v->entries = calloc(count, sizeof(*v->entries));
    if (v->entries == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory indexing the ISO listing");
        return SGN_ERR_IO;
    }

    i = 0;
    while (p[i] != '\0') {
        char *line = p + i;
        size_t n = 0;

        while (p[i] != '\0' && p[i] != '\n')
            i++;
        n = (size_t)(p + i - line);
        if (p[i] == '\n')
            p[i++] = '\0';
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '/'))
            line[--n] = '\0';
        /* bsdtar emits "." for the image root; it names nothing. */
        if (n == 0 || (n == 1u && line[0] == '.'))
            continue;
        /* Stored entries can never exceed the line count the allocation
         * was sized from; the explicit bound states that invariant. */
        if (v->n_entries >= count)
            break;
        v->entries[v->n_entries++] = line;
    }
    return SGN_OK;
}

int iso_view_open(const char *path, iso_view **out)
{
    iso_view *v;
    char *argv[5];
    size_t len = 0;
    int rc;

    if (path == NULL || out == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "iso_view_open: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    *out = NULL;
    if (!sgn_file_exists(path)) {
        sgn_set_error(SGN_ERR_IO, "no such ISO: '%s'", path);
        return SGN_ERR_IO;
    }

    v = calloc(1, sizeof(*v));
    if (v == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory");
        return SGN_ERR_IO;
    }
    v->path = strdup(path);
    if (v->path == NULL) {
        free(v);
        sgn_set_error(SGN_ERR_IO, "out of memory");
        return SGN_ERR_IO;
    }

    argv[0] = sgn_arg("bsdtar");
    argv[1] = sgn_arg("-tf");
    argv[2] = v->path;
    argv[3] = NULL;
    argv[4] = NULL;
    rc = sgn_run_capture(argv, &v->listing, &len, LISTING_MAX);
    if (rc != SGN_OK) {
        iso_view_close(v);
        return rc;
    }
    if (len == 0) {
        iso_view_close(v);
        sgn_set_error(SGN_ERR_FORMAT, "'%s' contains no files; not an ISO?",
                      path);
        return SGN_ERR_FORMAT;
    }
    rc = split_listing(v);
    if (rc != SGN_OK) {
        iso_view_close(v);
        return rc;
    }
    sgn_log(SGN_LOG_DEBUG, "listed %zu entries in %s", v->n_entries, path);
    *out = v;
    return SGN_OK;
}

void iso_view_close(iso_view *v)
{
    if (v == NULL)
        return;
    free(v->entries);
    free(v->listing);
    free(v->path);
    free(v);
}

/* ------------------------------------------------------------ lookup */

/* Does entry name query, or something inside it? The second half is what
 * makes a query for a directory work when the listing only enumerates its
 * contents; family.c asks about bare directories ("/casper", "/arch") more
 * often than about files. */
static int entry_matches(const char *entry, const char *query, size_t qlen)
{
    size_t elen = strlen(entry);

    if (elen < qlen)
        return 0;
    if (!ci_eq(entry, query, qlen))
        return 0;
    return elen == qlen || entry[qlen] == '/';
}

int iso_view_find(iso_view *v, const char *path, const char **match)
{
    const char *q;
    size_t qlen;
    size_t i;
    int wild;

    if (match != NULL)
        *match = NULL;
    if (v == NULL || path == NULL)
        return 0;
    q = strip_leading(path);
    qlen = strlen(q);
    wild = (qlen > 0 && q[qlen - 1u] == '*');
    if (wild)
        qlen--;
    if (qlen == 0)
        return 0;

    for (i = 0; i < v->n_entries; i++) {
        const char *e = v->entries[i];

        if (wild) {
            if (strlen(e) < qlen || !ci_eq(e, q, qlen))
                continue;
        } else if (!entry_matches(e, q, qlen)) {
            continue;
        }
        if (match != NULL)
            *match = e;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ probe */

static int probe_exists(void *ctx, const char *path)
{
    return iso_view_find((iso_view *)ctx, path, NULL);
}

static int probe_read(void *ctx, const char *path, uint64_t off, void *buf,
                      size_t len, size_t *got)
{
    iso_view *v = ctx;
    const char *member = NULL;
    char *argv[7];
    char *data = NULL;
    size_t dlen = 0;
    size_t take;
    int rc;

    if (got != NULL)
        *got = 0;
    if (!iso_view_find(v, path, &member) || member == NULL)
        return 1;

    argv[0] = sgn_arg("bsdtar");
    argv[1] = sgn_arg("-x");
    argv[2] = sgn_arg("-O");
    argv[3] = sgn_arg("-f");
    argv[4] = v->path;
    argv[5] = sgn_arg(member);
    argv[6] = NULL;
    rc = sgn_run_capture(argv, &data, &dlen, MEMBER_MAX);
    if (rc != SGN_OK)
        return 1;

    if (off >= (uint64_t)dlen) {
        free(data);
        return 0;   /* past end of file: zero bytes, not an error */
    }
    take = dlen - (size_t)off;
    if (take > len)
        take = len;
    memcpy(buf, data + off, take);
    free(data);
    if (got != NULL)
        *got = take;
    return 0;
}

void iso_view_family_probe(iso_view *v, family_probe *fp)
{
    if (fp == NULL)
        return;
    fp->exists = probe_exists;
    fp->read = probe_read;
    fp->ctx = v;
}

/* ------------------------------------------------------------ extract */

int iso_view_extract(iso_view *v, const char *member, const char *dst)
{
    char *argv[7];

    if (v == NULL || member == NULL || dst == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "iso_view_extract: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    argv[0] = sgn_arg("bsdtar");
    argv[1] = sgn_arg("-x");
    argv[2] = sgn_arg("-O");
    argv[3] = sgn_arg("-f");
    argv[4] = v->path;
    argv[5] = sgn_arg(member);
    argv[6] = NULL;
    return sgn_run_to_file(argv, dst);
}

/* ------------------------------------------------------ kernel paths */

/* Where each family keeps the kernel the bless wrap is built around.
 *
 * This table lives in the tool and not in src/family, on purpose. The
 * registry is the loader's data: it answers "what cmdline does this family
 * need", which the loader consumes at boot. Which file inside the ISO holds
 * the kernel is a question only the signing step ever asks, and answering
 * it in the registry would put a host-tooling concern into an EFI module
 * for no one's benefit.
 *
 * Candidates are tried in order; a trailing '*' matches any suffix, which
 * is what Debian live images need since their kernel carries the version.
 * Verification status per row is in the sign tool's section of the report
 * and in docs/notes when it lands: the Arch and Ubuntu rows are checked
 * against real media on this machine, the rest are from the families'
 * published layouts and are marked as such. */
typedef struct {
    family_id          id;
    const char *const *paths;
} kernel_row;

static const char *const k_casper[] = {
    "/casper/vmlinuz", "/casper/vmlinuz.efi", "/casper/vmlinuz*", NULL
};
static const char *const k_debian_live[] = {
    "/live/vmlinuz", "/live/vmlinuz*", NULL
};
static const char *const k_fedora[] = {
    "/images/pxeboot/vmlinuz", "/isolinux/vmlinuz", NULL
};
static const char *const k_arch[] = {
    "/arch/boot/x86_64/vmlinuz-linux", NULL
};
static const char *const k_systemrescue[] = {
    "/sysresccd/boot/x86_64/vmlinuz", NULL
};

static const kernel_row g_kernels[] = {
    { FAMILY_DEBIAN_CASPER, k_casper },
    { FAMILY_DEBIAN_LIVE,   k_debian_live },
    { FAMILY_FEDORA_DRACUT, k_fedora },
    { FAMILY_ARCH,          k_arch },
    { FAMILY_SYSTEMRESCUE,  k_systemrescue },
    /* FAMILY_MEMTEST is a bare EFI binary with no kernel to wrap, and it
     * needs no blessing: it boots with no cmdline and no backing media.
     * FAMILY_WINDOWS boots no Linux kernel at all. Both are absent rather
     * than empty so the "no entry" message can say why. */
};

const char *iso_kernel_path(iso_view *v, family_id id)
{
    size_t i;

    if (v == NULL)
        return NULL;
    for (i = 0; i < sizeof(g_kernels) / sizeof(g_kernels[0]); i++) {
        size_t j;

        if (g_kernels[i].id != id)
            continue;
        for (j = 0; g_kernels[i].paths[j] != NULL; j++) {
            const char *match = NULL;

            if (iso_view_find(v, g_kernels[i].paths[j], &match))
                return match;
        }
        sgn_set_error(SGN_ERR_FORMAT,
                      "no kernel found in the ISO for family '%s' (looked for "
                      "%s and %zu alternative(s))",
                      family_by_id(id)->name, g_kernels[i].paths[0], j - 1u);
        return NULL;
    }
    sgn_set_error(SGN_ERR_FORMAT,
                  "family '%s' has no kernel to wrap: it does not boot a "
                  "Linux kernel from the ISO, so blessing does not apply",
                  family_by_id(id)->name);
    return NULL;
}
