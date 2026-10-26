/* falcon-boot mkstick: interactive `create`, the questions and what they
 * add up to (DESIGN Section 8).
 *
 * The dialogue's only product is a command line: the flags `create` takes,
 * built from the answers, parsed and run as if they had been typed. The
 * summary prints it, so every stick made here can be made again from flags.
 *
 * The streams are parameters, so a test drives it with a script.
 */

#ifndef FALCON_MKSTICK_CREATEUI_H
#define FALCON_MKSTICK_CREATEUI_H

#include <stddef.h>
#include <stdio.h>

#include <common/status.h>
#include <mkstick/disk.h>
#include <mkstick/drives.h>

#define CUI_MAX_UKI  8u
#define CUI_MAX_ARGS 64u

typedef struct {
    FILE            *in;
    FILE            *out;
    const drv_entry *drives;
    size_t           n_drives;
    const char      *prog;          /* the command name the summary prints */
    const char      *key_dir;       /* the default for --key-out */
} cui_io;

typedef struct {
    const drv_entry *drive;
    int  has_p3;
    int  has_p4;
    int  aa64;
    int  bios;
    /* Empty for the defaults. */
    char iso_size[32];               /* p2's size, when not half */
    char p1_size[32];
    char p3_size[32];
    char p2_fs[16];
    char p3_fs[16];
    char key_out[MKS_PATH_MAX];      /* --no-p3's key directory */
    char key[MKS_PATH_MAX];          /* a MOK reused, with its cert */
    char cert[MKS_PATH_MAX];
    char uki[CUI_MAX_UKI][MKS_PATH_MAX];
    size_t n_uki;

    /* cui_build: `create`'s arguments, and `bios`'s when bios is set. */
    char  *argv[CUI_MAX_ARGS];
    int    argc;
    char  *bios_argv[8];
    int    bios_argc;
    char   store[16384];
    size_t used;
} cui_plan;

/* The questions, from the drive to the advanced options. Fails at the end
 * of input, and when the drive's name is not typed back. */
cli_status cui_ask(const cui_io *io, cui_plan *p);

/* p->argv from the answers, then `extra` (options given on the command
 * line with no target, as typed), then the target. */
cli_status cui_build(cui_plan *p, const char *const *extra, size_t n_extra,
                     const char *stage3);

/* The summary and the command lines, then the question. 1 for yes. */
int cui_confirm(const cui_io *io, const cui_plan *p);

/* What to do at the machine the first time the stick boots. */
void cui_first_boot(FILE *out, const cui_plan *p);

#endif /* FALCON_MKSTICK_CREATEUI_H */
