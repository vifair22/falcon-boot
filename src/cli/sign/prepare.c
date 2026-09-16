/* falcon-boot-sign prepare: Windows install media (DESIGN Sections 6.3, 6.3.1
 * and 8.1).
 *
 * Flow, in the order the steps depend on each other:
 *
 *   1. decide whether there is anything to do at all, from the manifest and
 *      the ISO's size alone (no hashing, no listing, no subprocess)
 *   2. list the ISO and fingerprint it; refuse anything but Windows media
 *   3. take \sources\boot.wim's length from the ISO's own directory entry
 *   4. hash the ISO: the digest names the sidecar folder (Section 6.3.1)
 *   5. extract boot.wim into a staging directory beside its destination
 *   6. locate its extent in the ISO, and PROVE it by comparing every byte of
 *      the range against the extracted copy
 *   7. patch image <boot index> of the WIM: our winjump.exe over
 *      winpeshl.exe, Microsoft's preserved as winpeshl_orig.exe, and the
 *      config that tells winjump which file on p2 to attach
 *   8. optimize, refuse if the result no longer fits the extent, zero-pad
 *      back to the extent's exact length, verify
 *   9. rename the staging directory into place and write the manifest record
 *
 * The user's ISO is opened read-only and never written. That is the whole
 * difference between this and tools/win-spike/patch-winjump.sh, which proved
 * the mechanism by overlaying the ISO in place: the bytes WinPE ends up
 * reading are identical, but here they are served from a sidecar through the
 * loader's override layer instead.
 *
 * Nothing here signs anything, holds a key or prompts for a passphrase, and
 * that is by design rather than by omission: a Windows ISO needs no blessing
 * to boot under Secure Boot ON, because every component the platform verifies
 * is already Microsoft-signed (Section 6.3). Our patch fixes media-finding and
 * touches nothing that is verified.
 *
 * Memory and scratch. The WIM is never held in memory: it is extracted with a
 * subprocess writing to a file, scanned for and compared through fixed
 * buffers in src/cli/sign/wimext.c, and padded with ftruncate. Peak RSS of
 * this process is a few megabytes of static buffers plus the ISO listing; the
 * real costs are wimlib's, which DESIGN Section 8 records as 23-106 MB RSS and
 * about 1.2 GB of scratch. The staging directory sits on the destination
 * filesystem on purpose, so the finished 570 MB sidecar is renamed into place
 * rather than copied, and a failed run takes the whole directory with it.
 */

#include <cli/sign/sign.h>
#include <cli/sign/prepare.h>
#include <cli/sign/prepcore.h>
#include <cli/sign/manifestio.h>
#include <cli/sign/wimext.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* wimlib's driver. The separately installed wimupdate/wimoptimize/wimverify
 * names are aliases for its subcommands, so depending on the one binary keeps
 * the missing-dependency message to a single name. */
#define WIMLIB "wimlib-imagex"

/* Where \sources\boot.wim lives, written in the case Microsoft's UDF
 * namespace uses. iso_view_find folds case on both sides, so this is
 * documentation rather than a constraint. */
#define BOOTWIM_MEMBER "/sources/boot.wim"

#define CFG_MAX 1024u

/* ------------------------------------------------------------ options */

typedef struct {
    const char *iso;
    const char *stick;
    const char *iso_path;   /* overrides where on p2 the ISO is assumed to be */
    const char *winjump;    /* the WinPE-side binary to inject */
    const char *image;      /* WIM image index; NULL means the WIM's own */
    int         recompress;
    int         force;
    int         no_verify;
    int         dry_run;
    int         verbose;
    int         quiet;
} options;

typedef struct {
    const char *name;
    int         takes_value;
} opt_spec;

static const opt_spec g_opts[] = {
    { "--stick",      1 },
    { "--iso-path",   1 },
    { "--winjump",    1 },
    { "--image",      1 },
    { "--recompress", 0 },
    { "--force",      0 },
    { "--no-verify",  0 },
    { "--dry-run",    0 },
    { "--verbose",    0 },
    { "--quiet",      0 },
    { "--help",       0 },
};

