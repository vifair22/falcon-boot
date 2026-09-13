/* falcon-boot-sign: bless an ISO (docs/DESIGN.md Sections 7.5, 8, 8.1).
 *
 * Flow, in the order the steps depend on each other:
 *
 *   1. list the ISO with bsdtar and 7-Zip, one namespace each (no mount,
 *      no root)
 *   2. fingerprint it through src/family/family.c, over that listing
 *   3. find the family's kernel inside the ISO
 *   4. render the family's cmdline template with the ISO's path on p2
 *   5. extract the kernel, wrap it with ukify into a minimal UKI whose
 *      .cmdline is that string
 *   6. sign the wrap with sbsign and the caller's MOK key
 *   7. hash the ISO and upsert its manifest record
 *
 * Step 5 is the one that carries the design's weight. The cmdline lives
 * inside the signed image, so an attacker with write access to p2 cannot
 * inject kernel arguments into a blessed boot: the kernel and its arguments
 * verify together or the boot refuses (Section 8.1). That is also why the
 * tool refuses to guess a cmdline it cannot derive; a wrong one would be
 * signed, and re-blessing is the only way to take it back.
 *
 * What this tool never does: sign anything itself (it shells out to sbsign,
 * Section 8.1), decrypt p3, prompt for a passphrase, or relax any
 * enforcement anywhere. The private key is a file path the caller hands it,
 * and keeping that file encrypted at rest is the caller's job: on the
 * on-stick appliance it is cryptsetup that has just opened p3, and on a host
 * it is whatever the user's own key hygiene is (Section 7.5).
 */

#include <cli/sign/sign.h>

#include <manifest/manifest.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CMDLINE_MAX 4096u
#define MANIFEST_RECORDS 512u
/* A manifest with 512 records at the field caps of manifest.h; the renderer
 * reports what it would have needed if this is ever too small. */
#define MANIFEST_TEXT_MAX (1u << 20)

/* ukify's own default, named here so --stub documents what it overrides and
 * so a missing systemd-boot is one clear message instead of a ukify traceback. */
#define DEFAULT_STUB "/usr/lib/systemd/boot/efi/linuxx64.efi.stub"

#define BLESSED_DIR "falcon/blessed"

/* Short registry ids for the manifest's family= field. DESIGN Section 8.1
 * spells the Arch one "arch", and the loader will match on these strings, so
 * they are a wire format: add rows, never rename them. They live here rather
 * than in src/family/family.h because nothing in the loader needs them until
 * the blessed boot path reads the manifest, at which point this table is
 * what that path should be taught from. */
typedef struct {
    family_id   id;
    const char *slug;
} family_slug;

static const family_slug g_slugs[] = {
    { FAMILY_UNKNOWN,       "unknown" },
    { FAMILY_DEBIAN_CASPER, "casper" },
    { FAMILY_DEBIAN_LIVE,   "debian-live" },
    { FAMILY_FEDORA_DRACUT, "fedora" },
    { FAMILY_ARCH,          "arch" },
    { FAMILY_SYSTEMRESCUE,  "systemrescue" },
    { FAMILY_MEMTEST,       "memtest" },
    { FAMILY_WINDOWS,       "windows" },
};

static const char *slug_for(family_id id)
{
    size_t i;

    for (i = 0; i < sizeof(g_slugs) / sizeof(g_slugs[0]); i++) {
        if (g_slugs[i].id == id)
            return g_slugs[i].slug;
    }
    return "unknown";
}

/* ------------------------------------------------------------ options */

typedef struct {
    const char *iso;
    const char *stick;
    const char *key;
    const char *cert;
    const char *name;
    const char *cmdline;        /* replaces the family template entirely */
    const char *cmdline_extra;  /* appended to whatever was rendered */
    const char *dev;            /* %DEV% substitution */
    const char *iso_path;       /* %ISO% substitution, relative to p2 root */
    const char *kernel;         /* member to wrap, overriding the table */
    const char *stub;
    const char *work;
    int         embed_initrd;
    int         dry_run;
    int         verbose;
    int         quiet;
} options;

/* Whitelist. Only these reach the parser, and every one of them is either a
 * flag or takes exactly one value; anything else is a usage error rather
 * than something quietly ignored. */
