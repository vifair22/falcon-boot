/* falcon-boot mkstick: interactive `create`. createui.h has the shape. */

#include <mkstick/createui.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <common/util.h>
#include <mkstick/mkstick.h>

static cli_status copy_str(char *dst, size_t cap, const char *src)
{
    size_t len = strlen(src);

    if (len >= cap)
        return set_error(CLI_ERR_RANGE, "%.40s... is too long", src);
    memcpy(dst, src, len + 1u);
    return CLI_OK;
}

/* One answer: the line, its newline and surrounding blanks dropped. A line
 * longer than buf is read to its end and kept cut short. */
static cli_status get_line(const cui_io *io, const char *prompt, char *buf,
                           size_t cap)
{
    size_t len;
    size_t start = 0;

    (void)fputs(prompt, io->out);
    (void)fflush(io->out);
    if (fgets(buf, (int)cap, io->in) == NULL)
        return set_error(CLI_ERR_IO, "end of input: interactive create reads "
                         "its answers from stdin; nothing was written");
    len = strlen(buf);
    if (len > 0 && buf[len - 1] != '\n') {
        int c;

        while ((c = fgetc(io->in)) != EOF && c != '\n')
            ;
    }
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
                       buf[len - 1] == ' ' || buf[len - 1] == '\t'))
        buf[--len] = '\0';
    while (buf[start] == ' ' || buf[start] == '\t')
        start++;
    if (start > 0)
        memmove(buf, buf + start, len - start + 1u);
    return CLI_OK;
}

static cli_status ask_yn(const cui_io *io, const char *prompt, int dflt,
                         int *out)
{
    char buf[64];
    cli_status rc;

    for (;;) {
        rc = get_line(io, prompt, buf, sizeof buf);
        if (rc != CLI_OK)
            return rc;
        if (buf[0] == '\0') {
            *out = dflt;
            return CLI_OK;
        }
        if (strcmp(buf, "y") == 0 || strcmp(buf, "Y") == 0 ||
            strcmp(buf, "yes") == 0) {
            *out = 1;
            return CLI_OK;
        }
        if (strcmp(buf, "n") == 0 || strcmp(buf, "N") == 0 ||
            strcmp(buf, "no") == 0) {
            *out = 0;
            return CLI_OK;
        }
        (void)fputs("Answer y or n.\n", io->out);
    }
}

static cli_status ask_number(const cui_io *io, const char *prompt,
                             unsigned lo, unsigned hi, unsigned dflt,
                             unsigned *out)
{
    char buf[64];
    cli_status rc;

    for (;;) {
        char *end;
        unsigned long v;

        rc = get_line(io, prompt, buf, sizeof buf);
        if (rc != CLI_OK)
            return rc;
        if (buf[0] == '\0' && dflt != 0) {
            *out = dflt;
            return CLI_OK;
        }
        v = strtoul(buf, &end, 10);
        if (buf[0] != '\0' && *end == '\0' && v >= lo && v <= hi) {
            *out = (unsigned)v;
            return CLI_OK;
        }
        (void)fprintf(io->out, "Answer a number from %u to %u.\n", lo, hi);
    }
}

/* A size as --p1-size takes it; empty keeps the default. */
static cli_status ask_size(const cui_io *io, const char *prompt,
                           uint64_t floor_, char *out, size_t cap)
{
    char buf[64];
    cli_status rc;

    for (;;) {
        uint64_t v;
        char want[32];

        rc = get_line(io, prompt, buf, sizeof buf);
        if (rc != CLI_OK)
            return rc;
        if (buf[0] == '\0') {
            out[0] = '\0';
            return CLI_OK;
        }
        if (util_parse_size(buf, &v) != CLI_OK) {
            cli_error_clear();
            (void)fputs("Answer a size: 64M, 2G.\n", io->out);
            continue;
        }
        if (v < floor_) {
            (void)fprintf(io->out, "It needs %s or more.\n",
                          util_format_size(floor_, want, sizeof want));
            continue;
        }
        return copy_str(out, cap, buf);
    }
}

