/* falcon-boot-sign: reading an ISO without mounting it.
 *
 * The bless tool has to fingerprint an ISO's layout (DESIGN Section 6.2)
 * and pull one file out of it. Loop-mounting would need root, which the
 * on-stick signing appliance has but a user blessing an ISO on their own
 * laptop should not need, and which would put a mount of untrusted media
 * into a tool whose whole job is to be the careful step. So the ISO is read
 * by listing it with a helper that already speaks the filesystem, exactly as
 * DESIGN Section 8.1's vendoring line asks: one listing up front becomes the
 * directory tree, and members are extracted on demand.
 *
 * Two helpers, because one namespace is not the whole image. bsdtar speaks
 * ISO9660 and Rock Ridge, which is the entire tree of every Linux image here
 * and the reason it stays the primary reader. It does not speak UDF, and
 * Microsoft builds Windows install media with `oscdimg -u1 -udfver102`: the
 * ISO9660 side of that image is a stub holding one ~135-byte /README.TXT and
 * every real file (/sources/install.wim, /efi/microsoft/boot/bcd) exists only
 * in the UDF namespace. Listed with bsdtar alone, Windows media looks like an
 * almost-empty filesystem and classifies as unknown. 7-Zip does speak UDF, so
 * the view is the union of both listings, each entry remembering which helper
 * can extract it. This is the host-side twin of the loader's own UDF fix.
 *
 * 7-Zip is a soft dependency, not a hard one: without it the ISO9660 half
 * still lists and every Linux family still blesses, so a missing 7z is a
 * warning naming what it costs rather than a refusal to run.
 *
 * The listing is also what lets falcon-core/src/media/family.c run here
 * unchanged. It reaches the filesystem through two callbacks precisely so the
 * same detection code can fingerprint a firmware-mounted ESP in the loader and
 * a helper's listing on a host, and this file is the second of those two
 * bindings.
 *
 * Path convention, and the case landmine. family.h specifies absolute,
 * '/'-separated, ASCII, no trailing slash, matched case-insensitively, and
 * its fingerprints are written in the case each family's own media uses. No
 * two readers agree on that case or on the separators: bsdtar prints paths
 * relative to the image root with no leading slash and directories sometimes
 * with a trailing one, 7-Zip prints the UDF name, and on an oscdimg image the
 * UDF name is all lower case ("efi/microsoft/boot/bcd") while the ISO9660
 * stub beside it is all upper. So every comparison here folds both sides and
 * normalises the separators of both, rather than trusting either.
 */

#include <sign/sign.h>
#include <sign/isoprobe.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A bsdtar listing is a few hundred kilobytes even for a bloated 5 GB desktop
 * ISO (the kubuntu 26.04 image on this machine lists 758 entries). A megabyte
 * is room for two orders of magnitude more; past that something is wrong
 * with the image and stopping is better than growing. */
#define LISTING_MAX (4u << 20)

/* 7-Zip's -slt listing spends about ten lines and 250 bytes per entry where
 * bsdtar spends one line and a path, so the same entry budget costs roughly
 * forty times the bytes: the Windows 11 25H2 image lists 1069 entries in
 * 255 KB. The cap is raised to match rather than shared, so that a large
 * image is not rejected purely for the verbosity of the reader. */
#define Z_LISTING_MAX (32u << 20)

/* Content fingerprints read the head of a small text file (family.c asks
 * for 1 KB of a loader entry). Nothing legitimate needs more, and the cap
 * keeps a malicious ISO from making the tool buffer a gigabyte. */
#define MEMBER_MAX (4u << 20)

/* Every -slt property line is "key = value"; only this key names an entry. */
#define Z_PATH_KEY "Path = "

/* The one other property worth keeping: the byte length the ISO's own
 * directory entry records. Preparation needs it to know how long a range it
 * has to locate and serve (DESIGN Section 6.3), and taking it from the
 * listing rather than from the extracted copy is what makes "the length
 * agrees with the directory entry" a real check rather than a tautology.
 * Matched at the start of the line so that "Packed Size = ", which is the
 * cluster-rounded allocation and not the file length, cannot be mistaken
 * for it. */
#define Z_SIZE_KEY "Size = "