void sgn_prepare_usage(FILE *f)
{
    (void)fprintf(f,
"usage: falcon-boot-sign prepare <iso> --stick <dir>\n"
"\n"
"Prepares a Windows install ISO so that Setup can find its own media after\n"
"the virtual CD disappears at ExitBootServices (docs/DESIGN.md Sections 6.3\n"
"and 6.3.1). The ISO itself is never modified: the patched \\sources\\boot.wim\n"
"is staged under <stick>/" PREP_DIR "/<sha256-of-iso>/ and the manifest\n"
"records the byte range it replaces.\n"
"\n"
"Signs nothing and needs no key, so it works on a --p3-plain stick too.\n"
"\n"
"  <iso>                 the Windows install ISO to prepare\n"
"  --stick <dir>         p2's mount point (or any directory to stage into)\n"
"\n"
"  --iso-path <path>     the ISO's path relative to the p2 root, as WinPE\n"
"                        will look for it (default: derived from where the\n"
"                        ISO sits under --stick)\n"
"  --winjump <file>      the WinPE jump binary to inject (default: winjump.exe\n"
"                        beside this tool, then build/bin/winjump.exe; build\n"
"                        it with `make winjump`)\n"
"  --image <n>           WIM image to patch (default: the boot index the WIM's\n"
"                        own header names, which is 2 on Windows 11 media)\n"
"  --recompress          full LZX repack instead of a plain optimize. Slower,\n"
"                        and reclaims several megabytes more; needed only when\n"
"                        the patched WIM no longer fits its original extent\n"
"  --force               re-prepare even when the manifest says this ISO is\n"
"                        already prepared\n"
"  --no-verify           skip the wimverify pass over the padded result\n"
"  --dry-run             report what would be written and write nothing\n"
"  --verbose, --quiet    lower or raise the diagnostic threshold\n"
"  --help\n");
}