typedef struct {
    const char *name;
    int         takes_value;
} opt_spec;

static const opt_spec g_opts[] = {
    { "--stick",         1 },
    { "--key",           1 },
    { "--cert",          1 },
    { "--name",          1 },
    { "--cmdline",       1 },
    { "--cmdline-extra", 1 },
    { "--dev",           1 },
    { "--iso-path",      1 },
    { "--kernel",        1 },
    { "--stub",          1 },
    { "--work",          1 },
    { "--no-initrd",     0 },
    { "--dry-run",       0 },
    { "--verbose",       0 },
    { "--quiet",         0 },
    { "--help",          0 },
};

static void usage(FILE *f)
{
    size_t n = 0;
    const family_desc *tab = family_table(&n);
    size_t i;

    (void)fprintf(f,
"usage: falcon-boot-sign <iso> --stick <dir> --key <mok.key> --cert <mok.crt>\n"
"\n"
"Blesses an ISO for Secure Boot: wraps the ISO's kernel in a minimal UKI\n"
"whose embedded cmdline points the distro's own initramfs back at the ISO\n"
"file on p2, signs that wrap with the MOK key, and records it in the p2\n"
"manifest (docs/DESIGN.md Sections 7.5, 8 and 8.1).\n"
"\n"
"  <iso>                 the ISO to bless\n"
"  --stick <dir>         p2's mount point (or any directory to stage into)\n"
"  --key <file>          MOK private key, PEM\n"
"  --cert <file>         MOK certificate, PEM\n"
"\n"
"  --name <file.uki>     output name under <stick>/" BLESSED_DIR "\n"
"                        (default: the ISO's name with .uki in place of .iso)\n"
"  --iso-path <path>     the ISO's path relative to the p2 root, as the\n"
"                        booted kernel will see it (default: derived from\n"
"                        where the ISO sits under --stick)\n"
"  --dev <token>         what the family's cmdline should name p2 as, e.g.\n"
"                        UUID=1234-ABCD or PARTLABEL=falcon-isos (default:\n"
"                        the filesystem UUID of the device behind --stick)\n"
"  --cmdline <string>    use this cmdline verbatim instead of the family\n"
"                        template; required to bless an unknown family\n"
"  --kernel <path>       the kernel inside the ISO, overriding the per-family\n"
"                        table; required alongside --cmdline for a family the\n"
"                        table does not cover. A trailing * matches a suffix.\n"
"  --cmdline-extra <s>   appended to the rendered cmdline\n"
"  --stub <file>         sd-stub to wrap with (default: " DEFAULT_STUB ")\n"
"  --no-initrd           thin wrap: kernel and cmdline only. The default\n"
"                        embeds the ISO's initramfs so the entire blessed\n"
"                        boot chain is signed (DESIGN Section 8.1); the\n"
"                        thin wrap trades that for the initramfs's size\n"
"                        per blessed ISO and reopens the unsigned-initrd\n"
"                        residual for this medium\n"
"  --work <dir>          scratch directory (default: $TMPDIR or /tmp)\n"
"  --dry-run             report what would be written and write nothing\n"
"  --verbose, --quiet    lower or raise the diagnostic threshold\n"
"  --help\n"
"\n"
"Known families and their cmdline templates:\n");
    for (i = 0; i < n; i++) {
        if (tab[i].id == FAMILY_UNKNOWN)
            continue;
        (void)fprintf(f, "  %-14s %-22s %s\n", slug_for(tab[i].id),
                      tab[i].name,
                      tab[i].cmdline_template != NULL
                          ? tab[i].cmdline_template : "(no cmdline needed)");
    }
}