typedef struct {
    char      *name;      /* into the owning listing buffer, normalised */
    uint64_t   size;      /* directory-entry length, when the reader gave one */
    iso_reader reader;    /* which helper can extract it */
    uint8_t    has_size;
} iso_entry;

struct iso_view {
    char       *path;                   /* the ISO itself */
    char       *listing[ISO_READER_N];  /* helper output, chopped into lines */
    const char *zbin;                   /* 7-Zip binary, NULL when absent */
    char        zbin_path[PATH_MAX];    /* zbin's storage when resolved here */
    iso_entry  *entries;
    size_t      n_entries;
    size_t      n_iso9660;              /* how many of those came from bsdtar */
    size_t      cap;
};

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* ASCII case-insensitive comparison of n bytes. The media we fingerprint
 * are case-insensitive by nature (ISO9660, UDF and the UEFI File Protocol
 * all), and all of it is ASCII by construction. */
static int ci_eq(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (lower_ascii(a[i]) != lower_ascii(b[i]))
            return 0;
    }
    return 1;
}

/* Strips the leading separators family.h puts on every query path, and the
 * "./" prefix a listing tool may put on every entry of its own. */
static const char *strip_leading(const char *p)
{
    for (;;) {
        if (*p == '/')
            p++;
        else if (p[0] == '.' && p[1] == '/')
            p += 2;
        else
            return p;
    }
}

/* ------------------------------------------------------------ listings */

/* Normalises one raw listing line in place into the convention above: no
 * leading separator, no trailing one, no CR. Returns NULL when the line
 * names nothing, which is the "." bsdtar prints for the image root, the
 * blank lines 7-Zip puts between records, and a line that was all
 * separators. */
static char *normalise_entry(char *line, size_t n)
{
    size_t lead;

    while (n > 0 && (line[n - 1u] == '\r' || line[n - 1u] == '/'))
        line[--n] = '\0';
    lead = (size_t)(strip_leading(line) - line);
    line += lead;
    n -= lead;
    if (n == 0 || (n == 1u && line[0] == '.'))
        return NULL;
    return line;
}

static int entries_push(iso_view *v, char *name, iso_reader reader)
{
    if (v->n_entries == v->cap) {
        size_t cap = (v->cap == 0) ? 256u : v->cap * 2u;
        iso_entry *bigger = realloc(v->entries, cap * sizeof(*bigger));

        if (bigger == NULL) {
            sgn_set_error(SGN_ERR_IO, "out of memory indexing the ISO listing");
            return SGN_ERR_IO;
        }
        v->entries = bigger;
        v->cap = cap;
    }
    v->entries[v->n_entries].name = name;
    v->entries[v->n_entries].reader = reader;
    v->entries[v->n_entries].size = 0;
    v->entries[v->n_entries].has_size = 0;
    v->n_entries++;
    return SGN_OK;
}

/* Decimal, whole string, no sign and no overflow. Anything else means the
 * property was not a plain byte count (7-Zip prints an empty Size for a
 * directory), and the entry simply keeps no size. */
static int parse_u64(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    size_t digits = 0;
    size_t i = 0;

    while (s[i] >= '0' && s[i] <= '9') {
        unsigned d = (unsigned)(s[i] - '0');

        if (v > (UINT64_MAX - d) / 10u)
            return 0;
        v = v * 10u + d;
        digits++;
        i++;
    }
    if (digits == 0)
        return 0;
    /* A CRLF listing leaves the carriage return on the value; nothing else
     * may follow the number. */
    while (s[i] == '\r' || s[i] == ' ' || s[i] == '\t')
        i++;
    if (s[i] != '\0')
        return 0;
    *out = v;
    return 1;
}

/* Chops one helper's output into NUL-terminated lines in place and indexes
 * the entries it names. text stays owned by the view: every stored name is a
 * pointer into it.
 *
 * For 7-Zip the interesting lines are the "Path = " properties of the -slt
 * record format. No ISO9660 or UDF name can carry the newline that would let
 * some other property forge one, so a line-oriented scan is exact rather than
 * a heuristic. */
