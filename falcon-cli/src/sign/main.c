/* falcon-boot-sign: bless an ISO (docs/DESIGN.md Sections 7.5, 8, 8.1).
 *
 * Flow, in the order the steps depend on each other:
 *
 *   1. index the ISO with falcon-core's ISO9660/UDF readers (no mount, no
 *      root, no helper process)
 *   2. fingerprint it through falcon-core/src/media/family.c, over that index
 *   3. find the family's kernel inside the ISO
 *   4. render the family's cmdline template with the ISO's path on p2
 *   5. extract the kernel, wrap it into a minimal UKI whose .cmdline is
 *      that string (uki.c, laid out as ukify lays it out)
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
 * Section 8.1), decrypt p3, or prompt for a passphrase. The private key is
 * a file path the caller hands it; keeping it encrypted at rest is the
 * caller's job (Section 7.5).
 */

#include <sign/sign.h>
#include <sign/manifestio.h>
#include <sign/payload.h>
#include <sign/uki.h>

#include <manifest/manifest.h>
#include <media/hookcmd.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CMDLINE_MAX 4096u

#define DEFAULT_STUB UKI_DEFAULT_STUB

#define BLESSED_DIR SGN_BLESSED_DIR

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
    int         hook;           /* wrap falcon-hook in with the CD entry */
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
    { "--no-hook",       0 },
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
"usage: falcon-boot-sign [bless] <iso> --stick <dir> --key <mok.key> --cert <mok.crt>\n"
"\n"
"Blesses an ISO for Secure Boot: wraps the ISO's kernel in a minimal UKI\n"
"with the cmdline and initrds of the ISO's own CD boot entry and\n"
"falcon-hook appended, which rebuilds the ISO as a device before the\n"
"distro looks for its media; signs that wrap with the MOK key, and records\n"
"it in the p2 manifest (docs/DESIGN.md Sections 6.4, 7.5, 8 and 8.1).\n"
"\n"
"A FreeBSD or memtest86+ ISO is blessed by signing its own EFI loader into\n"
"<stick>/" BLESSED_DIR " instead; the ISO is not modified.\n"
"\n"
"A .efi or a .img takes the bare-payload path instead (Section 7.2): it is\n"
"signed in place, which MODIFIES YOUR FILE. Only --key, --cert and --dry-run\n"
"apply there.\n"
"\n"
"  <iso>                 the ISO, .efi or .img to bless\n"
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
"                        UUID=1234-ABCD (default: UUID= and the filesystem\n"
"                        UUID of the device behind --stick)\n"
"  --cmdline <string>    use this cmdline verbatim instead of the family\n"
"                        template; required to bless an unknown family\n"
"  --kernel <path>       the kernel inside the ISO, overriding the per-family\n"
"                        table; required alongside --cmdline for a family the\n"
"                        table does not cover. A trailing * matches a suffix.\n"
"  --cmdline-extra <s>   added to the rendered cmdline, before a \"--\"\n"
"  --stub <file>         sd-stub to wrap with (default: " DEFAULT_STUB ")\n"
"  --no-initrd           thin wrap: kernel and cmdline only. The default\n"
"                        embeds the ISO's initramfs so the entire blessed\n"
"                        boot chain is signed (DESIGN Section 8.1); the\n"
"                        thin wrap trades that for the initramfs's size\n"
"                        per blessed ISO and reopens the unsigned-initrd\n"
"                        residual for this medium\n"
"  --no-hook             leave falcon-hook out and point the initramfs at\n"
"                        the ISO file instead: the ISO's loopback entry, or\n"
"                        the family's cmdline template. Needs the initramfs\n"
"                        to read p2's filesystem\n"
"  --work <dir>          scratch directory (default: $TMPDIR or /tmp)\n"
"  --dry-run             report what would be written and write nothing\n"
"  --verbose, --quiet    lower or raise the diagnostic threshold\n"
"  --help\n"
"\n"
"Known families and their cmdline templates:\n");
    for (i = 0; i < n; i++) {
        if (tab[i].id == FAMILY_UNKNOWN)
            continue;
        (void)fprintf(f, "  %-16s %-22s %s\n", sgn_family_slug(tab[i].id),
                      tab[i].name,
                      family_bless_loader(tab[i].id) != NULL
                          ? "(signs its own loader)"
                      : tab[i].cmdline_template != NULL
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
    o->hook = 1;
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
        } else if (strcmp(a, "--no-hook") == 0) {
            o->hook = 0;
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
    /* A bare payload needs no --stick: nothing is staged beside it and no
     * record is written, so there is no p2 for this tool to write into
     * (DESIGN Section 7.2, and payload.h on why there is no record). It is
     * still accepted, because the appliance passes it to every invocation. */
    if (o->stick == NULL &&
        sgn_payload_shape_of(o->iso) == SGN_PAYLOAD_NONE) {
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

/* Substitutes the two placeholders family.h documents. Anything else in the
 * template is copied through, so a template gaining a third placeholder
 * fails loudly at boot rather than silently here. */
static int render_cmdline(const char *tmpl, const char *iso_arg,
                          const char *dev, const char *iso_file, char *out,
                          size_t cap)
{
    char label[64];
    int have_label = 0;

    size_t w = 0;
    size_t i = 0;

    while (tmpl[i] != '\0') {
        const char *sub = NULL;
        size_t n;

        if (strncmp(tmpl + i, "%ISO%", 5) == 0) {
            sub = iso_arg;
            i += 5u;
        } else if (strncmp(tmpl + i, "%LABEL%", 7) == 0) {
            /* Read only when a template asks for it, and once. */
            if (!have_label) {
                int rc = iso_volume_label(iso_file, label, sizeof(label));

                if (rc != SGN_OK)
                    return rc;
                have_label = 1;
            }
            sub = label;
            i += 7u;
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

/* The key of one kernel argument: everything before its '=', or the whole
 * word. */
static size_t arg_key_len(const char *w, size_t n)
{
    size_t k = 0;

    while (k < n && w[k] != '=')
        k++;
    return k;
}

/* Appends each argument of frag to cmdline unless cmdline already carries an
 * argument with the same key. The family fragment is what finds the ISO as a
 * file; the ISO's own entry is what boots it, and where the two name the same
 * thing (Gentoo's root=live:CDLABEL=) the ISO's own word is the one kept. */
static int append_fragment(char *cmdline, size_t cap, const char *frag)
{
    const char *f = frag;

    while (*f != '\0') {
        const char *w;
        size_t wl, kl;
        const char *c = cmdline;
        int dup = 0;
        size_t have;

        while (*f == ' ')
            f++;
        w = f;
        while (*f != '\0' && *f != ' ')
            f++;
        wl = (size_t)(f - w);
        if (wl == 0)
            break;
        kl = arg_key_len(w, wl);
        while (*c != '\0') {
            const char *x;
            size_t xl;

            while (*c == ' ')
                c++;
            x = c;
            while (*c != '\0' && *c != ' ')
                c++;
            xl = (size_t)(c - x);
            if (xl > 0 && arg_key_len(x, xl) == kl && memcmp(x, w, kl) == 0)
                dup = 1;
        }
        if (dup)
            continue;
        have = strlen(cmdline);
        if (have + (have ? 1u : 0u) + wl + 1u > cap) {
            sgn_set_error(SGN_ERR_NOSPACE, "rendered cmdline exceeds %zu bytes",
                          cap - 1u);
            return SGN_ERR_NOSPACE;
        }
        if (have)
            cmdline[have++] = ' ';
        memcpy(cmdline + have, w, wl);
        cmdline[have + wl] = '\0';
    }
    return SGN_OK;
}

/* The ISO's boot configuration, parsed once per bless. Static: the result
 * is ~50 KB and the scratch 512 KB, neither of which belongs on a stack. */
static bootcfg_result g_cfg;
static char g_cfg_scratch[512u * 1024u];

/* The artifact's file name under BLESSED_DIR. ext is ".uki" for a wrap and
 * ".efi" for a signed loader, because what stands behind the ISO is not the
 * same kind of thing for every family. */
static int derive_uki_name(const options *o, char *out, size_t cap,
                           const char *ext)
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
        return sgn_field_copy(out, cap, o->name, "--name");
    }
    base = sgn_base_name(o->iso);
    n = strlen(base);
    if (n > 4u && strcmp(base + n - 4u, ".iso") == 0)
        n -= 4u;
    if (n + strlen(ext) + 1u > cap) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "the ISO's name is too long for a UKI name; use --name");
        return SGN_ERR_NOSPACE;
    }
    memcpy(out, base, n);
    memcpy(out + n, ext, strlen(ext) + 1u);
    return SGN_OK;
}

/* ------------------------------------------------------------ the bless */

/* The bare-payload half of the bless verb (DESIGN Section 7.2). Same verb and
 * same arguments, because it is the same act from the user's side: "make this
 * thing on my stick bootable with Secure Boot left on". What differs is where
 * the signature ends up, and that difference is exactly what the tool states
 * before it acts, since here it lands in the user's own file. */
static int bless_payload(const options *o)
{
    sgn_payload_opts p;

    memset(&p, 0, sizeof(p));
    p.path = o->iso;
    p.key = o->key;
    p.cert = o->cert;
    p.work = o->work;
    p.dry_run = o->dry_run;

    /* Arguments that only mean something for the wrap path. Rejected rather
     * than ignored: a user who passed --cmdline believes something about what
     * is going to be built, and nothing here builds it. */
    if (o->cmdline != NULL || o->cmdline_extra != NULL || o->kernel != NULL ||
        o->name != NULL || o->iso_path != NULL || o->dev != NULL) {
        sgn_set_error(SGN_ERR_ARG,
                      "'%s' is a bare EFI payload and is signed in place. "
                      "Drop --cmdline/--cmdline-extra/--kernel/--name/"
                      "--iso-path/--dev.", o->iso);
        return SGN_ERR_ARG;
    }
    return sgn_payload_bless(&p);
}

/* The hook, as make embeds it (falcon-hook/build.mk). */
extern const unsigned char falcon_hook_bin[];
extern const size_t falcon_hook_bin_len;

/* Pads the staged initrd to 4 bytes and appends the hook's archive. */
static int append_hook(const char *initrd)
{
    size_t cap = hookcmd_archive_size(falcon_hook_bin_len, 0), len = 0;
    uint8_t *buf;
    int rc;

    buf = cap != 0 ? malloc(cap) : NULL;
    if (buf == NULL) {
        sgn_set_error(SGN_ERR_NOSPACE, "no memory for the hook's archive");
        return SGN_ERR_NOSPACE;
    }
    /* No map: a wrap is built before the ISO's extents are known, and the
     * loader publishes them in FalconIsoMap at boot. */
    if (hookcmd_archive(falcon_hook_bin, falcon_hook_bin_len, NULL, 0, buf, cap,
                        &len) != HOOKCMD_OK) {
        free(buf);
        sgn_set_error(SGN_ERR_FORMAT, "the hook's archive could not be built");
        return SGN_ERR_FORMAT;
    }
    rc = sgn_pad4_file(initrd);
    if (rc == SGN_OK)
        rc = sgn_append_bytes(initrd, buf, len);
    free(buf);
    if (rc == SGN_OK)
        sgn_log(SGN_LOG_INFO, "appended falcon-hook (%zu bytes)", len);
    return rc;
}

/* ---- blessing a medium by signing its own loader ---- */

/* The family's loader (family_bless_loader), signed, is the artifact behind
 * the ISO; there is no wrap. A FreeBSD medium has no kernel-plus-initramfs
 * for a UKI: its /boot/loader.efi reads the kernel off the medium, which the
 * loader serves as a memory disk (falcon-efi/src/bsd; DESIGN Section 7.7 for
 * what the signature then covers). memtest86+ is one self-contained EFI
 * binary, \EFI\BOOT\BOOTX64.EFI, that reads nothing after it starts.
 */
static int bless_loader(const options *o, iso_view *view,
                        const family_desc *fam)
{
    const char *member;
    char efi_name[MANIFEST_NAME_MAX];
    char iso_arg[MANIFEST_PATH_MAX + 2u];
    char blessed_dir[PATH_MAX];
    char out_efi[PATH_MAX];
    char manifest_path[PATH_MAX];
    char work_dir[PATH_MAX];
    char work_loader[PATH_MAX];
    char hex[65];
    uint8_t digest[32];
    uint64_t size = 0;
    manifest_record rec;
    int replaced = 0;
    int rc;

    member = iso_loader_path(view, fam->id);
    if (member == NULL)
        return SGN_ERR_FORMAT;     /* iso_loader_path set the message */
    sgn_log(SGN_LOG_INFO, "loader: /%s", member);
    /* Said rather than refused, because the caller is usually a script that
     * passes the same arguments for every medium: nothing here builds a
     * cmdline. */
    if (o->cmdline_extra != NULL)
        sgn_log(SGN_LOG_INFO, "--cmdline-extra ignored: this medium takes no "
                              "kernel arguments from us");

    rc = sgn_derive_iso_path(o->iso, o->stick, o->iso_path, iso_arg,
                             sizeof(iso_arg));
    if (rc != SGN_OK)
        return rc;
    rc = derive_uki_name(o, efi_name, sizeof(efi_name), ".efi");
    if (rc != SGN_OK)
        return rc;

    if (snprintf(blessed_dir, sizeof(blessed_dir), "%s/" BLESSED_DIR,
                 o->stick) >= (int)sizeof(blessed_dir) ||
        snprintf(out_efi, sizeof(out_efi), "%s/%s", blessed_dir, efi_name)
            >= (int)sizeof(out_efi) ||
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest",
                 blessed_dir) >= (int)sizeof(manifest_path) ||
        snprintf(work_dir, sizeof(work_dir), "%s/falcon-sign-%ld", o->work,
                 (long)getpid()) >= (int)sizeof(work_dir) ||
        snprintf(work_loader, sizeof(work_loader), "%s/loader.efi", work_dir)
            >= (int)sizeof(work_loader)) {
        sgn_set_error(SGN_ERR_NOSPACE, "the staging paths are too long");
        return SGN_ERR_NOSPACE;
    }

    sgn_log(SGN_LOG_INFO, "hashing %s...", o->iso);
    rc = sgn_sha256_file(o->iso, digest, &size);
    if (rc != SGN_OK)
        return rc;
    sgn_hex32(digest, hex);

    memset(&rec, 0, sizeof(rec));
    rc = sgn_field_copy(rec.iso, sizeof(rec.iso), iso_arg + 1,
                        "the ISO's path");
    if (rc == SGN_OK)
        rc = sgn_field_copy(rec.uki, sizeof(rec.uki), efi_name,
                            "the artifact's name");
    if (rc == SGN_OK)
        rc = sgn_field_copy(rec.family, sizeof(rec.family),
                            sgn_family_slug(fam->id), "the family id");
    if (rc != SGN_OK)
        return rc;
    memcpy(rec.sha256, hex, sizeof(rec.sha256));
    rec.has_sha256 = 1;
    rec.size = size;

    if (!manifest_record_valid(&rec)) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "the record for this ISO cannot be written to the "
                      "manifest: its path or name carries a '#' or a "
                      "non-ASCII byte. Rename the file, or pass --iso-path "
                      "and --name.");
        return SGN_ERR_FORMAT;
    }

    if (o->dry_run) {
        (void)printf("would write %s\n", out_efi);
        (void)printf("would record in %s:\n", manifest_path);
        (void)printf("iso=%s\nsize=%llu\nsha256=%s\nuki=%s\nfamily=%s\n",
                     rec.iso, (unsigned long long)rec.size, rec.sha256,
                     rec.uki, rec.family);
        return SGN_OK;
    }

    rc = sgn_mkdir_p(blessed_dir);
    if (rc == SGN_OK)
        rc = sgn_mkdir_p(work_dir);
    if (rc != SGN_OK)
        return rc;

    sgn_log(SGN_LOG_INFO, "extracting the loader");
    rc = iso_view_extract(view, member, work_loader);
    if (rc != SGN_OK)
        goto cleanup;

    {
        char *argv[9];

        argv[0] = sgn_arg("sbsign");
        argv[1] = sgn_arg("--key");
        argv[2] = sgn_arg(o->key);
        argv[3] = sgn_arg("--cert");
        argv[4] = sgn_arg(o->cert);
        argv[5] = sgn_arg("--output");
        argv[6] = out_efi;
        argv[7] = work_loader;
        argv[8] = NULL;

        sgn_log(SGN_LOG_INFO, "sbsign");
        rc = sgn_run(argv);
        if (rc != SGN_OK)
            goto cleanup;
    }

    rc = sgn_manifest_update(manifest_path, &rec, &replaced);
    if (rc != SGN_OK)
        goto cleanup;

    (void)printf("blessed %s\n", rec.iso);
    (void)printf("  loader   %s\n", out_efi);
    (void)printf("  size     %llu\n", (unsigned long long)rec.size);
    (void)printf("  sha256   %s\n", rec.sha256);
    (void)printf("  manifest %s (%s)\n", manifest_path,
                 replaced ? "record replaced" : "record added");