static int parse_args(int argc, char **argv, options *o)
{
    int i;

    memset(o, 0, sizeof(*o));
    /* Embedded initrd is the default (DESIGN Section 8.1): the whole blessed
     * chain signed, --no-initrd as the space-over-coverage escape. */
    o->embed_initrd = 1;
    o->stub = DEFAULT_STUB;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        size_t s;
        const opt_spec *spec = NULL;

        if (a[0] != '-') {
            if (o->iso != NULL) {
                sgn_set_error(SGN_ERR_ARG,
                              "more than one ISO given ('%s' and '%s'); this "
                              "tool blesses one at a time", o->iso, a);
                return SGN_ERR_ARG;
            }
            o->iso = a;
            continue;
        }
        for (s = 0; s < sizeof(g_opts) / sizeof(g_opts[0]); s++) {
            if (strcmp(a, g_opts[s].name) == 0) {
                spec = &g_opts[s];
                break;
            }
        }
        if (spec == NULL) {
            sgn_set_error(SGN_ERR_ARG, "unknown option '%s'", a);
            return SGN_ERR_ARG;
        }
        if (spec->takes_value && i + 1 >= argc) {
            sgn_set_error(SGN_ERR_ARG, "'%s' needs a value", a);
            return SGN_ERR_ARG;
        }

        if (strcmp(a, "--help") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(a, "--stick") == 0) {
            o->stick = argv[++i];
        } else if (strcmp(a, "--key") == 0) {
            o->key = argv[++i];
        } else if (strcmp(a, "--cert") == 0) {
            o->cert = argv[++i];
        } else if (strcmp(a, "--name") == 0) {
            o->name = argv[++i];
        } else if (strcmp(a, "--cmdline") == 0) {
            o->cmdline = argv[++i];
        } else if (strcmp(a, "--cmdline-extra") == 0) {
            o->cmdline_extra = argv[++i];
        } else if (strcmp(a, "--dev") == 0) {
            o->dev = argv[++i];
        } else if (strcmp(a, "--iso-path") == 0) {
            o->iso_path = argv[++i];
        } else if (strcmp(a, "--kernel") == 0) {
            o->kernel = argv[++i];
        } else if (strcmp(a, "--stub") == 0) {
            o->stub = argv[++i];
        } else if (strcmp(a, "--work") == 0) {
            o->work = argv[++i];
        } else if (strcmp(a, "--no-initrd") == 0) {
            o->embed_initrd = 0;
        } else if (strcmp(a, "--dry-run") == 0) {
            o->dry_run = 1;
        } else if (strcmp(a, "--verbose") == 0) {
            o->verbose = 1;
        } else if (strcmp(a, "--quiet") == 0) {
            o->quiet = 1;
        }
    }

    if (o->iso == NULL) {
        sgn_set_error(SGN_ERR_ARG, "no ISO given");
        return SGN_ERR_ARG;
    }
    if (o->stick == NULL) {
        sgn_set_error(SGN_ERR_ARG, "--stick is required");
        return SGN_ERR_ARG;
    }
    /* Checked even under --dry-run: a dry run whose only difference from the
     * real thing is that it never noticed the key is missing is not a
     * rehearsal of anything. */
    if (o->key == NULL || o->cert == NULL) {
        sgn_set_error(SGN_ERR_ARG, "--key and --cert are required");
        return SGN_ERR_ARG;
    }
    if (o->work == NULL) {
        o->work = getenv("TMPDIR");
        if (o->work == NULL || o->work[0] == '\0')
            o->work = "/tmp";
    }
    return SGN_OK;
}

/* ------------------------------------------------------------ helpers */

static const char *base_name(const char *p)
{
    const char *slash = strrchr(p, '/');

    return (slash != NULL) ? slash + 1 : p;
}

static int field_copy(char *dst, size_t cap, const char *src, const char *what)
{
    size_t n = strlen(src);

    if (n + 1u > cap) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "%s is %zu bytes, which does not fit the manifest's "
                      "%zu-byte field: shorten it or use --name",
                      what, n, cap - 1u);
        return SGN_ERR_NOSPACE;
    }
    memcpy(dst, src, n + 1u);
    return SGN_OK;
}

/* Substitutes the two placeholders family.h documents. Anything else in the
 * template is copied through, so a template gaining a third placeholder
 * fails loudly at boot rather than silently here. */