static int parse_args(int argc, char **argv, options *o)
{
    int i;

    memset(o, 0, sizeof(*o));

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        size_t s;
        const opt_spec *spec = NULL;

        if (a[0] != '-') {
            if (o->iso != NULL) {
                sgn_set_error(SGN_ERR_ARG,
                              "more than one ISO given ('%s' and '%s'); this "
                              "tool prepares one at a time", o->iso, a);
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
            sgn_set_error(SGN_ERR_ARG, "unknown option '%s' for prepare", a);
            return SGN_ERR_ARG;
        }
        if (spec->takes_value && i + 1 >= argc) {
            sgn_set_error(SGN_ERR_ARG, "'%s' needs a value", a);
            return SGN_ERR_ARG;
        }

        if (strcmp(a, "--help") == 0) {
            sgn_prepare_usage(stdout);
            exit(0);
        } else if (strcmp(a, "--stick") == 0) {
            o->stick = argv[++i];
        } else if (strcmp(a, "--iso-path") == 0) {
            o->iso_path = argv[++i];
        } else if (strcmp(a, "--winjump") == 0) {
            o->winjump = argv[++i];
        } else if (strcmp(a, "--image") == 0) {
            o->image = argv[++i];
        } else if (strcmp(a, "--recompress") == 0) {
            o->recompress = 1;
        } else if (strcmp(a, "--force") == 0) {
            o->force = 1;
        } else if (strcmp(a, "--no-verify") == 0) {
            o->no_verify = 1;
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
    return SGN_OK;
}

/* ------------------------------------------------------- the jump binary */

/* Where to look for winjump.exe when the caller did not say. Beside this
 * binary first, because that is how the signing appliance ships the pair
 * (DESIGN Section 6.3.1) and how an installed tool would be laid out; then
 * the repo's own build tree, which is where a developer's copy is. */
static int find_winjump(const options *o, char *out, size_t cap)
{
    static const char *const fallbacks[] = {
        "build/bin/winjump.exe",
        "/usr/lib/falcon-boot/winjump.exe",
        NULL
    };
    char self[PATH_MAX];
    ssize_t n;
    size_t i;

    if (o->winjump != NULL) {
        if (!sgn_file_exists(o->winjump)) {
            sgn_set_error(SGN_ERR_IO, "no such jump binary: '%s'",
                          o->winjump);
            return SGN_ERR_IO;
        }
        if (snprintf(out, cap, "%s", o->winjump) >= (int)cap) {
            sgn_set_error(SGN_ERR_NOSPACE, "--winjump is too long");
            return SGN_ERR_NOSPACE;
        }
        return SGN_OK;
    }

    n = readlink("/proc/self/exe", self, sizeof(self) - 1u);
    if (n > 0 && (size_t)n < sizeof(self)) {
        char *slash;

        self[n] = '\0';
        slash = strrchr(self, '/');
        if (slash != NULL) {
            *slash = '\0';
            if (snprintf(out, cap, "%s/winjump.exe", self) < (int)cap &&
                sgn_file_exists(out))
                return SGN_OK;
        }
    }
    for (i = 0; fallbacks[i] != NULL; i++) {
        if (snprintf(out, cap, "%s", fallbacks[i]) < (int)cap &&
            sgn_file_exists(out))
            return SGN_OK;
    }

    sgn_set_error(SGN_ERR_IO,
                  "the WinPE jump binary was not found. Build it with "
                  "`make winjump`, or pass --winjump <path to winjump.exe>.");
    return SGN_ERR_IO;
}

/* ---------------------------------------------------- file read callback */

/* The medium behind a wimext_read_fn: an open file descriptor. pread rather
 * than seek-and-read so the two callbacks of a comparison cannot disturb each
 * other's position, and so nothing here keeps state worth getting wrong. */
typedef struct {
    int         fd;
    const char *path;
} file_src;

static int file_read(void *ctx, uint64_t off, void *buf, size_t len,
                     size_t *got)
{
    file_src *s = ctx;
    uint8_t *p = buf;
    size_t done = 0;

    if (got != NULL)
        *got = 0;
    while (done < len) {
        uint64_t at = off + (uint64_t)done;
        ssize_t n = pread(s->fd, p + done, len - done, (off_t)at);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            sgn_set_error_errno(SGN_ERR_IO, "read '%s' at %llu", s->path,
                                (unsigned long long)at);
            return 1;
        }
        if (n == 0)
            break;              /* end of file: a short read, not a failure */
        done += (size_t)n;
    }
    if (got != NULL)
        *got = done;
    return 0;
}

/* ------------------------------------------------------------ the steps */

/* Establishes where boot.wim lives in the ISO and proves it is one unbroken,
 * sector-aligned run there. The proof is a full-length comparison against the
 * copy the ISO reader extracted: the scan only produces a candidate, and a
 * candidate that matched a header but not the body would be a fragmented file
 * whose first fragment we would happily serve and whose remainder Windows
 * would read as garbage. */
static int locate_and_prove(const char *iso, uint64_t iso_size,
                            const char *wim, uint64_t wim_len,
                            uint64_t *out_off, wim_header *out_hdr)
{
    file_src iso_src;
    file_src wim_src;
    static uint8_t head[WIMEXT_HEAD_MAX];
    uint64_t off = 0;
    uint64_t diff = 0;
    size_t got = 0;
    int rc;

    iso_src.fd = -1;
    iso_src.path = iso;
    wim_src.fd = -1;
    wim_src.path = wim;

    iso_src.fd = open(iso, O_RDONLY | O_CLOEXEC);
    if (iso_src.fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "open '%s'", iso);
        return SGN_ERR_IO;
    }
    wim_src.fd = open(wim, O_RDONLY | O_CLOEXEC);
    if (wim_src.fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "open '%s'", wim);
        (void)close(iso_src.fd);
        return SGN_ERR_IO;
    }

    /* The extracted copy must itself be a WIM of exactly the length the ISO's
     * directory entry claimed, or the two things we are about to match are
     * not the same object and no offset would be right. Its head then becomes
     * the needle: a header alone would also describe the install.wim next to
     * it, which is why the spike matched 4 KB of the real file. */
    if (file_read(&wim_src, 0, head, sizeof(head), &got) != 0) {
        rc = SGN_ERR_IO;
        goto out;
    }
    rc = wim_header_parse(head, got, wim_len, out_hdr);
    if (rc != SGN_OK)
        goto out;

    sgn_log(SGN_LOG_INFO, "scanning for the boot.wim extent");
    rc = wimext_locate(file_read, &iso_src, iso_size, head, got, wim_len,
                       &off);
    if (rc != SGN_OK)
        goto out;
    if (off % WIMEXT_ALIGN != 0) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "boot.wim starts at %llu, which is not a %u-byte sector "
                      "boundary", (unsigned long long)off,
                      (unsigned)WIMEXT_ALIGN);
        rc = SGN_ERR_FORMAT;
        goto out;
    }
    sgn_log(SGN_LOG_INFO, "candidate extent at %llu; proving it byte for byte",
            (unsigned long long)off);

    rc = wimext_compare(file_read, &iso_src, off, file_read, &wim_src, 0,
                        wim_len, &diff);
    if (rc != SGN_OK) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "the ISO's bytes at %llu stop matching boot.wim after "
                      "%llu of %llu bytes, so the file is not stored as a "
                      "single contiguous extent. The override layer serves "
                      "one byte range per splice, so this ISO cannot be "
                      "prepared by this tool; rebuild the image with oscdimg "
                      "or xorriso so that \\sources\\boot.wim is contiguous.",
                      (unsigned long long)off, (unsigned long long)diff,
                      (unsigned long long)wim_len);
        rc = SGN_ERR_FORMAT;
        goto out;
    }

    *out_off = off;
    rc = SGN_OK;