int iso_listing_index(iso_view *v, char *text, iso_reader reader)
{
    size_t i = 0;

    if (v == NULL || text == NULL)
        return SGN_ERR_INTERNAL;

    while (text[i] != '\0') {
        char *line = text + i;
        size_t n;

        while (text[i] != '\0' && text[i] != '\n')
            i++;
        n = (size_t)(text + i - line);
        if (text[i] == '\n')
            text[i++] = '\0';

        if (reader == ISO_READER_7Z) {
            if (n >= sizeof(Z_SIZE_KEY) - 1u &&
                memcmp(line, Z_SIZE_KEY, sizeof(Z_SIZE_KEY) - 1u) == 0) {
                uint64_t sz = 0;
                iso_entry *last;

                /* Properties belong to the record above them, and only to one
                 * this pass just indexed: an entry the other reader supplied
                 * has no business acquiring a size from this listing. */
                if (v->n_entries == 0)
                    continue;
                last = &v->entries[v->n_entries - 1u];
                if (last->reader != reader || last->has_size)
                    continue;
                if (parse_u64(line + sizeof(Z_SIZE_KEY) - 1u, &sz)) {
                    last->size = sz;
                    last->has_size = 1;
                }
                continue;
            }
            if (n < sizeof(Z_PATH_KEY) - 1u ||
                memcmp(line, Z_PATH_KEY, sizeof(Z_PATH_KEY) - 1u) != 0)
                continue;
            line += sizeof(Z_PATH_KEY) - 1u;
            n -= sizeof(Z_PATH_KEY) - 1u;
        }
        line = normalise_entry(line, n);
        if (line == NULL)
            continue;
        if (entries_push(v, line, reader) != SGN_OK)
            return SGN_ERR_IO;
    }
    return SGN_OK;
}

/* ------------------------------------------------------------ open */

/* The 7-Zip binary goes by three names depending on how it was packaged, and
 * all three of the builds in circulation carry the UDF handler. Tried in the
 * order of how likely each is to be the full build.
 *
 * Resolved to a full path and run by it, never by bare name: 7-Zip loads its
 * format library (7z.so) from the directory in argv[0], so started as plain
 * "7z" it looks for ./7z.so in whatever the current directory is, loads no
 * formats, and answers "Cannot open the file as archive" for every image. A
 * host whose 7z is a shell wrapper that execs the real binary by full path
 * hides this; the appliance, which has no shell and stages the real binary,
 * does not (seen on hardware 2026-09-18). */
static const char *const g_zbins[] = { "7z", "7zz", "7za", NULL };

static const char *pick_zbin(char *buf, size_t cap)
{
    size_t i;

    for (i = 0; g_zbins[i] != NULL; i++) {
        if (sgn_which(g_zbins[i], buf, cap))
            return buf;
    }
    return NULL;
}

int iso_udf_reader_ok(char *which, size_t cap)
{
    char path[PATH_MAX];
    char *argv[3];
    char *out = NULL;
    size_t len = 0;
    int ok = 0;

    if (which != NULL && cap > 0u)
        which[0] = '\0';
    if (pick_zbin(path, sizeof(path)) == NULL)
        return 0;
    if (which != NULL && cap > 0u)
        (void)snprintf(which, cap, "%s", path);

    /* `7z i` lists the formats that loaded; the UDF handler is one row of
     * that table, its name standing alone between runs of spaces. */
    argv[0] = path;
    argv[1] = sgn_arg("i");
    argv[2] = NULL;
    if (sgn_run_capture(argv, &out, &len, Z_LISTING_MAX) == SGN_OK)
        ok = strstr(out, " Udf ") != NULL;
    free(out);
    return ok;
}

static iso_view *view_alloc(const char *path)
{
    iso_view *v = calloc(1, sizeof(*v));

    if (v == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory");
        return NULL;
    }
    if (path != NULL) {
        v->path = strdup(path);
        if (v->path == NULL) {
            free(v);
            sgn_set_error(SGN_ERR_IO, "out of memory");
            return NULL;
        }
    }
    return v;
}

