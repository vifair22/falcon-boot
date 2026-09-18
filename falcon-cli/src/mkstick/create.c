/* falcon-boot mkstick: `create`, the whole device-configuration flow of
 * DESIGN Section 8.
 *
 * What this command is. It turns a blank block device or image file into the
 * four-partition stick of DESIGN Section 3, with the Secure Boot chain of
 * Section 4 on p1, a MOK keypair whose private half lands only inside p3's
 * LUKS volume per Section 7.5, and whatever UKIs the user named signed with
 * that key and staged on p3.
 *
 * What it is not. It signs, it never verifies-and-permits: enforcement is the
 * platform's (Section 7.2), and nothing here can relax it. The loader it
 * installs is signed here, in userland, because the loader itself never holds
 * a key.
 *
 * The order of the steps is load-bearing in two places and arbitrary
 * everywhere else:
 *
 *   - The partition table is written before anything is attached, because
 *     losetup -P is what publishes the partition nodes and it reads the table
 *     it finds at attach time.
 *   - The MOK keypair exists before the loader is signed, which is before p1
 *     is populated, because the name our loader takes on p1 is read out of
 *     the shim binary and the file we copy there has to be the signed one.
 *
 * Resource discipline: every loop device, mapper and mount is registered with
 * cleanup.h the instant it exists and released in reverse order, so that an
 * error return and a Ctrl-C leave the same (clean) machine state.
 */

#include <mkstick/mkstick.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <common/log.h>
#include <common/proc.h>
#include <common/util.h>
#include <mkstick/cleanup.h>
#include <mkstick/disk.h>
#include <mkstick/keys.h>
#include <mkstick/pass.h>
#include <mkstick/shimname.h>

#include <fs/fs.h>
#include <part/part.h>

#define DEFAULT_SHIM_DIR   "build/shim/bins"
#define DEFAULT_LOADER     "build/bin/falcon-boot.efi"

/* Every external tool this command needs, checked before the first byte is
 * written. Discovering that mkfs.exfat is missing after p1 has been formatted
 * would leave a half-built stick for no reason. */
static const char *const required_tools[] = {
    "sgdisk", "mkfs.fat", "mkfs.exfat", "mkfs.ext4",
    "cryptsetup", "losetup", "mount", "umount", "openssl", "sbsign"
};

static const cli_opt create_opts[] = {
    { "force", CLI_OPT_FLAG, 0, NULL,
      "required for a block device; also overwrites an existing stick" },
    { "size", CLI_OPT_VALUE, 0, "<size>",
      "create the image file at this size when it does not exist" },
    { "p1-size", CLI_OPT_VALUE, 0, "<size>",
      "ESP size (default 64M)" },
    { "iso-size", CLI_OPT_VALUE, 0, "<size|rest>",
      "p2, the exFAT ISO library (default: half of what is left)" },
    { "p3-size", CLI_OPT_VALUE, 0, "<size>",
      "p3, the LUKS2 UKI shelf (default 1G)" },
    { "p4-size", CLI_OPT_VALUE, 0, "<size|rest>",
      "p4, LUKS2 root and home (default: the rest)" },
    { "passphrase-file", CLI_OPT_VALUE, 0, "<file>",
      "read the p3/p4 passphrase from this file instead of prompting" },
    { "shim", CLI_OPT_VALUE, 0, "<dir>",
      "directory holding shimx64.efi and its MokManager" },
    { "loader", CLI_OPT_VALUE, 0, "<file>",
      "the falcon-boot.efi to sign and install" },
    { "uki", CLI_OPT_VALUE, 1, "<file>",
      "a UKI to sign with the new MOK and stage on p3:/uki" },
    { "cn", CLI_OPT_VALUE, 0, "<name>",
      "subject CN of the MOK certificate" },
    { "cert-out", CLI_OPT_VALUE, 0, "<file>",
      "also write the PUBLIC certificate here, in PEM form" },
    { "key-out", CLI_OPT_VALUE, 0, "<dir>",
      "also keep the MOK key and cert here (host-side signing route)" },
    { "p3-plain", CLI_OPT_FLAG, 0, NULL,
      "create p3 WITHOUT LUKS; no on-stick private key, no signing appliance" },
    { "p2-fs", CLI_OPT_VALUE, 0, "<fs>",
      "filesystem for p2 (default exfat)" },
    { "p3-fs", CLI_OPT_VALUE, 0, "<fs>",
      "filesystem for p3 (default ext4)" }
};

/* ---- filesystem choices (DESIGN Section 9) ----
 *
 * The flags exist so the CLI surface is future-proof, and the lists exist so
 * it stays honest: a driver target DESIGN sanctions but nobody has written is
 * refused by name rather than accepted into a stick the loader cannot read.
 *
 * Per role rather than one list, because the roles are not interchangeable.
 * p2 must be readable by every desktop OS the user drags an ISO from and must
 * carry the extent map the injection framework resolves files through (DESIGN
 * Section 6.2), which is exFAT today. p3 is read by our driver alone.
 *
 * `implemented` is cross-checked against the loader's own registry, so this
 * tool cannot offer to create something the loader could not then mount. */
static const char *const p2_implemented[] = { "exfat" };
static const char *const p3_implemented[] = { "ext4" };

/* Sanctioned in DESIGN Section 9, write on demand. Named in the refusal so a
 * user asking for one learns it is planned rather than rejected. */
static const char *const sanctioned[] = {
    "exfat", "ext4", "fat32", "xfs", "btrfs"
};

static int in_list(const char *const *list, size_t n, const char *name)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (strcmp(list[i], name) == 0)
            return 1;
    }
    return 0;
}