out:
    (void)close(wim_src.fd);
    (void)close(iso_src.fd);
    return rc;
}

/* The three-entry injection of DESIGN Section 6.3, as one wimupdate run.
 *
 * wimupdate takes its command list on stdin and nowhere else, so the list is
 * written to a file in the staging directory and handed over as the child's
 * stdin. An `add` onto an existing path replaces it in place, which is how
 * Microsoft's winpeshl.exe is displaced and how ours takes its name. */
static int patch_wim(const char *wim, const char *image, const char *staging,
                     const char *winjump, const char *iso_rel,
                     uint64_t iso_size)
{
    char orig_dir[PATH_MAX];
    char orig_exe[PATH_MAX];
    char cfg_path[PATH_MAX];
    char cmd_path[PATH_MAX];
    char cfg[CFG_MAX];
    char cmds[4u * PATH_MAX];
    size_t cfg_len = 0;
    int rc;
    int n;

    if (snprintf(orig_dir, sizeof(orig_dir), "%s/orig", staging) >=
            (int)sizeof(orig_dir) ||
        snprintf(orig_exe, sizeof(orig_exe), "%s/" PREP_SHELL, orig_dir) >=
            (int)sizeof(orig_exe) ||
        snprintf(cfg_path, sizeof(cfg_path), "%s/" PREP_CFG_NAME, staging) >=
            (int)sizeof(cfg_path) ||
        snprintf(cmd_path, sizeof(cmd_path), "%s/update.txt", staging) >=
            (int)sizeof(cmd_path)) {
        sgn_set_error(SGN_ERR_NOSPACE, "the staging paths are too long");
        return SGN_ERR_NOSPACE;
    }

    /* Microsoft's own shell, saved before it is overwritten. It is the thing
     * winjump hands control back to, so losing it means WinPE comes up with
     * no Setup at all. */
    rc = sgn_mkdir_p(orig_dir);
    if (rc != SGN_OK)
        return rc;
    {
        char dest[PATH_MAX + 16];
        char *argv[10];
        size_t i = 0;

        if (snprintf(dest, sizeof(dest), "--dest-dir=%s", orig_dir) >=
            (int)sizeof(dest)) {
            sgn_set_error(SGN_ERR_NOSPACE, "the staging paths are too long");
            return SGN_ERR_NOSPACE;
        }
        argv[i++] = sgn_arg(WIMLIB);
        argv[i++] = sgn_arg("extract");
        argv[i++] = sgn_arg(wim);
        argv[i++] = sgn_arg(image);
        argv[i++] = sgn_arg(PREP_SYSTEM32 "/" PREP_SHELL);
        argv[i++] = dest;
        argv[i++] = sgn_arg("--no-acls");
        argv[i] = NULL;

        sgn_log(SGN_LOG_INFO, "preserving the original " PREP_SHELL
                " from image %s", image);
        rc = sgn_run(argv);
        if (rc != SGN_OK)
            return rc;
    }
    if (!sgn_file_exists(orig_exe)) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "image %s of this boot.wim has no " PREP_SYSTEM32 "/"
                      PREP_SHELL ". Pass --image with the index of the WinPE "
                      "image that actually boots (`wiminfo` lists them).",
                      image);
        return SGN_ERR_FORMAT;
    }

    rc = prep_cfg_render(iso_rel, iso_size, cfg, sizeof(cfg), &cfg_len);
    if (rc != SGN_OK)
        return rc;
    rc = sgn_write_file_atomic(cfg_path, cfg, cfg_len);
    if (rc != SGN_OK)
        return rc;

    n = snprintf(cmds, sizeof(cmds),
                 "add %s %s/%s\n"
                 "add %s %s/%s\n"
                 "add %s %s/%s\n",
                 orig_exe, PREP_SYSTEM32, PREP_SHELL_ORIG,
                 winjump,  PREP_SYSTEM32, PREP_SHELL,
                 cfg_path, PREP_SYSTEM32, PREP_CFG_NAME);
    if (n < 0 || (size_t)n >= sizeof(cmds)) {
        sgn_set_error(SGN_ERR_NOSPACE, "the update command list is too long");
        return SGN_ERR_NOSPACE;
    }
    rc = sgn_write_file_atomic(cmd_path, cmds, (size_t)n);
    if (rc != SGN_OK)
        return rc;

    {
        char *argv[8];
        size_t i = 0;

        argv[i++] = sgn_arg(WIMLIB);
        argv[i++] = sgn_arg("update");
        argv[i++] = sgn_arg(wim);
        argv[i++] = sgn_arg(image);
        argv[i++] = sgn_arg("--no-acls");
        argv[i] = NULL;

        sgn_log(SGN_LOG_INFO, "injecting winjump into image %s", image);
        rc = sgn_run_stdin(argv, cmd_path);
        if (rc != SGN_OK)
            return rc;
    }

    /* The staged sidecar must hold the patched WIM and nothing else: the
     * loader resolves splice names inside this folder, and a stray
     * update.txt there would be a file nothing can explain later. */
    (void)unlink(cmd_path);
    (void)unlink(cfg_path);
    return sgn_rm_rf(orig_dir);
}