static cli_status ask_fs(const cui_io *io, const char *prompt,
                         const char *const *names, size_t n, char *out,
                         size_t cap)
{
    char buf[64];
    cli_status rc;
    size_t i;

    for (;;) {
        rc = get_line(io, prompt, buf, sizeof buf);
        if (rc != CLI_OK)
            return rc;
        if (buf[0] == '\0') {
            out[0] = '\0';
            return CLI_OK;
        }
        for (i = 0; i < n; i++) {
            if (strcmp(buf, names[i]) == 0) {
                /* The first name is the default: no flag. */
                return copy_str(out, cap, i == 0 ? "" : names[i]);
            }
        }
        (void)fputs("Answer one of the names listed.\n", io->out);
    }
}

static cli_status ask_file(const cui_io *io, const char *prompt, char *out,
                           size_t cap)
{
    cli_status rc;

    for (;;) {
        rc = get_line(io, prompt, out, cap);
        if (rc != CLI_OK)
            return rc;
        if (util_is_file(out))
            return CLI_OK;
        (void)fprintf(io->out, "There is no file at %s.\n", out);
    }
}

/* ---- the questions ---- */

static void show_drive(const cui_io *io, const drv_entry *d, unsigned num)
{
    char size[32];
    char psize[32];
    unsigned i;

    if (num != 0)
        (void)fprintf(io->out, "  %2u  ", num);
    else
        (void)fputs("   -  ", io->out);
    (void)fprintf(io->out, "%-9s %10s  %-30s", d->name,
                  util_format_size(d->size, size, sizeof size),
                  d->model[0] != '\0' ? d->model : "(no model)");
    if (d->busy[0] != '\0')
        (void)fprintf(io->out, " in use: %s", d->busy);
    else if (d->usb)
        (void)fputs(" USB", io->out);
    else if (d->removable)
        (void)fputs(" removable", io->out);
    (void)fputs("\n        ", io->out);
    if (d->n_parts == 0)
        (void)fputs("no partitions", io->out);
    for (i = 0; i < d->n_parts; i++)
        (void)fprintf(io->out, "%s%s %s", i == 0 ? "" : ", ", d->part[i],
                      util_format_size(d->part_size[i], psize,
                                       sizeof psize));
    (void)fputc('\n', io->out);
}

static cli_status ask_drive(const cui_io *io, cui_plan *p)
{
    const drv_entry *pick[DRV_MAX];
    unsigned n = 0;
    unsigned choice;
    char prompt[96];
    char buf[64];
    char size[32];
    cli_status rc;
    size_t i;

    (void)fputs("Drives, removable first:\n\n", io->out);
    for (i = 0; i < io->n_drives && n < DRV_MAX; i++) {
        const drv_entry *d = &io->drives[i];

        if (d->busy[0] == '\0') {
            pick[n++] = d;
            show_drive(io, d, n);
        } else {
            show_drive(io, d, 0);
        }
    }
    (void)fflush(io->out);
    if (n == 0)
        return set_error(CLI_ERR_NOTFOUND, "no drive here can be written; "
                         "plug in a stick, or name a device or image file: "
                         "%s create <target>", io->prog);

    (void)snprintf(prompt, sizeof prompt, "\nDrive to make the stick on "
                   "(1-%u): ", n);
    rc = ask_number(io, prompt, 1, n, 0, &choice);
    if (rc != CLI_OK)
        return rc;
    p->drive = pick[choice - 1u];

    (void)fprintf(io->out, "\nEverything on %s (%s, %s) is destroyed.\n",
                  p->drive->path,
                  p->drive->model[0] != '\0' ? p->drive->model : "no model",
                  util_format_size(p->drive->size, size, sizeof size));
    (void)snprintf(prompt, sizeof prompt, "Type %s to go on: ",
                   p->drive->name);
    rc = get_line(io, prompt, buf, sizeof buf);
    if (rc != CLI_OK)
        return rc;
    if (strcmp(buf, p->drive->name) != 0)
        return set_error(CLI_ERR_PERM, "%s was not typed back; nothing was "
                         "written", p->drive->name);
    return CLI_OK;
}

/* --no-p3's key directory. One that holds a MOK key already is that key,
 * reused, or another directory: create would write over it. */