int iso_view_open(const char *path, iso_view **out)
{
    iso_view *v;
    char *argv[10];
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

    v = view_alloc(path);
    if (v == NULL)
        return SGN_ERR_IO;

    /* ISO9660 and Rock Ridge. A failure here is not fatal on its own: an
     * image can legitimately carry nothing but a stub in this namespace, and
     * the UDF pass below is the one that will name its files. */
    argv[0] = sgn_arg("bsdtar");
    argv[1] = sgn_arg("-tf");
    argv[2] = v->path;
    argv[3] = NULL;
    if (sgn_run_capture(argv, &v->listing[ISO_READER_ISO9660], &len,
                        LISTING_MAX) == SGN_OK && len > 0) {
        rc = iso_listing_index(v, v->listing[ISO_READER_ISO9660],
                               ISO_READER_ISO9660);
        if (rc != SGN_OK) {
            iso_view_close(v);
            return rc;
        }
    } else {
        sgn_log(SGN_LOG_DEBUG, "bsdtar listed no ISO9660 tree in %s", path);
    }
    v->n_iso9660 = v->n_entries;

    /* UDF. Listed second so that anything both namespaces carry keeps the
     * bsdtar entry, and with it the extraction path that has always been
     * used for Linux media. */
    v->zbin = pick_zbin(v->zbin_path, sizeof(v->zbin_path));
    if (v->zbin == NULL) {
        sgn_log(SGN_LOG_WARN,
                "7z is not on PATH, so only this image's ISO9660 namespace "
                "can be read. UDF-only media (Windows install ISOs) looks "
                "empty; install p7zip or 7-Zip to read it.");
    } else {
        argv[0] = sgn_arg(v->zbin);
        argv[1] = sgn_arg("l");
        argv[2] = sgn_arg("-ba");   /* no banner, no summary: records only */
        argv[3] = sgn_arg("-slt");  /* one "key = value" per line */
        argv[4] = sgn_arg("-bd");   /* no progress indicator on stderr */
        argv[5] = sgn_arg("--");
        argv[6] = v->path;
        argv[7] = NULL;
        len = 0;
        if (sgn_run_capture(argv, &v->listing[ISO_READER_7Z], &len,
                            Z_LISTING_MAX) == SGN_OK && len > 0) {
            rc = iso_listing_index(v, v->listing[ISO_READER_7Z],
                                   ISO_READER_7Z);
            if (rc != SGN_OK) {
                iso_view_close(v);
                return rc;
            }
        } else {
            sgn_log(SGN_LOG_DEBUG, "%s listed nothing in %s", v->zbin, path);
        }
    }

    if (v->n_entries == 0) {
        iso_view_close(v);
        sgn_set_error(SGN_ERR_FORMAT,
                      "'%s' contains no files in either the ISO9660 or the "
                      "UDF namespace; not an ISO?", path);
        return SGN_ERR_FORMAT;
    }
    sgn_log(SGN_LOG_DEBUG, "listed %zu entries in %s (%zu ISO9660, %zu UDF)",
            v->n_entries, path, v->n_iso9660, v->n_entries - v->n_iso9660);
    *out = v;
    return SGN_OK;
}

int iso_view_from_listings(const char *iso9660, const char *udf,
                           iso_view **out)
{
    iso_view *v;
    int rc;

    if (out == NULL)
        return SGN_ERR_INTERNAL;
    *out = NULL;
    v = view_alloc(NULL);
    if (v == NULL)
        return SGN_ERR_IO;

    if (iso9660 != NULL) {
        v->listing[ISO_READER_ISO9660] = strdup(iso9660);
        if (v->listing[ISO_READER_ISO9660] == NULL) {
            iso_view_close(v);
            sgn_set_error(SGN_ERR_IO, "out of memory");
            return SGN_ERR_IO;
        }
        rc = iso_listing_index(v, v->listing[ISO_READER_ISO9660],
                               ISO_READER_ISO9660);
        if (rc != SGN_OK) {
            iso_view_close(v);
            return rc;
        }
    }
    v->n_iso9660 = v->n_entries;

    if (udf != NULL) {
        v->zbin = g_zbins[0];
        v->listing[ISO_READER_7Z] = strdup(udf);
        if (v->listing[ISO_READER_7Z] == NULL) {
            iso_view_close(v);
            sgn_set_error(SGN_ERR_IO, "out of memory");
            return SGN_ERR_IO;
        }
        rc = iso_listing_index(v, v->listing[ISO_READER_7Z], ISO_READER_7Z);
        if (rc != SGN_OK) {
            iso_view_close(v);
            return rc;
        }
    }
    *out = v;
    return SGN_OK;
}