static int render_cmdline(const char *tmpl, const char *iso_arg,
                          const char *dev, char *out, size_t cap)
{
    size_t w = 0;
    size_t i = 0;

    while (tmpl[i] != '\0') {
        const char *sub = NULL;
        size_t n;

        if (strncmp(tmpl + i, "%ISO%", 5) == 0) {
            sub = iso_arg;
            i += 5u;
        } else if (strncmp(tmpl + i, "%DEV%", 5) == 0) {
            if (dev == NULL) {
                sgn_set_error(SGN_ERR_ARG,
                              "this family's cmdline needs to name the "
                              "partition the ISO lives on, and it could not "
                              "be derived from --stick: pass --dev, e.g. "
                              "--dev UUID=1234-ABCD or --dev PARTLABEL=...");
                return SGN_ERR_ARG;
            }
            sub = dev;
            i += 5u;
        } else {
            if (w + 1u >= cap)
                break;
            out[w++] = tmpl[i++];
            continue;
        }

        n = strlen(sub);
        if (w + n + 1u > cap)
            break;
        memcpy(out + w, sub, n);
        w += n;
    }
    if (tmpl[i] != '\0') {
        sgn_set_error(SGN_ERR_NOSPACE, "rendered cmdline exceeds %zu bytes",
                      cap - 1u);
        return SGN_ERR_NOSPACE;
    }
    out[w] = '\0';
    return SGN_OK;
}

/* The ISO's path as the booted kernel will see it: relative to the p2 root,
 * with a leading slash. Derived from where the file sits under --stick,
 * because that is the only place it can be at boot; an ISO outside the stick
 * is a copy the user still has to make, and saying so beats embedding a path
 * that will not resolve into something signed. */
static int derive_iso_path(const options *o, char *out, size_t cap)
{
    char iso_real[PATH_MAX];
    char stick_real[PATH_MAX];
    size_t slen;

    if (o->iso_path != NULL) {
        const char *p = o->iso_path;

        while (*p == '/')
            p++;
        if (snprintf(out, cap, "/%s", p) >= (int)cap) {
            sgn_set_error(SGN_ERR_NOSPACE, "--iso-path is too long");
            return SGN_ERR_NOSPACE;
        }
        return SGN_OK;
    }

    if (realpath(o->iso, iso_real) == NULL) {
        sgn_set_error_errno(SGN_ERR_IO, "realpath '%s'", o->iso);
        return SGN_ERR_IO;
    }
    if (realpath(o->stick, stick_real) == NULL) {
        sgn_set_error_errno(SGN_ERR_IO, "realpath '%s'", o->stick);
        return SGN_ERR_IO;
    }
    slen = strlen(stick_real);
    if (slen > 0 && stick_real[slen - 1u] == '/')
        slen--;

    if (strncmp(iso_real, stick_real, slen) == 0 && iso_real[slen] == '/') {
        if (snprintf(out, cap, "%s", iso_real + slen) >= (int)cap) {
            sgn_set_error(SGN_ERR_NOSPACE, "the ISO's path on p2 is too long");
            return SGN_ERR_NOSPACE;
        }
        return SGN_OK;
    }

    sgn_log(SGN_LOG_WARN,
            "the ISO is not under --stick, so its path on p2 is a guess: "
            "assuming /%s. Copy it there, or pass --iso-path.",
            base_name(o->iso));
    if (snprintf(out, cap, "/%s", base_name(o->iso)) >= (int)cap) {
        sgn_set_error(SGN_ERR_NOSPACE, "the ISO's name is too long");
        return SGN_ERR_NOSPACE;
    }
    return SGN_OK;
}

static int derive_uki_name(const options *o, char *out, size_t cap)
{
    const char *base;
    size_t n;

    if (o->name != NULL) {
        if (strchr(o->name, '/') != NULL) {
            sgn_set_error(SGN_ERR_ARG,
                          "--name is a file name under " BLESSED_DIR
                          ", not a path: '%s'", o->name);
            return SGN_ERR_ARG;
        }
        return field_copy(out, cap, o->name, "--name");
    }
    base = base_name(o->iso);
    n = strlen(base);
    if (n > 4u && strcmp(base + n - 4u, ".iso") == 0)
        n -= 4u;
    if (n + 5u > cap) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "the ISO's name is too long for a UKI name; use --name");
        return SGN_ERR_NOSPACE;
    }
    memcpy(out, base, n);
    memcpy(out + n, ".uki", 5u);
    return SGN_OK;
}

/* ------------------------------------------------------------ manifest */