static cli_status ask_key_dir(const cui_io *io, cui_plan *p)
{
    char prompt[MKS_PATH_MAX + 80];
    char key[MKS_PATH_MAX + 32];
    cli_status rc;

    for (;;) {
        int reuse;

        (void)snprintf(prompt, sizeof prompt, "Directory to keep the MOK key "
                       "and certificate in [%s]: ", io->key_dir);
        rc = get_line(io, prompt, p->key_out, sizeof p->key_out);
        if (rc != CLI_OK)
            return rc;
        if (p->key_out[0] == '\0') {
            rc = copy_str(p->key_out, sizeof p->key_out, io->key_dir);
            if (rc != CLI_OK)
                return rc;
        }
        (void)snprintf(key, sizeof key, "%s/%s", p->key_out, MKS_P3_KEY_NAME);
        if (!util_is_file(key))
            return CLI_OK;
        (void)fprintf(io->out, "%s already holds a MOK key.\n", p->key_out);
        rc = ask_yn(io, "Use it for this stick? [Y/n]: ", 1, &reuse);
        if (rc != CLI_OK)
            return rc;
        if (reuse) {
            if (snprintf(p->cert, sizeof p->cert, "%s/%s", p->key_out,
                         MKS_P3_CERT_NAME) >= (int)sizeof p->cert)
                return set_error(CLI_ERR_RANGE, "%s is too long",
                                 p->key_out);
            return copy_str(p->key, sizeof p->key, key);
        }
    }
}

static cli_status ask_layout(const cui_io *io, cui_plan *p)
{
    unsigned choice;
    cli_status rc;

    (void)fputs(
        "\nWhat goes on the stick beside p1 (the loaders) and p2 (the ISO "
        "library)?\n"
        "  1  a personal OS: p3, the encrypted shelf for UKIs, the signing\n"
        "     appliance and the MOK key, and p4, the OS's encrypted root\n"
        "  2  the shelf only: p3, no p4\n"
        "  3  neither\n"
        "Without p3 there is no personal OS, and unsigned media is blessed "
        "on this\n"
        "machine instead of on the stick, which matters only for unsigned "
        "images\n"
        "under Secure Boot. The MOK key is then kept on this machine.\n",
        io->out);
    rc = ask_number(io, "Choice [1]: ", 1, 3, 1, &choice);
    if (rc != CLI_OK)
        return rc;
    p->has_p3 = choice != 3u;
    p->has_p4 = choice == 1u;
    if (!p->has_p3)
        return ask_key_dir(io, p);
    return CLI_OK;
}

static uint64_t size_or(const char *text, uint64_t dflt)
{
    uint64_t v;

    if (text[0] == '\0')
        return dflt;
    if (util_parse_size(text, &v) != CLI_OK) {
        cli_error_clear();
        return dflt;
    }
    return v;
}

/* What p2 and p4 share, as create's plan_layout counts it. */
static uint64_t split_pool(const cui_plan *p)
{
    const uint64_t overhead = 2u * (uint64_t)MKS_ALIGN;
    uint64_t p1 = size_or(p->p1_size, MKS_DEFAULT_P1);
    uint64_t p3 = size_or(p->p3_size, MKS_DEFAULT_P3);
    uint64_t pool;

    if (p->drive->size <= overhead)
        return 0;
    pool = (p->drive->size - overhead) / MKS_ALIGN * MKS_ALIGN;
    return pool > p1 + p3 ? pool - p1 - p3 : 0;
}

static cli_status ask_split(const cui_io *io, cui_plan *p)
{
    uint64_t rest = split_pool(p);
    char text[32];
    char lo[32];
    char hi[32];
    cli_status rc;

    (void)fprintf(io->out, "\nThe space after p1 and p3, about %s, is split "
                  "between p2, the ISO\nlibrary, and p4, the personal OS.\n",
                  util_format_size(rest, text, sizeof text));
    for (;;) {
        uint64_t v;

        rc = get_line(io, "Size of p2, or half [half]: ", p->iso_size,
                      sizeof p->iso_size);
        if (rc != CLI_OK)
            return rc;
        if (p->iso_size[0] == '\0' || strcmp(p->iso_size, "half") == 0) {
            p->iso_size[0] = '\0';
            return CLI_OK;
        }
        if (util_parse_size(p->iso_size, &v) != CLI_OK) {
            cli_error_clear();
            (void)fputs("Answer a size (32G) or half.\n", io->out);
            continue;
        }
        if (v >= MKS_MIN_P2 && rest >= MKS_MIN_P4 && v <= rest - MKS_MIN_P4)
            return CLI_OK;
        (void)fprintf(io->out, "p2 can be from %s to %s.\n",
                      util_format_size(MKS_MIN_P2, lo, sizeof lo),
                      util_format_size(rest > MKS_MIN_P4 ? rest - MKS_MIN_P4
                                                         : 0,
                                       hi, sizeof hi));
    }
}