static cli_status fs_choose(const cli_args *a, const char *opt,
                            const char *const *impl, size_t n,
                            const char *dflt, const char **out)
{
    const char *want = cli_value(a, opt, dflt);

    *out = dflt;
    if (in_list(impl, n, want)) {
        /* The loader's registry is the authority on "implemented": if these
         * two ever disagree, the stick is the thing that would not boot. */
        if (fs_by_name(want) == NULL)
            return set_error(CLI_ERR_INTERNAL,
                             "%s is listed here but no loader driver claims "
                             "that name", want);
        *out = want;
        return CLI_OK;
    }
    if (in_list(sanctioned, sizeof sanctioned / sizeof sanctioned[0], want))
        return set_error(CLI_ERR_USAGE,
                         "--%s %s: driver not yet implemented for this "
                         "partition; implemented: %s. Sanctioned targets "
                         "(DESIGN Section 9): exfat, ext4, fat32, xfs, btrfs",
                         opt, want, impl[0]);
    return set_error(CLI_ERR_USAGE,
                     "--%s %s: no such filesystem driver; implemented: %s. "
                     "Sanctioned targets (DESIGN Section 9): exfat, ext4, "
                     "fat32, xfs, btrfs", opt, want, impl[0]);
}

/* What `create` was asked to build, beyond the sizes. Gathered once so the
 * steps below take a decision rather than re-reading the command line. */
typedef struct {
    int         p3_plain;
    const char *p2_fs;
    const char *p3_fs;
} fs_plan;

/* ---- a small argv builder, because sgdisk's arguments are computed ---- */

#define AB_MAX_ARGS 64u
#define AB_MAX_POOL 32u
#define AB_POOL_LEN 128u

typedef struct {
    const char *argv[AB_MAX_ARGS];
    size_t      n;
    char        pool[AB_MAX_POOL][AB_POOL_LEN];
    size_t      npool;
    cli_status  rc;
} argbuf;

static void ab_add(argbuf *a, const char *s)
{
    if (a->rc != CLI_OK)
        return;
    if (a->n + 1u >= AB_MAX_ARGS) {
        a->rc = set_error(CLI_ERR_INTERNAL, "argv overflow building a command");
        return;
    }
    a->argv[a->n++] = s;
    a->argv[a->n] = NULL;
}