void iso_view_close(iso_view *v)
{
    size_t i;

    if (v == NULL)
        return;
    for (i = 0; i < (size_t)ISO_READER_N; i++)
        free(v->listing[i]);
    free(v->entries);
    free(v->path);
    free(v);
}

const char *iso_view_udf_reader(const iso_view *v)
{
    return (v != NULL) ? v->zbin : NULL;
}

size_t iso_view_count(const iso_view *v, iso_reader reader)
{
    if (v == NULL)
        return 0;
    return (reader == ISO_READER_ISO9660) ? v->n_iso9660
                                          : v->n_entries - v->n_iso9660;
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
        const char *e = v->entries[i].name;

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

/* Which helper can extract this member. Every member handed out by
 * iso_view_find is a pointer into the entry table, so identity answers it
 * exactly; a pointer from anywhere else is a caller bug, and bsdtar is the
 * answer that preserves the behaviour this tool had before UDF existed. */
iso_reader iso_view_reader_of(const iso_view *v, const char *member)
{
    size_t i;

    if (v == NULL || member == NULL)
        return ISO_READER_ISO9660;
    for (i = 0; i < v->n_entries; i++) {
        if (v->entries[i].name == member)
            return v->entries[i].reader;
    }
    return ISO_READER_ISO9660;
}

int iso_view_size(const iso_view *v, const char *member, uint64_t *out)
{
    size_t i;

    if (out != NULL)
        *out = 0;
    if (v == NULL || member == NULL || out == NULL)
        return 0;
    for (i = 0; i < v->n_entries; i++) {
        if (v->entries[i].name != member)
            continue;
        if (!v->entries[i].has_size)
            return 0;
        *out = v->entries[i].size;
        return 1;
    }
    return 0;
}

/* Fills argv with the command that writes one member to stdout. 7-Zip needs
 * -spd so a member whose name happens to contain a glob character is matched
 * literally rather than as a pattern, and both readers are given "--" or an
 * option-terminating form so an image path starting with '-' stays a path. */
static void member_argv(iso_view *v, iso_reader reader, const char *member,
                        char *argv[10])
{
    size_t i = 0;

    if (reader == ISO_READER_7Z && v->zbin != NULL) {
        argv[i++] = sgn_arg(v->zbin);
        argv[i++] = sgn_arg("x");
        argv[i++] = sgn_arg("-so");
        argv[i++] = sgn_arg("-ba");
        argv[i++] = sgn_arg("-bd");
        argv[i++] = sgn_arg("-spd");
        argv[i++] = sgn_arg("--");
        argv[i++] = v->path;
        argv[i++] = sgn_arg(member);
    } else {
        argv[i++] = sgn_arg("bsdtar");
        argv[i++] = sgn_arg("-x");
        argv[i++] = sgn_arg("-O");
        argv[i++] = sgn_arg("-f");
        argv[i++] = v->path;
        argv[i++] = sgn_arg(member);
    }
    argv[i] = NULL;
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
    char *argv[10];
    char *data = NULL;
    size_t dlen = 0;
    size_t take;
    int rc;

    if (got != NULL)
        *got = 0;
    /* A view built from listings alone (iso_view_from_listings) has no image
     * behind it, so it answers layout questions and never content ones. An
     * unreadable fingerprint is a fingerprint that does not match, which is
     * exactly what family.h asks this callback to report. */
    if (v == NULL || v->path == NULL)
        return 1;
    if (!iso_view_find(v, path, &member) || member == NULL)
        return 1;

    member_argv(v, iso_view_reader_of(v, member), member, argv);
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
    char *argv[10];

    if (v == NULL || v->path == NULL || member == NULL || dst == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "iso_view_extract: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    member_argv(v, iso_view_reader_of(v, member), member, argv);
    return sgn_run_to_file(argv, dst);
}

/* ------------------------------------------------------ kernel paths */

/* Where each family keeps the kernel the bless wrap is built around.
 *
 * This table lives in the tool and not in falcon-core/src/media/family.c, on
 * purpose. The registry is the loader's data: it answers "what cmdline does
 * this family need", which the loader consumes at boot. Which file inside the
 * ISO holds the kernel is a question only the signing step ever asks, and
 * answering it in the registry would put a host-tooling concern into an EFI
 * module for no one's benefit.
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