static cli_status ask_loaders(const cui_io *io, cui_plan *p)
{
    cli_status rc;

    (void)fputs("\nThe stick boots x86-64 UEFI machines.\n", io->out);
    rc = ask_yn(io, "Add the AArch64 loader, for 64-bit ARM UEFI machines? "
                "[Y/n]: ", 1, &p->aa64);
    if (rc != CLI_OK)
        return rc;
    return ask_yn(io, "Add the BIOS loader, for machines booting without "
                  "UEFI? [Y/n]: ", 1, &p->bios);
}

static cli_status ask_mok(const cui_io *io, cui_plan *p)
{
    unsigned choice;
    cli_status rc;

    if (p->key[0] != '\0')
        return CLI_OK;
    (void)fputs(
        "\nThe MOK is the key the loader is signed with; each machine with "
        "Secure\nBoot on enrolls it once.\n"
        "  1  make a new one\n"
        "  2  reuse one from another stick: a machine that enrolled it boots "
        "this\n"
        "     stick with no enrollment\n", io->out);
    rc = ask_number(io, "Choice [1]: ", 1, 2, 1, &choice);
    if (rc != CLI_OK || choice == 1u)
        return rc;
    rc = ask_file(io, "MOK private key (PEM): ", p->key, sizeof p->key);
    if (rc != CLI_OK)
        return rc;
    return ask_file(io, "Its certificate (PEM): ", p->cert, sizeof p->cert);
}

static cli_status ask_ukis(const cui_io *io, cui_plan *p)
{
    cli_status rc;

    if (!p->has_p3)
        return CLI_OK;
    (void)fputs("\nUKIs to sign and stage on p3, one path per line; an empty "
                "line ends the\nlist.\n", io->out);
    while (p->n_uki < CUI_MAX_UKI) {
        char *u = p->uki[p->n_uki];

        rc = get_line(io, "UKI: ", u, sizeof p->uki[0]);
        if (rc != CLI_OK)
            return rc;
        if (u[0] == '\0')
            return CLI_OK;
        if (!util_is_file(u)) {
            (void)fprintf(io->out, "There is no file at %s.\n", u);
            continue;
        }
        p->n_uki++;
    }
    return CLI_OK;
}

static cli_status ask_advanced(const cui_io *io, cui_plan *p)
{
    static const char *const p2_fs[] = { "exfat", "fat32" };
    static const char *const p3_fs[] = { "ext4", "fat32", "btrfs", "xfs" };
    int yes;
    cli_status rc;

    rc = ask_yn(io, "\nChange filesystems or partition sizes? [y/N]: ", 0,
                &yes);
    if (rc != CLI_OK || !yes)
        return rc;
    rc = ask_size(io, "p1 (ESP) size [64M]: ", MKS_MIN_P1, p->p1_size,
                  sizeof p->p1_size);
    if (rc == CLI_OK)
        rc = ask_fs(io, "p2 filesystem: exfat or fat32 [exfat]: ", p2_fs,
                    2, p->p2_fs, sizeof p->p2_fs);
    if (rc != CLI_OK || !p->has_p3)
        return rc;
    rc = ask_size(io, "p3 size [1G]: ", MKS_MIN_P3, p->p3_size,
                  sizeof p->p3_size);
    while (rc == CLI_OK) {
        rc = ask_fs(io, "p3 filesystem: ext4, fat32, btrfs or xfs [ext4]: ",
                    p3_fs, 4, p->p3_fs, sizeof p->p3_fs);
        if (rc != CLI_OK || strcmp(p->p3_fs, "xfs") != 0 ||
            size_or(p->p3_size, MKS_DEFAULT_P3) >= MKS_MIN_P3_XFS)
            break;
        (void)fputs("xfs needs a p3 of 320M or more.\n", io->out);
    }
    return rc;
}