static void ab_addf(argbuf *a, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void ab_addf(argbuf *a, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (a->rc != CLI_OK)
        return;
    if (a->npool >= AB_MAX_POOL) {
        a->rc = set_error(CLI_ERR_INTERNAL, "argument pool exhausted");
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(a->pool[a->npool], AB_POOL_LEN, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= AB_POOL_LEN) {
        a->rc = set_error(CLI_ERR_RANGE, "argument too long");
        return;
    }
    ab_add(a, a->pool[a->npool]);
    a->npool++;
}

/* ---- sizing (DESIGN Section 3: p1 ~64 MB, p3 ~1 GB, p2 and p4 bulk) ---- */

typedef struct {
    uint64_t size[MKS_PARTS];   /* bytes, each a multiple of MKS_ALIGN */
} layout;

static uint64_t round_down(uint64_t v, uint64_t to)
{
    return v - (v % to);
}

/* One size option: an absolute size, or the word "rest". `rest` is reported
 * through *is_rest so the caller can decide what "the rest" means; only p2
 * and p4 accept it, because only they have a remainder to take. */
static cli_status size_opt(const cli_args *a, const char *name,
                           uint64_t dflt, uint64_t *out, int *is_rest)
{
    const char *text = cli_value(a, name, NULL);
    cli_status rc;

    if (is_rest != NULL)
        *is_rest = 0;
    if (text == NULL) {
        *out = dflt;
        return CLI_OK;
    }
    if (strcmp(text, "rest") == 0) {
        if (is_rest == NULL)
            return set_error(CLI_ERR_USAGE,
                             "--%s cannot be 'rest'; only --iso-size and "
                             "--p4-size split the remainder", name);
        *is_rest = 1;
        *out = 0;
        return CLI_OK;
    }
    rc = util_parse_size(text, out);
    if (rc != CLI_OK)
        return rc;
    *out = round_down(*out, MKS_ALIGN);
    return CLI_OK;
}

static cli_status plan_layout(const cli_args *a, uint64_t disk_bytes,
                              layout *out)
{
    /* One MiB at the front for the protective MBR and the primary GPT (which
     * need 34 sectors) and one at the back for the backup GPT, both rounded
     * up to the alignment so every partition start is a whole number of 4 KiB
     * filesystem blocks. */
    const uint64_t overhead = 2u * (uint64_t)MKS_ALIGN;
    const uint64_t floor_[MKS_PARTS] = {
        MKS_MIN_P1, MKS_MIN_P2, MKS_MIN_P3, MKS_MIN_P4
    };
    static const char *const label[MKS_PARTS] = { "p1", "p2", "p3", "p4" };
    uint64_t pool;
    uint64_t p1 = 0;
    uint64_t p2 = 0;
    uint64_t p3 = 0;
    uint64_t p4 = 0;
    int p2_rest = 0;
    int p4_rest = 0;
    int p2_takes_rest;
    int p4_takes_rest;
    uint64_t rest;
    cli_status rc;
    size_t i;

    if (disk_bytes <= overhead)
        return set_error(CLI_ERR_RANGE, "target is only %llu bytes",
                         (unsigned long long)disk_bytes);
    pool = round_down(disk_bytes - overhead, MKS_ALIGN);

    rc = size_opt(a, "p1-size", MKS_DEFAULT_P1, &p1, NULL);
    if (rc != CLI_OK)
        return rc;
    rc = size_opt(a, "p3-size", MKS_DEFAULT_P3, &p3, NULL);
    if (rc != CLI_OK)
        return rc;
    rc = size_opt(a, "iso-size", 0, &p2, &p2_rest);
    if (rc != CLI_OK)
        return rc;
    rc = size_opt(a, "p4-size", 0, &p4, &p4_rest);
    if (rc != CLI_OK)
        return rc;

    if (p1 + p3 + MKS_MIN_P2 + MKS_MIN_P4 > pool)
        return set_error(CLI_ERR_RANGE,
                         "the target holds %llu bytes, which does not cover "
                         "p1 (%llu) + p3 (%llu) and the floors for p2 and p4; "
                         "shrink --p1-size/--p3-size or use a larger target",
                         (unsigned long long)pool, (unsigned long long)p1,
                         (unsigned long long)p3);
    rest = pool - p1 - p3;

    /* The split rule, stated once: an explicit size wins; one explicit size
     * leaves the whole remainder to the other; with neither given the
     * remainder is halved, because a stick is equally an ISO library and a
     * personal OS and guessing either way would be wrong half the time. Two
     * explicit sizes are taken literally, free space and all. */
    if (p2_rest && p4_rest)
        return set_error(CLI_ERR_USAGE,
                         "--iso-size and --p4-size cannot both be 'rest'");

    /* "Takes the rest" is either asked for by name or implied by the other
     * one having been pinned. Deciding it up front keeps the chain below to
     * three distinct outcomes instead of five branches with repeated bodies. */
    p2_takes_rest = p2_rest || (!p4_rest && p2 == 0 && p4 != 0);
    p4_takes_rest = p4_rest || (!p2_rest && p4 == 0 && p2 != 0);

    if (!p2_rest && !p4_rest && p2 == 0 && p4 == 0) {
        p2 = round_down(rest / 2u, MKS_ALIGN);
        p4 = rest - p2;
    } else if (p2_takes_rest)
        p2 = (rest > p4) ? rest - p4 : 0;
    else if (p4_takes_rest)
        p4 = (rest > p2) ? rest - p2 : 0;

    if (p2 + p4 > rest)
        return set_error(CLI_ERR_RANGE,
                         "p2 (%llu) and p4 (%llu) do not fit in the %llu "
                         "bytes left after p1 and p3",
                         (unsigned long long)p2, (unsigned long long)p4,
                         (unsigned long long)rest);

    out->size[0] = p1;
    out->size[1] = p2;
    out->size[2] = p3;
    out->size[3] = p4;

    for (i = 0; i < MKS_PARTS; i++) {
        if (out->size[i] < floor_[i]) {
            char want[32];
            char got[32];

            return set_error(CLI_ERR_RANGE,
                             "%s would be %s, below the %s it needs to hold "
                             "what DESIGN Section 3 puts there",
                             label[i],
                             util_format_size(out->size[i], got, sizeof got),
                             util_format_size(floor_[i], want, sizeof want));
        }
        if (out->size[i] % MKS_ALIGN != 0)
            return set_error(CLI_ERR_INTERNAL, "%s is not aligned", label[i]);
    }
    return CLI_OK;
}

/* ---- shim discovery ---- */

/* Picks the shim binary out of a directory. `build/shim/bins`, which is what
 * falcon-efi/tools/shim-spike/fetch-shims.sh fills, holds several vendors'
 * shims side by side, so "the only *shimx64.efi in there" is not a rule that
 * works. The order is: an unprefixed shimx64.efi (what a user who staged one
 * file themselves would have), then the vendor and version DESIGN Section 4
 * pins, then a unique match, then a refusal that lists what it saw. */
static cli_status find_shim(const char *dir, char *out, size_t cap)
{
    static const char *const preferred[] = {
        "shimx64.efi", "fedora-16.1-shimx64.efi"
    };
    size_t i;

    for (i = 0; i < sizeof preferred / sizeof preferred[0]; i++) {
        char path[MKS_PATH_MAX];

        if (util_join(path, sizeof path, dir, preferred[i]) != CLI_OK)
            continue;
        if (util_is_file(path)) {
            if (snprintf(out, cap, "%s", path) < 0)
                return set_error(CLI_ERR_RANGE, "shim path too long");
            return CLI_OK;
        }
    }
    return set_error(CLI_ERR_NOTFOUND,
                     "no shim found in %s (looked for shimx64.efi and "
                     "fedora-16.1-shimx64.efi); run "
                     "falcon-efi/tools/shim-spike/fetch-shims.sh to stage "
                     "the pinned packages, or pass --shim <dir>", dir);
}

/* MokManager sits beside the shim under the same vendor prefix, which is how
 * fetch-shims.sh stages it. `wanted` is the name shim itself will look for at
 * boot, read out of the shim binary. */
static cli_status find_mokmanager(const char *shim_path, const char *wanted,
                                  char *out, size_t cap)
{
    char dir[MKS_PATH_MAX];
    const char *base = util_basename(shim_path);
    size_t dirlen = (size_t)(base - shim_path);
    const char *tail = "shimx64.efi";
    size_t baselen = strlen(base);
    size_t taillen = strlen(tail);
    char candidate[MKS_PATH_MAX];

    if (dirlen >= sizeof dir)
        return set_error(CLI_ERR_RANGE, "shim path too long");
    (void)snprintf(dir, sizeof dir, "%.*s", (int)dirlen, shim_path);
    if (dir[0] == '\0')
        (void)snprintf(dir, sizeof dir, "./");

    /* fedora-16.1-shimx64.efi -> fedora-16.1-mmx64.efi */
    if (baselen > taillen && strcmp(base + baselen - taillen, tail) == 0) {
        if (snprintf(candidate, sizeof candidate, "%s%.*s%s", dir,
                     (int)(baselen - taillen), base, wanted) > 0 &&
            util_is_file(candidate)) {
            (void)snprintf(out, cap, "%s", candidate);
            return CLI_OK;
        }
    }
    if (snprintf(candidate, sizeof candidate, "%s%s", dir, wanted) > 0 &&
        util_is_file(candidate)) {
        (void)snprintf(out, cap, "%s", candidate);
        return CLI_OK;
    }
    return set_error(CLI_ERR_NOTFOUND,
                     "shim launches MokManager as %s, but no such file sits "
                     "beside %s; without it a user cannot enroll the cert on "
                     "first boot", wanted, shim_path);
}

/* ---- the target ---- */

static cli_status ensure_target(const cli_args *a, mks_disk *d,
                                const char *path)
{
    const char *size_text = cli_value(a, "size", NULL);
    cli_status rc;

    if (!util_is_file(path) && !util_is_block(path)) {
        uint64_t want;
        int fd;

        if (size_text == NULL)
            return set_error(CLI_ERR_NOTFOUND,
                             "%s does not exist; pass --size to create an "
                             "image file of that size", path);
        rc = util_parse_size(size_text, &want);
        if (rc != CLI_OK)
            return rc;

        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0)
            return set_error_errno(CLI_ERR_IO, "creating %s", path);
        if (ftruncate(fd, (off_t)want) != 0) {
            (void)close(fd);
            return set_error_errno(CLI_ERR_IO, "sizing %s", path);
        }
        (void)close(fd);
        log_info("created %s", path);
    } else if (size_text != NULL) {
        log_warn("--size is ignored: %s already exists", path);
    }

    return disk_stat(d, path);
}

/* The idempotence guard. A target that already carries a partition table is
 * almost always the user running the same command twice, and the second run
 * would destroy the stick they just built (including the only copy of its MOK
 * private key, which lives nowhere else). --force is the way to say it was
 * meant. */
static cli_status refuse_existing(const mks_disk *d, int force)
{
    gpt_part parts[16];
    gpt_table table;
    disk_io io;
    cli_status rc;
    int gpt_rc;

    memset(&table, 0, sizeof table);
    table.parts = parts;
    table.cap = (uint32_t)(sizeof parts / sizeof parts[0]);

    rc = disk_io_open(&io, d->path, 0, d->size);
    if (rc != CLI_OK)
        return rc;
    gpt_rc = gpt_parse(&table, disk_io_read, &io, d->sector);
    disk_io_close(&io);

    if (gpt_rc != GPT_OK || table.count == 0)
        return CLI_OK;

    if (force) {
        log_warn("%s already carries a GPT with %u partitions; --force given, "
                 "overwriting", d->path, table.count);
        return CLI_OK;
    }
    return set_error(CLI_ERR_EXISTS,
                     "%s already carries a GPT with %u partitions%s; pass "
                     "--force to overwrite it",
                     d->path, table.count,
                     gpt_has_role(&table, PART_ROLE_SHELF)
                         ? " and looks like a falcon stick (its p3 holds the "
                           "only copy of that stick's MOK private key)"
                         : "");
}

/* ---- the steps ---- */

static cli_status write_partition_table(const mks_disk *d, const layout *l)
{
    argbuf ab;
    uint64_t mib[MKS_PARTS];
    size_t i;

    for (i = 0; i < MKS_PARTS; i++)
        mib[i] = l->size[i] / ((uint64_t)1024 * 1024);

    memset(&ab, 0, sizeof ab);
    ab.rc = CLI_OK;
    ab_add(&ab, "sgdisk");
    ab_add(&ab, "--zap-all");

    /* p1 starts at the 1 MiB alignment boundary; every later partition takes
     * sgdisk's "0" for start, which means the next aligned free block. */
    ab_addf(&ab, "-n1:1M:+%lluM", (unsigned long long)mib[0]);
    ab_addf(&ab, "-t1:%s", MKS_TYPE_ESP);
    ab_addf(&ab, "-c1:%s", MKS_LABEL_P1);

    /* p2's -c is not a label for the user's benefit: with a type GUID the
     * whole world shares, the name is what tells the loader this partition is
     * ours (DESIGN Section 3, and the selfcheck in guid.c). */
    ab_addf(&ab, "-n2:0:+%lluM", (unsigned long long)mib[1]);
    ab_addf(&ab, "-t2:%s", MKS_TYPE_ISO);
    ab_addf(&ab, "-c2:%s", MKS_LABEL_P2);

    ab_addf(&ab, "-n3:0:+%lluM", (unsigned long long)mib[2]);
    ab_addf(&ab, "-t3:%s", MKS_TYPE_SHELF);
    ab_addf(&ab, "-c3:%s", MKS_LABEL_P3);

    ab_addf(&ab, "-n4:0:+%lluM", (unsigned long long)mib[3]);
    ab_addf(&ab, "-t4:%s", MKS_TYPE_LUKS);
    ab_addf(&ab, "-c4:%s", MKS_LABEL_P4);

    ab_add(&ab, d->path);
    if (ab.rc != CLI_OK)
        return ab.rc;

    log_info("partitioning (GPT, four partitions per DESIGN Section 3)");
    {
        proc_spec spec;

        memset(&spec, 0, sizeof spec);
        spec.argv = ab.argv;
        spec.quiet_stdout = (cli_log_level_get() > CLI_LOG_DEBUG);
        return proc_run(&spec);
    }
}

/* One mkfs invocation, chosen by name. The label flag is not shared across
 * filesystems (mkfs.exfat and mkfs.ext4 spell it the same way by luck, not by
 * standard), so a new target adds a row here rather than a special case at
 * every call site. */
static cli_status make_fs(const char *fs, const char *label, const char *dev)
{
    if (strcmp(fs, "exfat") == 0)
        return proc_runl_quiet("mkfs.exfat", "-n", label, dev, PROC_END);
    if (strcmp(fs, "ext4") == 0)
        return proc_runl_quiet("mkfs.ext4", "-q", "-L", label, dev, PROC_END);
    return set_error(CLI_ERR_INTERNAL, "no mkfs rule for %s", fs);
}

static cli_status make_filesystems(const mks_disk *d, const mks_pass *pass,
                                   const fs_plan *plan)
{
    char mapper[64];
    char mapper_path[MKS_PATH_MAX];
    cli_status rc;

    log_info("p1: FAT32 (the ESP the firmware boots)");
    rc = proc_runl_quiet("mkfs.fat", "-F", "32", "-n", "FALCONESP",
                         d->part[0], PROC_END);
    if (rc != CLI_OK)
        return rc;

    log_info("p2: %s (the drag-and-drop ISO library)", plan->p2_fs);
    rc = make_fs(plan->p2_fs, MKS_LABEL_P2, d->part[1]);
    if (rc != CLI_OK)
        return rc;

    /* DESIGN Section 3: p3's encryption is the default, not a requirement.
     * Plaintext means no keyslot to format and no mapper to open; the
     * filesystem goes straight onto the partition, and the consequences were
     * printed before any of this ran. */
    if (plan->p3_plain) {
        log_info("p3: %s, PLAINTEXT (no LUKS: --p3-plain)", plan->p3_fs);
        rc = make_fs(plan->p3_fs, MKS_LABEL_P3, d->part[2]);
        if (rc != CLI_OK)
            return rc;
        goto p4;
    }

    /* p3's pbkdf2 keyslot, with the iteration count DESIGN Section 8 pins.
     * This is the one cryptsetup invocation whose every flag matters: the
     * loader's in-EFI unlock handles pbkdf2 and aes-xts-plain64 and nothing
     * else, so a default luksFormat here produces a partition the boot menu
     * can see and cannot open. */
    log_info("p3: LUKS2 (pbkdf2, %s iterations) + %s",
             MKS_P3_PBKDF2_ITERATIONS, plan->p3_fs);
    rc = proc_input_l(pass->buf, pass->len, "cryptsetup", "luksFormat",
                      "--type", "luks2",
                      "--pbkdf", "pbkdf2",
                      "--pbkdf-force-iterations", MKS_P3_PBKDF2_ITERATIONS,
                      "--cipher", "aes-xts-plain64",
                      "--key-size", "512",
                      "--batch-mode", d->part[2], "-", PROC_END);
    if (rc != CLI_OK)
        return rc;

    (void)snprintf(mapper, sizeof mapper, "falcon-mkstick-p3-%d",
                   (int)getpid());
    rc = proc_input_l(pass->buf, pass->len, "cryptsetup", "open",
                      d->part[2], mapper, "-", PROC_END);
    if (rc != CLI_OK)
        return rc;
    rc = cleanup_push_crypt(mapper);
    if (rc != CLI_OK) {
        (void)proc_runl_quiet("cryptsetup", "close", mapper, PROC_END);
        return rc;
    }
    (void)snprintf(mapper_path, sizeof mapper_path, "/dev/mapper/%s", mapper);
    rc = make_fs(plan->p3_fs, MKS_LABEL_P3, mapper_path);
    if (rc != CLI_OK)
        return rc;
    rc = cleanup_pop();                    /* close the mapper */
    if (rc != CLI_OK)
        return rc;

p4:
    /* p4 keeps LUKS2's stock argon2id, because it is unlocked by the Linux
     * kernel inside the UKI's initramfs, where memory-hardness is free
     * (DESIGN Section 3). EFI-land never touches it. p4 is encrypted whether
     * or not p3 is: it holds root and home, the loader never reads it, and
     * --p3-plain is a statement about the UKI shelf alone. */
    log_info("p4: LUKS2 (argon2id, stock defaults) + ext4");
    rc = proc_input_l(pass->buf, pass->len, "cryptsetup", "luksFormat",
                      "--type", "luks2", "--batch-mode", d->part[3], "-",
                      PROC_END);
    if (rc != CLI_OK)
        return rc;

    (void)snprintf(mapper, sizeof mapper, "falcon-mkstick-p4-%d",
                   (int)getpid());
    rc = proc_input_l(pass->buf, pass->len, "cryptsetup", "open",
                      d->part[3], mapper, "-", PROC_END);
    if (rc != CLI_OK)
        return rc;
    rc = cleanup_push_crypt(mapper);
    if (rc != CLI_OK) {
        (void)proc_runl_quiet("cryptsetup", "close", mapper, PROC_END);
        return rc;
    }
    (void)snprintf(mapper_path, sizeof mapper_path, "/dev/mapper/%s", mapper);
    rc = proc_runl_quiet("mkfs.ext4", "-q", "-L", MKS_LABEL_P4, mapper_path,
                         PROC_END);
    if (rc != CLI_OK)
        return rc;
    return cleanup_pop();
}

static cli_status mount_at(const char *device, const char *mountpoint)
{
    cli_status rc = proc_runl_quiet("mount", device, mountpoint, PROC_END);

    if (rc != CLI_OK)
        return rc;
    return cleanup_push_mount(mountpoint);
}

/* p1: the Secure Boot chain of DESIGN Section 4. The loader's filename is not
 * ours to choose; it is whatever this shim binary has compiled in. */
static cli_status install_esp(const mks_disk *d, const char *mnt,
                              const char *shim_path, const char *mm_path,
                              const char *mm_name, const char *next_stage,
                              const char *signed_loader, const mok_keys *keys)
{
    char bootdir[MKS_PATH_MAX];
    char dst[MKS_PATH_MAX];
    cli_status rc;

    log_info("p1: staging shim -> %s -> %s", mm_name, next_stage);
    rc = mount_at(d->part[0], mnt);
    if (rc != CLI_OK)
        return rc;

    rc = util_join(bootdir, sizeof bootdir, mnt, MKS_ESP_BOOT_DIR);
    if (rc == CLI_OK)
        rc = util_mkdir_p(bootdir, 0755);
    if (rc == CLI_OK)
        rc = util_join(dst, sizeof dst, bootdir, MKS_ESP_SHIM_NAME);
    if (rc == CLI_OK)
        rc = util_copy_file(shim_path, dst, 0644);
    if (rc == CLI_OK)
        rc = util_join(dst, sizeof dst, bootdir, mm_name);
    if (rc == CLI_OK)
        rc = util_copy_file(mm_path, dst, 0644);
    if (rc == CLI_OK)
        rc = util_join(dst, sizeof dst, bootdir, next_stage);
    if (rc == CLI_OK)
        rc = util_copy_file(signed_loader, dst, 0644);

    /* The PUBLIC certificate, and only ever the public one. DESIGN Section 4
     * puts it at the root of p1 so MokManager can be pointed at it on first
     * boot; DESIGN Section 7.5 is why its private half is not within a mile
     * of this partition. */
    if (rc == CLI_OK)
        rc = util_join(dst, sizeof dst, mnt, MKS_ESP_CERT_NAME);
    if (rc == CLI_OK)
        rc = util_copy_file(keys->cer, dst, 0644);

    if (rc != CLI_OK) {
        (void)cleanup_pop();
        return rc;
    }
    return cleanup_pop();
}

/* p3: the UKI shelf and the private key (DESIGN Sections 3 and 7.5).
 *
 * The plaintext variant differs in exactly two places, both stated in DESIGN
 * Section 3: there is no mapper to open, and the MOK private key does NOT go
 * on the stick. The second is the key model's invariant holding rather than
 * bending: the private key lives encrypted or it does not live on the stick
 * at all, so a plaintext shelf gets the public certificate and nothing else,
 * and the private half is the user's to keep (--key-out, required for this
 * flag). */
static cli_status install_shelf(const cli_args *a, const mks_disk *d,
                                const char *mnt, const mks_pass *pass,
                                const mok_keys *keys, const fs_plan *plan)
{
    char mapper[64];
    char mapper_path[MKS_PATH_MAX];
    char ukidir[MKS_PATH_MAX];
    char dst[MKS_PATH_MAX];
    size_t n = cli_count(a, "uki");
    size_t i;
    int mapped = 0;
    cli_status rc;

    if (plan->p3_plain) {
        rc = mount_at(d->part[2], mnt);
        if (rc != CLI_OK)
            return rc;
    } else {
        (void)snprintf(mapper, sizeof mapper, "falcon-mkstick-p3-%d",
                       (int)getpid());
        rc = proc_input_l(pass->buf, pass->len, "cryptsetup", "open",
                          d->part[2], mapper, "-", PROC_END);
        if (rc != CLI_OK)
            return rc;
        rc = cleanup_push_crypt(mapper);
        if (rc != CLI_OK) {
            (void)proc_runl_quiet("cryptsetup", "close", mapper, PROC_END);
            return rc;
        }
        mapped = 1;
        (void)snprintf(mapper_path, sizeof mapper_path, "/dev/mapper/%s",
                       mapper);

        rc = mount_at(mapper_path, mnt);
        if (rc != CLI_OK) {
            (void)cleanup_pop();
            return rc;
        }
    }

    rc = util_join(ukidir, sizeof ukidir, mnt, MKS_P3_UKI_DIR);
    if (rc == CLI_OK)
        rc = util_mkdir_p(ukidir, 0755);

    for (i = 0; i < n && rc == CLI_OK; i++) {
        const char *src = cli_at(a, "uki", i);

        rc = util_join(dst, sizeof dst, ukidir, util_basename(src));
        if (rc != CLI_OK)
            break;
        if (util_is_file(dst)) {
            rc = set_error(CLI_ERR_EXISTS,
                           "two --uki arguments have the basename %s",
                           util_basename(src));
            break;
        }
        log_info("p3: signing and staging %s", util_basename(src));
        /* Signed on the way in, never staged unsigned: an unsigned UKI on p3
         * is refused by the platform under Secure Boot, and the user would
         * only find out at the boot menu. */
        rc = mok_sign(keys, src, dst);
    }

    /* The private key, at mode 0600, inside the LUKS volume and nowhere else
     * (DESIGN Section 7.5). This is the one copy that travels with the stick;
     * the signing appliance and a future `sign` run read it from here after
     * unlocking p3 with the same passphrase the loader asks for.
     *
     * On a plaintext shelf this step does not happen at all. Writing the key
     * here would put it on a partition anyone can read, which is the one
     * thing the key model does not permit. */
    if (rc == CLI_OK && !plan->p3_plain) {
        log_info("p3: MOK private key -> %s (0600, inside the LUKS volume)",
                 MKS_P3_KEY_NAME);
        rc = util_join(dst, sizeof dst, mnt, MKS_P3_KEY_NAME);
        if (rc == CLI_OK)
            rc = util_copy_file(keys->key, dst, 0600);
    }
    if (rc == CLI_OK)
        rc = util_join(dst, sizeof dst, mnt, MKS_P3_CERT_NAME);
    if (rc == CLI_OK)
        rc = util_copy_file(keys->crt, dst, 0644);

    if (rc != CLI_OK) {
        (void)cleanup_pop();               /* umount */
        if (mapped)
            (void)cleanup_pop();           /* close */
        return rc;
    }
    rc = cleanup_pop();
    if (rc != CLI_OK) {
        if (mapped)
            (void)cleanup_pop();
        return rc;
    }
    return mapped ? cleanup_pop() : CLI_OK;
}

/* p2: the reserved directory of DESIGN Section 6.2, with the blessed-artifact
 * skeleton of Section 8.1 inside it. Created empty so that the bless tool and
 * the signing appliance have somewhere to write without having to invent the
 * layout, and so the loader's manifest read degrades to "nothing is blessed"
 * rather than to an error about a missing directory. */
static cli_status install_iso_library(const mks_disk *d, const char *mnt)
{
    char dir[MKS_PATH_MAX];
    cli_status rc;

    log_info("p2: creating /%s", MKS_P2_BLESSED_DIR);
    rc = mount_at(d->part[1], mnt);
    if (rc != CLI_OK)
        return rc;

    rc = util_join(dir, sizeof dir, mnt, MKS_P2_BLESSED_DIR);
    if (rc == CLI_OK)
        rc = util_mkdir_p(dir, 0755);

    if (rc != CLI_OK) {
        (void)cleanup_pop();
        return rc;
    }
    return cleanup_pop();
}

/* ---- the command ---- */

static cli_status create_run(const cli_args *a)
{
    const char *target = a->pos[0];
    const char *shim_dir = cli_value(a, "shim", DEFAULT_SHIM_DIR);
    const char *loader = cli_value(a, "loader", DEFAULT_LOADER);
    const char *cn = cli_value(a, "cn", MOK_DEFAULT_CN);
    const char *cert_out = cli_value(a, "cert-out", NULL);
    const char *key_out = cli_value(a, "key-out", NULL);
    const int force = cli_flag(a, "force");
    fs_plan plan_fs;
    char shim_path[MKS_PATH_MAX];
    char mm_path[MKS_PATH_MAX];
    char mm_name[SHIM_NEXT_STAGE_MAX];
    char next_stage[SHIM_NEXT_STAGE_MAX];
    char tmpdir[] = "/tmp/falcon-mkstick.XXXXXX";
    char keydir[MKS_PATH_MAX];
    char mntdir[MKS_PATH_MAX];
    char signed_loader[MKS_PATH_MAX];
    char sizetext[32];
    mks_disk disk;
    mks_pass pass;
    mok_keys keys;
    layout plan;
    cli_status rc;
    size_t i;

    memset(&pass, 0, sizeof pass);

    rc = mkstick_guid_selfcheck();
    if (rc != CLI_OK)
        return rc;

    plan_fs.p3_plain = cli_flag(a, "p3-plain");
    rc = fs_choose(a, "p2-fs", p2_implemented,
                   sizeof p2_implemented / sizeof p2_implemented[0], "exfat",
                   &plan_fs.p2_fs);
    if (rc != CLI_OK)
        return rc;
    rc = fs_choose(a, "p3-fs", p3_implemented,
                   sizeof p3_implemented / sizeof p3_implemented[0], "ext4",
                   &plan_fs.p3_fs);
    if (rc != CLI_OK)
        return rc;

    if (plan_fs.p3_plain) {
        /* DESIGN Section 3 asks for the consequences to be stated at creation
         * time rather than discovered later, and they are stated before the
         * first byte is written, because the one that matters is a refusal:
         * with no encrypted shelf the private key has nowhere on the stick to
         * live, so it must have somewhere on the HOST to live or it dies with
         * the temporary directory this command shreds on the way out, and the
         * stick can never be given another signed UKI. */
        if (key_out == NULL)
            return set_error(CLI_ERR_USAGE,
                             "--p3-plain leaves the MOK private key nowhere "
                             "to live. Pass --key-out <dir> to keep it on "
                             "this host, or drop --p3-plain");
        cli_log_blank();
        log_warn("--p3-plain: p3 will be PLAINTEXT. What that means:");
        log_warn("  - no on-stick MOK private key, so NO SIGNING APPLIANCE on "
                 "this stick;");
        log_warn("  - blessing media and signing UKIs is external-host only, "
                 "from --key-out;");
        log_warn("  - the loader asks for no passphrase and hands no "
                 "credential on, so the");
        log_warn("    initramfs prompts for p4 itself;");
        log_warn("  - integrity is unchanged (the UKI is still signed and "
                 "shim still verifies it);");
        log_warn("  - confidentiality of the UKI, its initramfs and the "
                 "appliance is given up.");
        cli_log_blank();
    }

    for (i = 0; i < sizeof required_tools / sizeof required_tools[0]; i++) {
        if (!proc_exists(required_tools[i]))
            return set_error(CLI_ERR_NOTFOUND,
                             "%s is not on PATH; mkstick shells out to it "
                             "rather than reimplementing it (DESIGN Section "
                             "8)", required_tools[i]);
    }

    /* Loop devices, device-mapper and mount are all root-only, and finding
     * that out three steps in would leave a partitioned, unformatted stick. */
    if (geteuid() != 0)
        return set_error(CLI_ERR_PERM,
                         "mkstick create needs root: it attaches loop "
                         "devices, opens LUKS mappings and mounts "
                         "filesystems");

    rc = ensure_target(a, &disk, target);
    if (rc != CLI_OK)
        return rc;

    if (disk.is_block) {
        disk_describe(&disk);
        if (!force)
            return set_error(CLI_ERR_PERM,
                             "%s is a block device and every byte on it will "
                             "be destroyed; pass --force once you have "
                             "checked the model and size above", disk.path);
        rc = disk_check_not_mounted(&disk);
        if (rc != CLI_OK)
            return rc;
    } else {
        disk_describe(&disk);
    }

    rc = refuse_existing(&disk, force);
    if (rc != CLI_OK)
        return rc;

    rc = plan_layout(a, disk.size, &plan);
    if (rc != CLI_OK)
        return rc;
    for (i = 0; i < MKS_PARTS; i++)
        log_info("  p%zu %s", i + 1u,
                 util_format_size(plan.size[i], sizetext, sizeof sizetext));

    if (!util_is_file(loader))
        return set_error(CLI_ERR_NOTFOUND,
                         "no loader at %s (run make), or pass --loader",
                         loader);
    rc = find_shim(shim_dir, shim_path, sizeof shim_path);
    if (rc != CLI_OK)
        return rc;

    /* DESIGN Section 4: read the next-stage name out of the shim rather than
     * assuming it, so that swapping shim vendors stays a data change. */
    rc = shim_next_stage_name(shim_path, next_stage, sizeof next_stage);
    if (rc != CLI_OK)
        return rc;
    rc = shim_mokmanager_name(shim_path, mm_name, sizeof mm_name);
    if (rc != CLI_OK)
        return rc;
    rc = find_mokmanager(shim_path, mm_name, mm_path, sizeof mm_path);
    if (rc != CLI_OK)
        return rc;
    log_info("shim:     %s", shim_path);
    log_info("  next stage compiled in: %s", next_stage);
    log_info("  MokManager:             %s", mm_name);

    for (i = 0; i < cli_count(a, "uki"); i++) {
        const char *u = cli_at(a, "uki", i);

        if (!util_is_file(u))
            return set_error(CLI_ERR_NOTFOUND, "no UKI at %s", u);
    }

    rc = pass_acquire(&pass, cli_value(a, "passphrase-file", NULL));
    if (rc != CLI_OK)
        return rc;

    /* Everything below leaves state behind on failure, so from here on every
     * exit goes through `out`. */
    if (mkdtemp(tmpdir) == NULL) {
        rc = set_error_errno(CLI_ERR_IO, "creating a private work directory");
        goto out;
    }
    rc = util_join(keydir, sizeof keydir, tmpdir, "keys");
    if (rc == CLI_OK)
        rc = util_mkdir_p(keydir, 0700);
    if (rc == CLI_OK)
        rc = cleanup_push_shreddir(keydir);
    if (rc == CLI_OK)
        rc = util_join(mntdir, sizeof mntdir, tmpdir, "mnt");
    if (rc == CLI_OK)
        rc = util_mkdir_p(mntdir, 0700);
    if (rc == CLI_OK)
        rc = cleanup_push_rmdir(mntdir);
    if (rc != CLI_OK)
        goto out;

    rc = write_partition_table(&disk, &plan);
    if (rc != CLI_OK)
        goto out;

    rc = disk_attach(&disk);
    if (rc != CLI_OK)
        goto out;
    if (disk.is_block && proc_exists("partprobe"))
        (void)proc_runl_quiet("partprobe", disk.base, PROC_END);
    rc = disk_settle(&disk);
    if (rc != CLI_OK)
        goto out;

    rc = make_filesystems(&disk, &pass, &plan_fs);
    if (rc != CLI_OK)
        goto out;

    rc = mok_paths(&keys, keydir);
    if (rc != CLI_OK)
        goto out;
    rc = mok_generate(&keys, cn);
    if (rc != CLI_OK)
        goto out;

    rc = util_join(signed_loader, sizeof signed_loader, keydir,
                   "falcon-boot-signed.efi");
    if (rc != CLI_OK)
        goto out;
    log_info("signing the loader with the new MOK");
    rc = mok_sign(&keys, loader, signed_loader);
    if (rc != CLI_OK)
        goto out;

    rc = install_esp(&disk, mntdir, shim_path, mm_path, mm_name, next_stage,
                     signed_loader, &keys);
    if (rc != CLI_OK)
        goto out;

    rc = install_shelf(a, &disk, mntdir, &pass, &keys, &plan_fs);
    if (rc != CLI_OK)
        goto out;

    rc = install_iso_library(&disk, mntdir);
    if (rc != CLI_OK)
        goto out;

    if (cert_out != NULL) {
        /* PEM, not the DER copy that went to p1: DER is what MokManager's
         * enrollment UI reads, PEM is what every host-side tool that might
         * want this certificate (sbverify, virt-fw-vars) takes. */
        rc = util_copy_file(keys.crt, cert_out, 0644);
        if (rc != CLI_OK)
            goto out;
        log_info("public certificate (PEM) also written to %s", cert_out);
    }
    if (key_out != NULL) {
        char dst[MKS_PATH_MAX];

        rc = util_mkdir_p(key_out, 0700);
        if (rc == CLI_OK)
            rc = util_join(dst, sizeof dst, key_out, MKS_P3_KEY_NAME);
        if (rc == CLI_OK)
            rc = util_copy_file(keys.key, dst, 0600);
        if (rc == CLI_OK)
            rc = util_join(dst, sizeof dst, key_out, MKS_P3_CERT_NAME);
        if (rc == CLI_OK)
            rc = util_copy_file(keys.crt, dst, 0644);
        if (rc != CLI_OK)
            goto out;
        /* Section 7.5's external-host signing route. Worth saying out loud:
         * this is a SECOND copy of the private key, outside the LUKS volume,
         * and it is now the user's to protect. */
        log_warn("a copy of the MOK PRIVATE key is now at %s/%s, outside any "
                 "encryption; protect it or delete it", key_out,
                 MKS_P3_KEY_NAME);
    }

out:
    pass_wipe(&pass);

    /* Unwind in order: mnt dir, key dir (shredded), loop device. cleanup_pop
     * reports its own failures; a successful build that cannot release its
     * loop device is still a failed run, because the next one would attach a
     * second. */
    while (cleanup_depth() > 0) {
        cli_status crc = cleanup_pop();

        if (crc != CLI_OK && rc == CLI_OK)
            rc = crc;
        else if (crc != CLI_OK)
            log_warn("%s", cli_error_msg());
    }
    if (rmdir(tmpdir) != 0)
        log_debug("leaving %s behind", tmpdir);

    if (rc == CLI_OK) {
        cli_log_blank();
        log_info("%s is ready.", disk.path);
        log_info("First boot with Secure Boot ON: shim launches MokManager; "
                 "enroll p1:/%s, reboot.", MKS_ESP_CERT_NAME);
        if (plan_fs.p3_plain)
            log_warn("p3 is PLAINTEXT: no signing appliance on this stick, "
                     "and the only copy of the MOK private key is the one in "
                     "%s.", key_out);
    }
    return rc;
}

const cli_cmd mks_cmd_create = {
    "create",
    "build a complete falcon-boot stick on a block device or image file",
    "<target>",
    create_opts,
    sizeof create_opts / sizeof create_opts[0],
    1, 1,
    "layout (DESIGN Section 3):\n"
    "  p1  ESP        FAT32          default 64M   shim, MokManager, loader, cert\n"
    "  p2  ISO lib    exFAT          half the rest drag-and-drop ISOs\n"
    "  p3  UKI shelf  LUKS2+ext4     default 1G    UKIs and the MOK private key\n"
    "  p4  root+home  LUKS2+ext4     the rest      never touched by EFI\n"
    "\n"
    "p3's keyslot is pbkdf2 with a pinned iteration count because the loader's\n"
    "in-EFI unlock must open it; p4 keeps LUKS2's argon2id because Linux opens\n"
    "it. Both share one passphrase.\n"
    "\n"
    "--p3-plain drops the LUKS layer on p3 (Section 3). The stick then has no\n"
    "on-stick private key and so no signing appliance, and the loader asks for\n"
    "no passphrase; --key-out is required, because the key would otherwise have\n"
    "nowhere to live. p4 stays encrypted either way.\n"
    "\n"
    "--p2-fs/--p3-fs accept the filesystems the loader can actually read today\n"
    "(exfat for p2, ext4 for p3) and name the sanctioned-but-unwritten targets\n"
    "of Section 9 in the refusal.\n"
    "\n"
    "Needs root (loop devices, device-mapper, mount).\n",
    create_run
};