cleanup:
    (void)unlink(work_loader);
    (void)rmdir(work_dir);
    return rc;
}

static int bless(const options *o)
{
    iso_view *view = NULL;
    family_probe probe;
    const family_desc *fam;
    const char *kernel_member = NULL;
    const char *initrd_member = NULL;
    /* What the ISO's own boot configuration says to boot, when it says:
     * the entry, and whether it is the file-boot (loopback) kind. */
    const bootcfg_entry *entry = NULL;
    int entry_loop = 0;
    /* The CD entry, booted through falcon-hook: the loader publishes the
     * ISO's extents at boot and the hook rebuilds the CD from them. Needs
     * the initrd in the wrap and the entry's own kernel. */
    int hooked = 0;
    bootcfg_src cfg;
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

    /* The one helper, up front: a missing sbsign three minutes into hashing
     * a 5 GB ISO is a worse message than a missing sbsign now. The ISO is
     * read in process (isoprobe.c), so nothing else is needed. */
    if (!sgn_have_program("sbsign")) {
        sgn_set_error(SGN_ERR_EXEC,
                      "need sbsign on PATH (it signs the UKI)");
        return SGN_ERR_EXEC;
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
    sgn_log(SGN_LOG_INFO, "family: %s (%s)", fam->name, sgn_family_slug(fam->id));

    /* A medium blessed by signing its own loader never needs sd-stub and
     * takes none of what follows. --cmdline or --kernel means the user is
     * describing a Linux wrap, and is left to the path that builds one. */
    if (family_bless_loader(fam->id) != NULL && o->cmdline == NULL &&
        o->kernel == NULL) {
        rc = bless_loader(o, view, fam);
        goto out;
    }

    /* Checked here rather than with sbsign above because only a wrap needs
     * it, and the family is what says whether this is one. */
    if (!sgn_file_exists(o->stub)) {
        sgn_set_error(SGN_ERR_IO,
                      "sd-stub not found at '%s'; install systemd-boot or "
                      "pass --stub", o->stub);
        rc = SGN_ERR_IO;
        goto out;
    }

    /* The ISO's own configuration (falcon-core/src/media/bootcfg.c): a
     * loopback entry boots it as a file with the distro's own arguments;
     * failing that, the CD entry is the base the family's fragment is
     * added to. Neither is consulted under --cmdline, which is the user
     * saying they know better. */
    if (o->cmdline == NULL) {
        iso_view_bootcfg_src(view, &cfg);
        if (o->hook && o->embed_initrd && o->kernel == NULL &&
            bootcfg_read(&cfg, BOOTCFG_CD, g_cfg_scratch,
                         sizeof(g_cfg_scratch), &g_cfg) == BOOTCFG_OK) {
            entry = &g_cfg.entry[g_cfg.pick];
            hooked = 1;
        } else if (bootcfg_read(&cfg, BOOTCFG_LOOP, g_cfg_scratch,
                                sizeof(g_cfg_scratch), &g_cfg) == BOOTCFG_OK) {
            entry = &g_cfg.entry[g_cfg.pick];
            entry_loop = 1;
        } else if (fam->cmdline_template != NULL &&
                   bootcfg_read(&cfg, BOOTCFG_CD, g_cfg_scratch,
                                sizeof(g_cfg_scratch), &g_cfg) == BOOTCFG_OK) {
            entry = &g_cfg.entry[g_cfg.pick];
        }
        if (entry != NULL)
            sgn_log(SGN_LOG_INFO, "boot entry: '%s' from %s%s", entry->title,
                    g_cfg.source,
                    hooked       ? " (boots through falcon-hook)"
                    : entry_loop ? " (boots as a file)" : "");
    }

    if (fam->id == FAMILY_UNKNOWN && o->cmdline == NULL && !entry_loop &&
        !hooked) {
        size_t count = 0;
        const family_desc *tab = family_table(&count);
        char known[512];
        size_t w = 0;
        size_t i;

        for (i = 0; i < count; i++) {
            int k;

            if (tab[i].id == FAMILY_UNKNOWN)
                continue;
            k = snprintf(known + w, sizeof(known) - w, "%s%s",
                         w > 0 ? ", " : "", sgn_family_slug(tab[i].id));
            if (k < 0 || (size_t)k >= sizeof(known) - w)
                break;
            w += (size_t)k;
        }
        sgn_set_error(SGN_ERR_FORMAT,
                      "this ISO matches no known family, so there is no "
                      "cmdline template to derive its boot arguments from. "
                      "Known families: %s. Pass --cmdline '<arguments>' and "
                      "--kernel <path-in-iso> to bless it anyway with "
                      "arguments you supply.", known);
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
    } else if (entry != NULL) {
        if (!iso_view_find(view, entry->kernel, &kernel_member)) {
            sgn_set_error(SGN_ERR_FORMAT, "the ISO's boot entry names kernel "
                          "%s, which is not in the ISO", entry->kernel);
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

    rc = sgn_derive_iso_path(o->iso, o->stick, o->iso_path, iso_arg,
                             sizeof(iso_arg));
    if (rc != SGN_OK)
        goto out;
    rc = derive_uki_name(o, uki_name, sizeof(uki_name), ".uki");
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
        rc = render_cmdline(o->cmdline, iso_arg, dev, o->iso, cmdline,
                            sizeof(cmdline));
    } else if (hooked) {
        /* The CD entry's own arguments and what the hook adds to them
         * (falcon-core/src/media/hookcmd.c), the same cmdline a direct boot
         * from the loader gets. */
        char base[CMDLINE_MAX];

        if (bootcfg_render(entry, NULL, base, sizeof(base)) != BOOTCFG_OK ||
            hookcmd_build(base, cmdline, sizeof(cmdline)) != HOOKCMD_OK) {
            sgn_set_error(SGN_ERR_NOSPACE, "the ISO's boot entry has a "
                          "cmdline longer than %zu bytes", sizeof(cmdline) - 1u);
            rc = SGN_ERR_NOSPACE;
        } else {
            rc = SGN_OK;
        }
    } else if (entry != NULL) {
        /* The entry's own arguments, ${iso_path} filled in for a loopback
         * entry and dropped for a CD one; then, for a CD entry, the
         * family's fragment for finding the file. */
        if (bootcfg_render(entry, entry_loop ? iso_arg : NULL, cmdline,
                           sizeof(cmdline)) != BOOTCFG_OK) {
            sgn_set_error(SGN_ERR_NOSPACE, "the ISO's boot entry has a "
                          "cmdline longer than %zu bytes", sizeof(cmdline) - 1u);
            rc = SGN_ERR_NOSPACE;
        } else if (!entry_loop) {
            char frag[CMDLINE_MAX];

            rc = render_cmdline(fam->cmdline_template, iso_arg, dev, o->iso,
                                frag, sizeof(frag));
            if (rc == SGN_OK)
                rc = append_fragment(cmdline, sizeof(cmdline), frag);
        } else {
            rc = SGN_OK;
        }
    } else if (fam->cmdline_template != NULL) {
        rc = render_cmdline(fam->cmdline_template, iso_arg, dev, o->iso,
                            cmdline, sizeof(cmdline));
    } else {
        sgn_set_error(SGN_ERR_FORMAT,
                      "family '%s' has no cmdline template and the ISO has "
                      "no boot entry to take one from. Pass --cmdline.",
                      fam->name);
        rc = SGN_ERR_FORMAT;
        goto out;
    }
    if (rc != SGN_OK)
        goto out;

    if (o->cmdline_extra != NULL &&
        sgn_cmdline_insert(cmdline, sizeof(cmdline), o->cmdline_extra) != SGN_OK) {
        sgn_set_error(SGN_ERR_NOSPACE, "cmdline too long with --cmdline-extra");
        rc = SGN_ERR_NOSPACE;
        goto out;
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
    rc = sgn_field_copy(rec.iso, sizeof(rec.iso), iso_arg + 1, "the ISO's path");
    if (rc != SGN_OK)
        goto out;
    rc = sgn_field_copy(rec.uki, sizeof(rec.uki), uki_name, "the UKI's name");
    if (rc != SGN_OK)
        goto out;
    rc = sgn_field_copy(rec.family, sizeof(rec.family), sgn_family_slug(fam->id),
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

    if (o->embed_initrd && entry != NULL && o->kernel == NULL &&
        (entry->n_initrd > 0 || hooked)) {
        /* Every initrd the entry loads, in its order, one after the other,
         * each padded to 4 bytes as a bootloader concatenating them does:
         * the kernel unpacks concatenated cpio archives in sequence, which
         * is how a bootloader passes microcode ahead of the initramfs, and
         * the wrap has one .initrd section to hold them. The hook's own
         * archive goes last. */
        uint8_t i;

        if (entry->n_initrd == 0) {
            rc = sgn_append_bytes(work_initrd, "", 0);
            if (rc != SGN_OK)
                goto cleanup;
        }
        for (i = 0; i < entry->n_initrd; i++) {
            char piece[PATH_MAX];

            if (!iso_view_find(view, entry->initrd[i], &initrd_member)) {
                sgn_set_error(SGN_ERR_FORMAT, "the ISO's boot entry names "
                              "initrd %s, which is not in the ISO",
                              entry->initrd[i]);
                rc = SGN_ERR_FORMAT;
                goto cleanup;
            }
            sgn_log(SGN_LOG_INFO, "extracting initramfs /%s", initrd_member);
            if (i == 0) {
                rc = iso_view_extract(view, initrd_member, work_initrd);
            } else {
                if (snprintf(piece, sizeof(piece), "%s.%u", work_initrd,
                             (unsigned)i) >= (int)sizeof(piece)) {
                    sgn_set_error(SGN_ERR_NOSPACE, "staging path too long");
                    rc = SGN_ERR_NOSPACE;
                    goto cleanup;
                }
                rc = iso_view_extract(view, initrd_member, piece);
                if (rc == SGN_OK)
                    rc = sgn_pad4_file(work_initrd);
                if (rc == SGN_OK)
                    rc = sgn_append_file(work_initrd, piece);
                (void)unlink(piece);
            }
            if (rc != SGN_OK)
                goto cleanup;
        }
        if (hooked) {
            rc = append_hook(work_initrd);
            if (rc != SGN_OK)
                goto cleanup;
        }
    } else if (o->embed_initrd) {
        initrd_member = iso_initrd_path(view, fam->id, kernel_member);
        if (initrd_member == NULL) {
            rc = SGN_ERR_FORMAT;    /* iso_initrd_path set the message */
            goto cleanup;
        }
        sgn_log(SGN_LOG_INFO, "extracting initramfs /%s", initrd_member);
        rc = iso_view_extract(view, initrd_member, work_initrd);
        if (rc != SGN_OK)
            goto cleanup;
    }

    {
        uki_spec spec;

        memset(&spec, 0, sizeof(spec));
        spec.stub = o->stub;
        spec.kernel = work_kernel;
        spec.initrd = o->embed_initrd ? work_initrd : NULL;
        spec.cmdline = cmdline;
        spec.osrel = UKI_BLESSED_OSREL;
        spec.out = work_uki;

        sgn_log(SGN_LOG_INFO, "building the UKI");
        rc = uki_build(&spec);
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

    rc = sgn_manifest_update(manifest_path, &rec, &replaced);
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

/* Operation dispatch. The verb is optional because the tool had a
 * bare-positional shape before any other verb existed: `falcon-boot-sign
 * x.iso ...` has always meant bless, and scripts and the appliance both spell
 * it that way, so an absent verb keeps meaning that. A verb is recognised only
 * in the first position, which is what keeps an ISO that happens to be named
 * "uki" from being one. */
typedef struct {
    const char *name;
    int (*run)(int argc, char **argv);
} sgn_command;

static const sgn_command g_commands[] = {
    { "uki",     sgn_uki_main },
};

int main(int argc, char **argv)
{
    options o;
    size_t i;
    int rc;

    if (argc < 2) {
        usage(stderr);
        return 1;
    }

    for (i = 0; i < sizeof(g_commands) / sizeof(g_commands[0]); i++) {
        if (strcmp(argv[1], g_commands[i].name) != 0)
            continue;
        return (g_commands[i].run(argc - 1, argv + 1) == SGN_OK) ? 0 : 1;
    }
    /* The default operation may also be named outright, so a caller that
     * wants to be explicit can be, and so `bless` never becomes a filename. */
    if (strcmp(argv[1], "bless") == 0) {
        argc--;
        argv++;
        if (argc < 2) {
            usage(stderr);
            return 1;
        }
    }

    rc = parse_args(argc, argv, &o);
    if (rc != SGN_OK) {
        sgn_log(SGN_LOG_ERROR, "%s", sgn_error());
        usage(stderr);
        return 1;
    }
    sgn_log_set_level(o.verbose ? SGN_LOG_DEBUG
                                : (o.quiet ? SGN_LOG_WARN : SGN_LOG_INFO));

    rc = (sgn_payload_shape_of(o.iso) != SGN_PAYLOAD_NONE) ? bless_payload(&o)
                                                           : bless(&o);
    if (rc != SGN_OK) {
        sgn_log(SGN_LOG_ERROR, "%s", sgn_error());
        return 1;
    }
    return 0;
}