cli_status cui_ask(const cui_io *io, cui_plan *p)
{
    cli_status rc;

    memset(p, 0, sizeof *p);
    rc = ask_drive(io, p);
    if (rc == CLI_OK)
        rc = ask_layout(io, p);
    if (rc == CLI_OK)
        rc = ask_loaders(io, p);
    if (rc == CLI_OK)
        rc = ask_mok(io, p);
    if (rc == CLI_OK)
        rc = ask_ukis(io, p);
    if (rc == CLI_OK)
        rc = ask_advanced(io, p);
    /* Last: what it splits is what p1 and p3 leave. */
    if (rc == CLI_OK && p->has_p4)
        rc = ask_split(io, p);
    return rc;
}

/* ---- the command line ---- */

static cli_status push(cui_plan *p, char **argv, int *argc, int max,
                       const char *s)
{
    size_t len = strlen(s) + 1u;

    if (*argc >= max || p->used + len > sizeof p->store)
        return set_error(CLI_ERR_RANGE, "too many arguments for "
                         "interactive create");
    memcpy(p->store + p->used, s, len);
    argv[(*argc)++] = p->store + p->used;
    p->used += len;
    return CLI_OK;
}

static cli_status arg(cui_plan *p, const char *s)
{
    return push(p, p->argv, &p->argc, (int)CUI_MAX_ARGS, s);
}

static cli_status opt(cui_plan *p, const char *name, const char *value)
{
    cli_status rc = CLI_OK;

    if (value[0] != '\0') {
        rc = arg(p, name);
        if (rc == CLI_OK)
            rc = arg(p, value);
    }
    return rc;
}

cli_status cui_build(cui_plan *p, const char *const *extra, size_t n_extra,
                     const char *stage3)
{
    const int bmax = (int)(sizeof p->bios_argv / sizeof p->bios_argv[0]);
    cli_status rc;
    size_t i;

    p->argc = 0;
    p->bios_argc = 0;
    p->used = 0;
    rc = arg(p, "--force");
    if (rc == CLI_OK && !p->has_p3)
        rc = arg(p, "--no-p3");
    else if (rc == CLI_OK && !p->has_p4)
        rc = arg(p, "--no-p4");
    if (rc == CLI_OK)
        rc = opt(p, "--key-out", p->key_out);
    if (rc == CLI_OK)
        rc = opt(p, "--p1-size", p->p1_size);
    if (rc == CLI_OK)
        rc = opt(p, "--iso-size", p->iso_size);
    if (rc == CLI_OK)
        rc = opt(p, "--p2-fs", p->p2_fs);
    if (rc == CLI_OK)
        rc = opt(p, "--p3-size", p->p3_size);
    if (rc == CLI_OK)
        rc = opt(p, "--p3-fs", p->p3_fs);
    if (rc == CLI_OK && !p->aa64)
        rc = arg(p, "--no-aa64");
    if (rc == CLI_OK)
        rc = opt(p, "--key", p->key);
    if (rc == CLI_OK)
        rc = opt(p, "--cert", p->cert);
    for (i = 0; rc == CLI_OK && i < p->n_uki; i++)
        rc = opt(p, "--uki", p->uki[i]);
    for (i = 0; rc == CLI_OK && i < n_extra; i++)
        rc = arg(p, extra[i]);
    if (rc == CLI_OK)
        rc = arg(p, p->drive->path);
    if (rc != CLI_OK || !p->bios)
        return rc;

    rc = push(p, p->bios_argv, &p->bios_argc, bmax, "--no-backup");
    if (rc == CLI_OK && stage3 != NULL)
        rc = push(p, p->bios_argv, &p->bios_argc, bmax, "--stage3");
    if (rc == CLI_OK && stage3 != NULL)
        rc = push(p, p->bios_argv, &p->bios_argc, bmax, stage3);
    if (rc == CLI_OK)
        rc = push(p, p->bios_argv, &p->bios_argc, bmax, p->drive->path);
    return rc;
}

/* One argument as a shell reads it back: bare when it is plain, in single
 * quotes otherwise. */