/* ------------------------------------------------------------ the flow */

static int prepare(const options *o)
{
    iso_view *view = NULL;
    family_probe probe;
    const family_desc *fam;
    const manifest_record *found = NULL;
    manifest_record *recs = NULL;
    manifest_record rec;
    wim_header hdr;
    char iso_arg[MANIFEST_PATH_MAX + 2u];
    char manifest_path[PATH_MAX];
    char manifest_dir[PATH_MAX];
    char prepared_root[PATH_MAX];
    char staging[PATH_MAX];
    char final_dir[PATH_MAX];
    char staged_wim[PATH_MAX];
    char final_wim[PATH_MAX];
    char winjump[PATH_MAX];
    char image_buf[16];
    char hex[65];
    const char *member = NULL;
    const char *image = NULL;
    uint8_t digest[32];
    uint64_t iso_size = 0;
    uint64_t wim_len = 0;
    uint64_t got_len = 0;
    uint64_t patched_len = 0;
    uint64_t pad = 0;
    uint64_t extent_off = 0;
    size_t n_recs = 0;
    int staged = 0;
    int replaced = 0;
    int rc;

    rc = sgn_file_size(o->iso, &iso_size);
    if (rc != SGN_OK)
        return rc;
    rc = sgn_derive_iso_path(o->iso, o->stick, o->iso_path, iso_arg,
                             sizeof(iso_arg));
    if (rc != SGN_OK)
        return rc;
    rc = sgn_manifest_path(o->stick, manifest_path, sizeof(manifest_path));
    if (rc != SGN_OK)
        return rc;
    if (snprintf(manifest_dir, sizeof(manifest_dir), "%s/" SGN_BLESSED_DIR,
                 o->stick) >= (int)sizeof(manifest_dir) ||
        snprintf(prepared_root, sizeof(prepared_root), "%s/" PREP_DIR,
                 o->stick) >= (int)sizeof(prepared_root) ||
        snprintf(staging, sizeof(staging), "%s/.prep-%ld", prepared_root,
                 (long)getpid()) >= (int)sizeof(staging) ||
        snprintf(staged_wim, sizeof(staged_wim), "%s/" PREP_BOOTWIM,
                 staging) >= (int)sizeof(staged_wim)) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "the staging paths under '%s' are too long", o->stick);
        return SGN_ERR_NOSPACE;
    }

    /* ---- 1. is there anything to do? ---- */

    recs = calloc(SGN_MANIFEST_RECORDS, sizeof(*recs));
    if (recs == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory reading the manifest");
        return SGN_ERR_IO;
    }
    rc = sgn_manifest_load(manifest_path, recs, SGN_MANIFEST_RECORDS,
                           &n_recs);
    if (rc != SGN_OK)
        goto out;

    switch (prep_lookup(recs, n_recs, iso_arg + 1, iso_size, &found)) {
    case PREP_STATE_READY: {
        /* The record says so; the artifacts have to agree. A folder emptied
         * by hand, or a run interrupted before the manifest was written, both
         * land here and both mean "do the work" rather than "already done". */
        int intact = (found != NULL && found->splice_n > 0);
        size_t i;

        for (i = 0; intact && i < found->splice_n; i++) {
            char path[PATH_MAX];
            uint64_t have = 0;

            if (snprintf(path, sizeof(path), "%s/%s/%s", prepared_root,
                         found->sha256, found->splice[i].name) >=
                (int)sizeof(path) ||
                sgn_file_size(path, &have) != SGN_OK ||
                have != found->splice[i].len) {
                sgn_log(SGN_LOG_WARN,
                        "the manifest says this ISO is prepared, but its "
                        "sidecar '%s' is missing or the wrong length; "
                        "preparing it again", found->splice[i].name);
                intact = 0;
                break;
            }
        }
        if (intact && !o->force) {
            (void)printf("already prepared %s\n", found->iso);
            (void)printf("  sidecar  %s/%s/\n", prepared_root, found->sha256);
            (void)printf("  manifest %s (unchanged)\n", manifest_path);
            (void)printf("Pass --force to prepare it again.\n");
            rc = SGN_OK;
            goto out;
        }
        if (intact)
            sgn_log(SGN_LOG_INFO,
                    "--force: re-preparing an ISO the manifest already "
                    "describes");
        break;
    }
    case PREP_STATE_STALE:
        sgn_log(SGN_LOG_INFO,
                "this path is in the manifest at %llu bytes and is now %llu: "
                "the ISO changed, so it is prepared again",
                (unsigned long long)((found != NULL) ? found->size : 0u),
                (unsigned long long)iso_size);
        break;
    case PREP_STATE_OTHER_KIND:
        sgn_log(SGN_LOG_WARN,
                "this path already has a blessed record naming '%s'; "
                "preparing replaces it, and the UKI it named is left behind "
                "under " SGN_BLESSED_DIR " for you to remove",
                (found != NULL) ? found->uki : "");
        break;
    case PREP_STATE_ABSENT:
    default:
        break;
    }

    /* ---- 2. the tools, and what the medium is ---- */

    if (!sgn_have_program(WIMLIB)) {
        sgn_set_error(SGN_ERR_EXEC,
                      "'" WIMLIB "' is not on PATH, and it is what rewrites "
                      "boot.wim. Install wimlib (app-arch/wimlib on Gentoo, "
                      "wimtools on Debian) and try again.");
        rc = SGN_ERR_EXEC;
        goto out;
    }
    rc = find_winjump(o, winjump, sizeof(winjump));
    if (rc != SGN_OK)
        goto out;
    sgn_log(SGN_LOG_DEBUG, "jump binary: %s", winjump);

    rc = iso_view_open(o->iso, &view);
    if (rc != SGN_OK)
        goto out;

    iso_view_family_probe(view, &probe);
    fam = family_detect(&probe);
    sgn_log(SGN_LOG_INFO, "family: %s (%s)", fam->name,
            sgn_family_slug(fam->id));
    if (fam->id != FAMILY_WINDOWS) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "this ISO classifies as '%s', not Windows install "
                      "media. Preparation applies to Windows media only; "
                      "Linux media takes the bless path.%s", fam->name,
                      (iso_view_udf_reader(view) == NULL)
                          ? " Note that 7z is not on PATH, so this ISO's UDF "
                            "namespace was not read, and Windows install "
                            "media looks empty without it."
                          : "");
        rc = SGN_ERR_FORMAT;
        goto out;
    }

    /* ---- 3. the length the ISO's own directory entry claims ---- */

    if (!iso_view_find(view, BOOTWIM_MEMBER, &member)) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "this ISO has no " BOOTWIM_MEMBER ", so there is no "
                      "WinPE image to patch");
        rc = SGN_ERR_FORMAT;
        goto out;
    }
    if (!iso_view_size(view, member, &wim_len) || wim_len == 0) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "the ISO listing gave no length for /%s, so the byte "
                      "range to replace cannot be established. This needs the "
                      "UDF listing (7z); the ISO9660 reader lists names "
                      "without sizes.", member);
        rc = SGN_ERR_FORMAT;
        goto out;
    }
    sgn_log(SGN_LOG_INFO, "/%s: %llu bytes per the ISO's directory entry",
            member, (unsigned long long)wim_len);

    /* ---- 4. the checksum, which names the sidecar folder ---- */

    sgn_log(SGN_LOG_INFO, "hashing %s...", o->iso);
    rc = sgn_sha256_file(o->iso, digest, &got_len);
    if (rc != SGN_OK)
        goto out;
    if (got_len != iso_size) {
        sgn_set_error(SGN_ERR_IO,
                      "'%s' changed length while it was being read (%llu then "
                      "%llu); prepare it again once it has settled", o->iso,
                      (unsigned long long)iso_size,
                      (unsigned long long)got_len);
        rc = SGN_ERR_IO;
        goto out;
    }
    sgn_hex32(digest, hex);

    if (snprintf(final_dir, sizeof(final_dir), "%s/%s", prepared_root, hex) >=
            (int)sizeof(final_dir) ||
        snprintf(final_wim, sizeof(final_wim), "%s/" PREP_BOOTWIM,
                 final_dir) >= (int)sizeof(final_wim)) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "the sidecar path under '%s' is too long", o->stick);
        rc = SGN_ERR_NOSPACE;
        goto out;
    }

    if (o->dry_run) {
        (void)printf("would prepare %s\n", iso_arg + 1);
        (void)printf("  size     %llu\n", (unsigned long long)iso_size);
        (void)printf("  sha256   %s\n", hex);
        (void)printf("  sidecar  %s\n", final_wim);
        (void)printf("  splice   <iso-offset>:%llu:" PREP_BOOTWIM "\n",
                     (unsigned long long)wim_len);
        (void)printf("  manifest %s\n", manifest_path);
        (void)printf("The extent's offset is only established by a real run: "
                     "it is proved against the extracted boot.wim, and "
                     "extracting it is the expensive half.\n");
        rc = SGN_OK;
        goto out;
    }

    /* ---- 5. staging, beside the destination so the result is renamed ---- */

    /* The manifest's directory first, and before any of the expensive work:
     * the record is written last, and a run that did 600 MB of it only to
     * fail on a missing directory would leave a sidecar nothing points at. */
    rc = sgn_mkdir_p(manifest_dir);
    if (rc != SGN_OK)
        goto out;
    rc = sgn_mkdir_p(staging);
    if (rc != SGN_OK)
        goto out;
    staged = 1;

    sgn_log(SGN_LOG_INFO, "extracting /%s (%llu bytes)", member,
            (unsigned long long)wim_len);
    rc = iso_view_extract(view, member, staged_wim);
    if (rc != SGN_OK)
        goto out;
    rc = sgn_file_size(staged_wim, &got_len);
    if (rc != SGN_OK)
        goto out;
    if (got_len != wim_len) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "the extracted boot.wim is %llu bytes where the ISO's "
                      "directory entry claims %llu; the two readers disagree "
                      "about this file and neither answer can be trusted",
                      (unsigned long long)got_len,
                      (unsigned long long)wim_len);
        rc = SGN_ERR_FORMAT;
        goto out;
    }

    /* ---- 6. the extent, found and then proved ---- */

    rc = locate_and_prove(o->iso, iso_size, staged_wim, wim_len, &extent_off,
                          &hdr);
    if (rc != SGN_OK)
        goto out;
    sgn_log(SGN_LOG_INFO,
            "boot.wim: offset %llu, length %llu, contiguous and "
            "sector-aligned (verified byte for byte)",
            (unsigned long long)extent_off, (unsigned long long)wim_len);
    if (hdr.total_parts != 1u) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "this boot.wim is part %u of a %u-part split WIM, which "
                      "this route does not handle",
                      (unsigned)hdr.part_number, (unsigned)hdr.total_parts);
        rc = SGN_ERR_FORMAT;
        goto out;
    }

    /* Which image WinPE boots is the WIM's own business and it says so in its
     * header; measured as 2 on Win11 25H2 media, where image 1's shell never
     * runs (DESIGN Section 6.3). Reading it beats hard-coding the measurement,
     * and --image overrides both. */
    if (o->image != NULL) {
        image = o->image;
    } else if (hdr.boot_index != 0) {
        if (snprintf(image_buf, sizeof(image_buf), "%u",
                     (unsigned)hdr.boot_index) >= (int)sizeof(image_buf)) {
            sgn_set_error(SGN_ERR_INTERNAL, "implausible boot index");
            rc = SGN_ERR_INTERNAL;
            goto out;
        }
        image = image_buf;
    } else {
        sgn_set_error(SGN_ERR_FORMAT,
                      "this boot.wim names no bootable image in its header, "
                      "so there is no way to tell which of its %u images "
                      "WinPE runs. Pass --image <n>.",
                      (unsigned)hdr.image_count);
        rc = SGN_ERR_FORMAT;
        goto out;
    }
    sgn_log(SGN_LOG_INFO, "patching WIM image %s of %u", image,
            (unsigned)hdr.image_count);

    /* ---- 7. the patch ---- */

    rc = patch_wim(staged_wim, image, staging, winjump, iso_arg + 1,
                   iso_size);
    if (rc != SGN_OK)
        goto out;

    /* ---- 8. reclaim, guard, pad, verify ---- */

    {
        char *argv[6];
        size_t i = 0;

        argv[i++] = sgn_arg(WIMLIB);
        argv[i++] = sgn_arg("optimize");
        argv[i++] = staged_wim;
        if (o->recompress)
            argv[i++] = sgn_arg("--recompress");
        argv[i] = NULL;

        sgn_log(SGN_LOG_INFO, "reclaiming slack (%s)",
                o->recompress ? "full LZX repack" : "plain optimize");
        rc = sgn_run(argv);
        if (rc != SGN_OK)
            goto out;
    }

    rc = sgn_file_size(staged_wim, &patched_len);
    if (rc != SGN_OK)
        goto out;
    sgn_log(SGN_LOG_INFO, "patched boot.wim: %llu bytes (original %llu)",
            (unsigned long long)patched_len, (unsigned long long)wim_len);

    rc = prep_fits(patched_len, wim_len);
    if (rc != SGN_OK)
        goto out;

    rc = sgn_pad_file(staged_wim, wim_len);
    if (rc != SGN_OK)
        goto out;
    pad = wim_len - patched_len;
    sgn_log(SGN_LOG_INFO, "padded %llu trailing zero bytes back to %llu",
            (unsigned long long)pad, (unsigned long long)wim_len);

    if (!o->no_verify) {
        char *argv[4];

        argv[0] = sgn_arg(WIMLIB);
        argv[1] = sgn_arg("verify");
        argv[2] = staged_wim;
        argv[3] = NULL;

        sgn_log(SGN_LOG_INFO, "verifying the padded WIM");
        rc = sgn_run(argv);
        if (rc != SGN_OK) {
            sgn_set_error(SGN_ERR_FORMAT,
                          "the padded boot.wim does not pass wimverify, so it "
                          "is not safe to serve. Re-run with --verbose to see "
                          "what wimlib objected to.");
            goto out;
        }
    }

    /* ---- 9. into place, then the record ---- */

    if (sgn_file_exists(final_dir)) {
        /* Same ISO, same checksum, so whatever is there was made from the
         * same bytes; it is replaced rather than merged because a folder
         * holding one old sidecar and one new one is a state nothing can
         * reason about. */
        sgn_log(SGN_LOG_DEBUG, "replacing the existing sidecar folder %s",
                final_dir);
        rc = sgn_rm_rf(final_dir);
        if (rc != SGN_OK)
            goto out;
    }
    if (rename(staging, final_dir) != 0) {
        sgn_set_error_errno(SGN_ERR_IO, "rename '%s' to '%s'", staging,
                            final_dir);
        rc = SGN_ERR_IO;
        goto out;
    }
    staged = 0;

    rc = prep_build_record(iso_arg + 1, iso_size, hex, extent_off, wim_len,
                           PREP_BOOTWIM, &rec);
    if (rc != SGN_OK)
        goto out;
    rc = sgn_manifest_update(manifest_path, &rec, &replaced);
    if (rc != SGN_OK)
        goto out;

    (void)printf("prepared %s\n", rec.iso);
    (void)printf("  sidecar  %s\n", final_wim);
    (void)printf("  splice   %llu:%llu:%s\n",
                 (unsigned long long)rec.splice[0].iso_off,
                 (unsigned long long)rec.splice[0].len,
                 rec.splice[0].name);
    (void)printf("  size     %llu\n", (unsigned long long)rec.size);
    (void)printf("  sha256   %s\n", rec.sha256);
    (void)printf("  manifest %s (%s)\n", manifest_path,
                 replaced ? "record replaced" : "record added");

out:
    /* A half-written prepared folder is worse than none: the loader would
     * find a sidecar the manifest does not describe, or describe one whose
     * bytes were never finished. The staging directory only ever becomes
     * visible as <sha256>/ by a rename, and until then it is removed whole. */
    if (staged) {
        int cleanup = sgn_rm_rf(staging);

        if (cleanup != SGN_OK)
            sgn_log(SGN_LOG_WARN,
                    "could not remove the staging directory '%s'; remove it "
                    "by hand", staging);
    }
    free(recs);
    iso_view_close(view);
    return rc;
}

int sgn_prepare_main(int argc, char **argv)
{
    options o;
    int rc;

    rc = parse_args(argc, argv, &o);
    if (rc != SGN_OK) {
        sgn_log(SGN_LOG_ERROR, "%s", sgn_error());
        sgn_prepare_usage(stderr);
        return rc;
    }
    sgn_log_set_level(o.verbose ? SGN_LOG_DEBUG
                                : (o.quiet ? SGN_LOG_WARN : SGN_LOG_INFO));

    rc = prepare(&o);
    if (rc != SGN_OK)
        sgn_log(SGN_LOG_ERROR, "%s", sgn_error());
    return rc;
}