static int manifest_update(const char *manifest_path,
                           const manifest_record *rec, int *replaced)
{
    manifest_record *recs = NULL;
    char *text = NULL;
    char *rendered = NULL;
    size_t len = 0;
    size_t n = 0;
    size_t used = 0;
    int rc;

    recs = calloc(MANIFEST_RECORDS, sizeof(*recs));
    rendered = malloc(MANIFEST_TEXT_MAX);
    if (recs == NULL || rendered == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory building the manifest");
        rc = SGN_ERR_IO;
        goto out;
    }

    /* An absent manifest is the first bless on this stick, not an error;
     * a corrupt one degrades to the records that did parse, which is the
     * same rule the loader follows (DESIGN Section 8.1). */
    if (sgn_file_exists(manifest_path)) {
        rc = sgn_read_file(manifest_path, &text, &len, MANIFEST_TEXT_MAX);
        if (rc != SGN_OK)
            goto out;
        rc = manifest_parse(text, len, recs, MANIFEST_RECORDS, &n);
        if (rc == MANIFEST_ERR_NOSPACE) {
            sgn_set_error(SGN_ERR_NOSPACE,
                          "'%s' holds %zu records, more than this build's "
                          "limit of %u", manifest_path, n,
                          (unsigned)MANIFEST_RECORDS);
            rc = SGN_ERR_NOSPACE;
            goto out;
        }
        if (rc != MANIFEST_OK) {
            sgn_set_error(SGN_ERR_INTERNAL, "manifest parse rejected its "
                          "own arguments");
            rc = SGN_ERR_INTERNAL;
            goto out;
        }
        sgn_log(SGN_LOG_DEBUG, "manifest: %zu existing record(s)", n);
    }

    *replaced = (manifest_find(recs, n, rec->iso) != NULL);

    rc = manifest_upsert(recs, &n, MANIFEST_RECORDS, rec);
    if (rc != MANIFEST_OK) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "could not add the record to the manifest (%d)", rc);
        rc = SGN_ERR_NOSPACE;
        goto out;
    }
    rc = manifest_render(recs, n, rendered, MANIFEST_TEXT_MAX, &used);
    if (rc != MANIFEST_OK) {
        sgn_set_error(SGN_ERR_NOSPACE, "the manifest would need %zu bytes",
                      used);
        rc = SGN_ERR_NOSPACE;
        goto out;
    }
    rc = sgn_write_file_atomic(manifest_path, rendered, used);

out:
    free(rendered);
    free(text);
    free(recs);
    return rc;
}

/* ------------------------------------------------------------ the bless */