static void put_word(FILE *out, const char *s)
{
    const char *c;
    int plain = s[0] != '\0';

    for (c = s; *c != '\0'; c++) {
        if (strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                   "0123456789-_./=:+,@%", *c) == NULL)
            plain = 0;
    }
    (void)fputc(' ', out);
    if (plain) {
        (void)fputs(s, out);
        return;
    }
    (void)fputc('\'', out);
    for (c = s; *c != '\0'; c++) {
        if (*c == '\'')
            (void)fputs("'\\''", out);
        else
            (void)fputc(*c, out);
    }
    (void)fputc('\'', out);
}

static void put_command(FILE *out, const char *prog, const char *cmd,
                        char *const *argv, int argc)
{
    int i;

    (void)fprintf(out, "  %s %s", prog, cmd);
    for (i = 0; i < argc; i++)
        put_word(out, argv[i]);
    (void)fputc('\n', out);
}

int cui_confirm(const cui_io *io, const cui_plan *p)
{
    char size[32];
    size_t i;
    int yes = 0;

    (void)fprintf(io->out, "\nThe stick:\n"
                  "  drive    %s, %s, %s: everything on it is destroyed\n",
                  p->drive->path,
                  p->drive->model[0] != '\0' ? p->drive->model : "no model",
                  util_format_size(p->drive->size, size, sizeof size));
    (void)fprintf(io->out, "  p1       ESP, %s\n",
                  p->p1_size[0] != '\0' ? p->p1_size : "64M");
    (void)fprintf(io->out, "  p2       ISO library, %s, %s\n",
                  p->p2_fs[0] != '\0' ? p->p2_fs : "exfat",
                  !p->has_p4 ? "the rest"
                  : p->iso_size[0] != '\0' ? p->iso_size
                  : "half the rest");
    if (p->has_p3)
        (void)fprintf(io->out, "  p3       shelf, LUKS2 and %s, %s\n",
                      p->p3_fs[0] != '\0' ? p->p3_fs : "ext4",
                      p->p3_size[0] != '\0' ? p->p3_size : "1G");
    if (p->has_p4)
        (void)fprintf(io->out, "  p4       personal OS, LUKS2, %s\n",
                      p->iso_size[0] != '\0' ? "the rest" : "half the rest");
    (void)fprintf(io->out, "  loaders  x86-64 UEFI%s%s\n",
                  p->aa64 ? ", AArch64 UEFI" : "", p->bios ? ", BIOS" : "");
    if (p->key[0] != '\0')
        (void)fprintf(io->out, "  MOK      %s\n", p->key);
    else
        (void)fputs("  MOK      new\n", io->out);
    if (p->key_out[0] != '\0')
        (void)fprintf(io->out, "  key copy %s\n", p->key_out);
    for (i = 0; i < p->n_uki; i++)
        (void)fprintf(io->out, "  UKI      %s\n", p->uki[i]);

    (void)fputs("\nThe same stick from the command line:\n", io->out);
    put_command(io->out, io->prog, "create", p->argv, p->argc);
    if (p->bios)
        put_command(io->out, io->prog, "bios", p->bios_argv, p->bios_argc);

    if (ask_yn(io, "\nWrite the stick? [y/N]: ", 0, &yes) != CLI_OK) {
        cli_error_clear();
        return 0;
    }
    return yes;
}

void cui_first_boot(FILE *out, const cui_plan *p)
{
    (void)fputs("\nFirst boot on a machine with Secure Boot on (Manual "
                "Section 3.1):\n", out);
    if (p->key[0] != '\0')
        (void)fputs("  A machine that enrolled this MOK boots the stick "
                    "as it is. Any other:\n", out);
    (void)fprintf(out,
        "  1. shim says \"Verification failed: (0x1A) Security "
        "Violation\". Press Enter\n"
        "     on OK.\n"
        "  2. MokManager counts down from 10. Press a key before it ends, "
        "or the\n"
        "     loader fails again and step 1 comes round.\n"
        "  3. Choose \"Enroll key from disk\", then FALCONESP, then %s.\n"
        "  4. Choose Continue, then Yes, then Reboot.\n"
        "  The stick boots from then on, on that machine.\n"
        "With Secure Boot off%s, nothing is enrolled.\n",
        MKS_ESP_CERT_NAME, p->bios ? ", or on a BIOS machine" : "");
}