static int bless(const options *o)
{
    iso_view *view = NULL;
    family_probe probe;
    const family_desc *fam;
    const char *kernel_member = NULL;
    const char *initrd_member = NULL;
    char iso_arg[MANIFEST_PATH_MAX + 2u];
    char uki_name[MANIFEST_NAME_MAX];
    char cmdline[CMDLINE_MAX];
    char dev_buf[128];
    char uuid[64];
    const char *dev = o->dev;
    char blessed_dir[PATH_MAX];
    char out_uki[PATH_MAX];
    char manifest_path[PATH_MAX];
    char work_dir[PATH_MAX];
    char work_kernel[PATH_MAX];
    char work_initrd[PATH_MAX];
    char work_uki[PATH_MAX];
    char hex[65];
    uint8_t digest[32];
    uint64_t size = 0;
    manifest_record rec;
    int replaced = 0;
    int rc;

    /* Every helper up front: a missing ukify three minutes into hashing a
     * 5 GB ISO is a worse message than a missing ukify now. 7z is not in this
     * list because it is a soft dependency: without it Linux media still
     * blesses, and iso_view_open warns about the UDF namespace it could not
     * read rather than refusing the whole run. */
    if (!sgn_have_program("bsdtar") || !sgn_have_program("ukify") ||
        !sgn_have_program("sbsign")) {
        sgn_set_error(SGN_ERR_EXEC,
                      "need bsdtar, ukify and sbsign on PATH (bsdtar reads "
                      "the ISO, ukify builds the UKI, sbsign signs it)");
        return SGN_ERR_EXEC;
    }
    if (!sgn_file_exists(o->stub)) {
        sgn_set_error(SGN_ERR_IO,
                      "sd-stub not found at '%s'; install systemd-boot or "
                      "pass --stub", o->stub);
        return SGN_ERR_IO;
    }
    if (!sgn_file_exists(o->key) || !sgn_file_exists(o->cert)) {
        sgn_set_error(SGN_ERR_IO, "MOK key or certificate not found ('%s', "
                      "'%s')", o->key, o->cert);
        return SGN_ERR_IO;
    }

    rc = iso_view_open(o->iso, &view);
    if (rc != SGN_OK)
        return rc;

    iso_view_family_probe(view, &probe);
    fam = family_detect(&probe);
    sgn_log(SGN_LOG_INFO, "family: %s (%s)", fam->name, slug_for(fam->id));

    if (fam->id == FAMILY_UNKNOWN && o->cmdline == NULL) {
        size_t count = 0;
        const family_desc *tab = family_table(&count);
        char known[512];
        size_t w = 0;
        size_t i;
        /* An image whose files are all in UDF lists as very nearly empty
         * without a reader for that namespace, and "matches no known family"
         * would then be true of the listing and false of the medium. Say
         * which it is. */
        const char *udf_hint =
            (iso_view_udf_reader(view) == NULL)
                ? " Note that 7z is not on PATH, so this ISO's UDF namespace "
                  "was not read; Windows install media looks empty without it."
                : "";

        for (i = 0; i < count; i++) {
            int k;

            if (tab[i].id == FAMILY_UNKNOWN)
                continue;
            k = snprintf(known + w, sizeof(known) - w, "%s%s",
                         w > 0 ? ", " : "", slug_for(tab[i].id));
            if (k < 0 || (size_t)k >= sizeof(known) - w)
                break;
            w += (size_t)k;
        }
        sgn_set_error(SGN_ERR_FORMAT,
                      "this ISO matches no known family, so there is no "
                      "cmdline template to derive its boot arguments from. "
                      "Known families: %s. Pass --cmdline '<arguments>' and "
                      "--kernel <path-in-iso> to bless it anyway with "
                      "arguments you supply.%s", known, udf_hint);
        rc = SGN_ERR_FORMAT;
        goto out;
    }

    if (o->kernel != NULL) {
        if (!iso_view_find(view, o->kernel, &kernel_member)) {
            sgn_set_error(SGN_ERR_FORMAT,
                          "--kernel '%s' is not in this ISO", o->kernel);
            rc = SGN_ERR_FORMAT;
            goto out;
        }
    } else {
        kernel_member = iso_kernel_path(view, fam->id);
        if (kernel_member == NULL) {
            rc = SGN_ERR_FORMAT;    /* iso_kernel_path set the message */
            goto out;
        }
    }
    sgn_log(SGN_LOG_INFO, "kernel: /%s", kernel_member);

    /* ---- the cmdline ---- */

    rc = derive_iso_path(o, iso_arg, sizeof(iso_arg));
    if (rc != SGN_OK)
        goto out;
    rc = derive_uki_name(o, uki_name, sizeof(uki_name));
    if (rc != SGN_OK)
        goto out;

    if (dev == NULL) {
        if (sgn_fs_uuid_for_path(o->stick, uuid, sizeof(uuid)) == SGN_OK) {
            if (snprintf(dev_buf, sizeof(dev_buf), "UUID=%s", uuid) <
                (int)sizeof(dev_buf)) {
                dev = dev_buf;
                sgn_log(SGN_LOG_INFO, "p2 resolved to %s", dev);
            }
        } else {
            /* Not fatal here: only a template that actually uses %DEV%
             * needs it, and render_cmdline says so precisely if it does. */
            sgn_log(SGN_LOG_DEBUG, "no filesystem UUID for --stick: %s",
                    sgn_error());
        }
    }

    if (o->cmdline != NULL) {
        rc = render_cmdline(o->cmdline, iso_arg, dev, cmdline,
                            sizeof(cmdline));
    } else if (fam->cmdline_template != NULL) {
        rc = render_cmdline(fam->cmdline_template, iso_arg, dev, cmdline,
                            sizeof(cmdline));
    } else {
        sgn_set_error(SGN_ERR_FORMAT,
                      "family '%s' has no cmdline template: it needs no "
                      "kernel arguments, which means it needs no blessing "
                      "either. Pass --cmdline to override.", fam->name);
        rc = SGN_ERR_FORMAT;
        goto out;
    }
    if (rc != SGN_OK)
        goto out;

    if (o->cmdline_extra != NULL) {
        size_t have = strlen(cmdline);

        if (snprintf(cmdline + have, sizeof(cmdline) - have, " %s",
                     o->cmdline_extra) >= (int)(sizeof(cmdline) - have)) {
            sgn_set_error(SGN_ERR_NOSPACE, "cmdline too long with --cmdline-extra");
            rc = SGN_ERR_NOSPACE;
            goto out;
        }
    }
    sgn_log(SGN_LOG_INFO, "cmdline: %s", cmdline);

    /* ---- paths ---- */

    if (snprintf(blessed_dir, sizeof(blessed_dir), "%s/" BLESSED_DIR,
                 o->stick) >= (int)sizeof(blessed_dir) ||
        snprintf(out_uki, sizeof(out_uki), "%s/%s", blessed_dir, uki_name)
            >= (int)sizeof(out_uki) ||
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest",
                 blessed_dir) >= (int)sizeof(manifest_path) ||
        snprintf(work_dir, sizeof(work_dir), "%s/falcon-sign-%ld", o->work,
                 (long)getpid()) >= (int)sizeof(work_dir) ||
        snprintf(work_kernel, sizeof(work_kernel), "%s/vmlinuz", work_dir)
            >= (int)sizeof(work_kernel) ||
        snprintf(work_initrd, sizeof(work_initrd), "%s/initrd", work_dir)
            >= (int)sizeof(work_initrd) ||
        snprintf(work_uki, sizeof(work_uki), "%s/unsigned.uki", work_dir)
            >= (int)sizeof(work_uki)) {
        sgn_set_error(SGN_ERR_NOSPACE, "the staging paths are too long");
        rc = SGN_ERR_NOSPACE;
        goto out;
    }

    /* ---- the ISO's identity ---- */

    sgn_log(SGN_LOG_INFO, "hashing %s...", o->iso);
    rc = sgn_sha256_file(o->iso, digest, &size);
    if (rc != SGN_OK)
        goto out;
    sgn_hex32(digest, hex);

    memset(&rec, 0, sizeof(rec));
    rc = field_copy(rec.iso, sizeof(rec.iso), iso_arg + 1, "the ISO's path");
    if (rc != SGN_OK)
        goto out;
    rc = field_copy(rec.uki, sizeof(rec.uki), uki_name, "the UKI's name");
    if (rc != SGN_OK)
        goto out;
    rc = field_copy(rec.family, sizeof(rec.family), slug_for(fam->id),
                    "the family id");
    if (rc != SGN_OK)
        goto out;
    memcpy(rec.sha256, hex, sizeof(rec.sha256));
    rec.has_sha256 = 1;
    rec.size = size;

    if (!manifest_record_valid(&rec)) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "the record for this ISO cannot be written to the "
                      "manifest: its path or name carries a '#' or a "
                      "non-ASCII byte. Rename the file, or pass --iso-path "
                      "and --name.");
        rc = SGN_ERR_FORMAT;
        goto out;
    }

    if (o->dry_run) {
        (void)printf("would write %s\n", out_uki);
        (void)printf("would record in %s:\n", manifest_path);
        (void)printf("iso=%s\nsize=%llu\nsha256=%s\nuki=%s\nfamily=%s\n",
                     rec.iso, (unsigned long long)rec.size, rec.sha256,
                     rec.uki, rec.family);
        (void)printf("cmdline=%s\n", cmdline);
        rc = SGN_OK;
        goto out;
    }

    /* ---- build, sign, record ---- */

    rc = sgn_mkdir_p(blessed_dir);
    if (rc != SGN_OK)
        goto out;
    rc = sgn_mkdir_p(work_dir);
    if (rc != SGN_OK)
        goto out;

    sgn_log(SGN_LOG_INFO, "extracting kernel");
    rc = iso_view_extract(view, kernel_member, work_kernel);
    if (rc != SGN_OK)
        goto cleanup;

    if (o->embed_initrd) {
        char guess[PATH_MAX];
        const char *slash = strrchr(kernel_member, '/');
        size_t dirlen = (slash != NULL) ? (size_t)(slash - kernel_member) : 0;

        /* The initramfs sits beside the kernel in every family in the table;
         * the name varies, so this is a prefix match in that directory. */
        if (snprintf(guess, sizeof(guess), "/%.*s/initr*", (int)dirlen,
                     kernel_member) >= (int)sizeof(guess)) {
            sgn_set_error(SGN_ERR_NOSPACE, "initramfs path too long");
            rc = SGN_ERR_NOSPACE;
            goto cleanup;
        }
        if (!iso_view_find(view, guess, &initrd_member)) {
            sgn_set_error(SGN_ERR_FORMAT,
                          "--initrd given but no initramfs found next to the "
                          "kernel in the ISO");
            rc = SGN_ERR_FORMAT;
            goto cleanup;
        }
        sgn_log(SGN_LOG_INFO, "extracting initramfs /%s", initrd_member);
        rc = iso_view_extract(view, initrd_member, work_initrd);
        if (rc != SGN_OK)
            goto cleanup;
    }

    {
        char a_linux[PATH_MAX + 16];
        char a_stub[PATH_MAX + 16];
        char a_out[PATH_MAX + 16];
        char a_initrd[PATH_MAX + 16];
        char *a_cmdline;
        char *argv[9];
        size_t n = 0;

        a_cmdline = malloc(strlen(cmdline) + 16u);
        if (a_cmdline == NULL) {
            sgn_set_error(SGN_ERR_IO, "out of memory");
            rc = SGN_ERR_IO;
            goto cleanup;
        }
        (void)sprintf(a_cmdline, "--cmdline=%s", cmdline);
        (void)snprintf(a_linux, sizeof(a_linux), "--linux=%s", work_kernel);
        (void)snprintf(a_stub, sizeof(a_stub), "--stub=%s", o->stub);
        (void)snprintf(a_out, sizeof(a_out), "--output=%s", work_uki);
        (void)snprintf(a_initrd, sizeof(a_initrd), "--initrd=%s", work_initrd);

        argv[n++] = sgn_arg("ukify");
        argv[n++] = sgn_arg("build");
        argv[n++] = a_linux;
        argv[n++] = a_cmdline;
        argv[n++] = a_stub;
        if (o->embed_initrd)
            argv[n++] = a_initrd;
        argv[n++] = a_out;
        argv[n] = NULL;

        sgn_log(SGN_LOG_INFO, "ukify build");
        rc = sgn_run(argv);
        free(a_cmdline);
        if (rc != SGN_OK)
            goto cleanup;
    }

    {
        char *argv[9];

        argv[0] = sgn_arg("sbsign");
        argv[1] = sgn_arg("--key");
        argv[2] = sgn_arg(o->key);
        argv[3] = sgn_arg("--cert");
        argv[4] = sgn_arg(o->cert);
        argv[5] = sgn_arg("--output");
        argv[6] = out_uki;
        argv[7] = work_uki;
        argv[8] = NULL;

        sgn_log(SGN_LOG_INFO, "sbsign");
        rc = sgn_run(argv);
        if (rc != SGN_OK)
            goto cleanup;
    }

    rc = manifest_update(manifest_path, &rec, &replaced);
    if (rc != SGN_OK)
        goto cleanup;

    (void)printf("blessed %s\n", rec.iso);
    (void)printf("  uki      %s\n", out_uki);
    (void)printf("  cmdline  %s\n", cmdline);
    (void)printf("  size     %llu\n", (unsigned long long)rec.size);
    (void)printf("  sha256   %s\n", rec.sha256);
    (void)printf("  manifest %s (%s)\n", manifest_path,
                 replaced ? "record replaced" : "record added");

cleanup:
    (void)unlink(work_kernel);
    (void)unlink(work_initrd);
    (void)unlink(work_uki);
    (void)rmdir(work_dir);
out:
    iso_view_close(view);
    return rc;
}

int main(int argc, char **argv)
{
    options o;
    int rc;

    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    rc = parse_args(argc, argv, &o);
    if (rc != SGN_OK) {
        sgn_log(SGN_LOG_ERROR, "%s", sgn_error());
        usage(stderr);
        return 1;
    }
    sgn_log_set_level(o.verbose ? SGN_LOG_DEBUG
                                : (o.quiet ? SGN_LOG_WARN : SGN_LOG_INFO));

    rc = bless(&o);
    if (rc != SGN_OK) {
        sgn_log(SGN_LOG_ERROR, "%s", sgn_error());
        return 1;
    }
    return 0;
}
