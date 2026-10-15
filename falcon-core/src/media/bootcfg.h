/* falcon-boot: an ISO's own boot configuration, read (docs/DESIGN.md
 * Section 6.4).
 *
 * A Linux ISO says how it boots: which kernel, which initramfs, which
 * cmdline. Reading that instead of keeping a per-family table is what lets
 * the loader boot an ISO's kernel directly (M2) and lets the bless tool wrap
 * the right one with the right arguments, for families nobody has written a
 * row for. The per-family tables become the fallback.
 *
 * Three formats, tried in this order:
 *
 *   GRUB       grub.cfg and loopback.cfg: menuentry blocks with linux/initrd
 *              lines, set/assignment for variables, source, configfile and
 *              eval followed, submenus stepped into, a menuentry's own
 *              arguments given to its body as $1, $2 and on. Not a GRUB
 *              interpreter: of the conditions only [ -z "$v" ], [ -n "$v" ]
 *              and string comparisons are evaluated (the rest read both
 *              branches, and what such a branch assigns is unknown once it
 *              ends); a for loop runs its body per word when the words are
 *              known, while/until loops and function bodies are skipped;
 *              and a value GRUB computes at run time (search --set, probe
 *              --set, a glob, and anything assigned from one) marks the
 *              entry that uses it. An unset variable expands to nothing, as
 *              in GRUB. What it reads is the shape every distro's generated
 *              config has.
 *   systemd-boot  loader/entries/NAME.conf: title, linux, initrd, options.
 *   isolinux   isolinux.cfg / syslinux.cfg: LABEL blocks with KERNEL or
 *              LINUX, APPEND (initrd= inside it) and INITRD, DEFAULT, and
 *              INCLUDE followed.
 *
 * Two kinds of entry matter, and a caller asks for one:
 *
 *   BOOTCFG_CD    how the ISO boots from its own CD: the default entry of
 *                 grub.cfg, systemd-boot or isolinux. Right when something
 *                 recreates the CD for the booted kernel (falcon-hook, M1).
 *   BOOTCFG_LOOP  how the ISO boots as a FILE on another filesystem: an
 *                 entry from GRUB's loopback.cfg convention, whose cmdline
 *                 carries ${iso_path} for the distro's own find-the-ISO
 *                 argument. Only taken when ${iso_path} is its one run-time
 *                 variable.
 *
 * ${iso_path} is kept in the cmdline as the literal token BOOTCFG_ISO_TOKEN;
 * bootcfg_render substitutes it, or drops it when there is no ISO path to
 * give (a CD boot, where GRUB leaves it empty).
 *
 * EFI-free, no allocator: every file is read into caller scratch through a
 * callback, and the result is a fixed-size struct. The input is off p2 and
 * trusted for nothing: every copy is bounded, includes are bounded in depth
 * and count, and an unterminated block ends at end of file.
 */

#ifndef FALCON_BOOTCFG_H
#define FALCON_BOOTCFG_H

#include <stddef.h>
#include <stdint.h>

enum {
    BOOTCFG_OK          = 0,
    BOOTCFG_ERR_BAD_ARG = -1,
    BOOTCFG_ERR_NONE    = -2, /* no configuration, or no usable entry */
    BOOTCFG_ERR_NOSPACE = -3  /* scratch too small for a config file */
};

#define BOOTCFG_TITLE_MAX   96u
#define BOOTCFG_PATH_MAX    192u
#define BOOTCFG_CMDLINE_MAX 1024u
#define BOOTCFG_INITRD_MAX  4u
#define BOOTCFG_ENTRIES_MAX 64u

/* ${iso_path} as it survives in a parsed cmdline. Not a string any distro's
 * cmdline contains. */
#define BOOTCFG_ISO_TOKEN "@@FALCON_ISO_PATH@@"

typedef enum {
    BOOTCFG_CD   = 0,
    BOOTCFG_LOOP = 1
} bootcfg_kind;

typedef enum {
    BOOTCFG_FMT_GRUB = 0,
    BOOTCFG_FMT_SDBOOT,
    BOOTCFG_FMT_ISOLINUX
} bootcfg_format;

typedef struct {
    char     title[BOOTCFG_TITLE_MAX];
    char     kernel[BOOTCFG_PATH_MAX];      /* absolute, device prefix gone */
    char     initrd[BOOTCFG_INITRD_MAX][BOOTCFG_PATH_MAX];
    uint8_t  n_initrd;
    char     cmdline[BOOTCFG_CMDLINE_MAX];  /* may hold BOOTCFG_ISO_TOKEN */
    uint8_t  nested;       /* inside a submenu: never the default */
    uint8_t  unresolved;   /* the cmdline used a value only GRUB at run
                              time could compute (search/probe --set) */
    uint8_t  kernel_unresolved; /* so did the kernel or an initrd path */
    uint8_t  truncated;    /* something did not fit its field */
    uint8_t  loop;         /* came from a loopback.cfg */
    bootcfg_format format;
} bootcfg_entry;

/* Where the configuration comes from. Paths are absolute, '/'-separated,
 * and looked up case-insensitively by the provider, as family.h's are. */
typedef struct {
    /* Reads the whole file into buf (at most cap bytes). *len receives its
     * length. 0 on success; nonzero when there is no such file, when it is
     * not a regular file, or when it does not fit (a config file larger
     * than cap is not one this module wants). */
    int (*read)(void *ctx, const char *path, char *buf, size_t cap,
                size_t *len);
    /* Lists a directory's regular files, one call of cb per name, in any
     * order; cb returning nonzero stops the listing. 0 on success, nonzero
     * when there is no such directory. May be NULL, and then systemd-boot
     * entries are not looked for. */
    int (*list)(void *ctx, const char *dir,
                int (*cb)(void *arg, const char *name), void *arg);
    /* Nonzero when a regular file exists at path. May be NULL; when given,
     * an entry is only picked if its kernel and every initrd exist, which is
     * what keeps a path built from a variable the parser could not know
     * ("/boot/vmlinuz-" from a loop over kernels) from being booted. */
    int (*exists)(void *ctx, const char *path);
    void *ctx;
} bootcfg_src;

typedef struct {
    bootcfg_entry entry[BOOTCFG_ENTRIES_MAX];
    size_t        n;
    /* Which entry a boot takes by default for the requested kind, or
     * BOOTCFG_ENTRIES_MAX when none qualifies. */
    size_t        pick;
    char          source[BOOTCFG_PATH_MAX];   /* the file it was read from */
} bootcfg_result;

/* Where GRUB's configuration sits on a CD, in the order bootcfg looks:
 * NULL-terminated. For a caller that needs the file GRUB reads even when the
 * entry came from another format (MX's GRUB entries name their kernel
 * through a run-time variable, so its CD entry is the isolinux one). */
const char *const *bootcfg_grub_cd_paths(void);

/* Reads the ISO's configuration for one kind of boot. scratch holds the
 * config files as they are parsed (several at once, while a source is
 * followed); 256 KiB is ample for every medium in the corpus.
 *
 * BOOTCFG_OK with res->pick set to the default entry; BOOTCFG_ERR_NONE when
 * nothing usable was found for that kind. res->n entries are listed either
 * way, for a menu that wants to offer the others. */
int bootcfg_read(const bootcfg_src *src, bootcfg_kind kind, char *scratch,
                 size_t scratch_len, bootcfg_result *res);

/* The entry's cmdline with BOOTCFG_ISO_TOKEN replaced by iso_path, or, when
 * iso_path is NULL, with the token removed along with the argument it is the
 * value of if that argument is left empty ("findiso=" disappears whole).
 * Runs of spaces collapse to one and the ends are trimmed. BOOTCFG_OK, or
 * BOOTCFG_ERR_NOSPACE when out is too small. */
int bootcfg_render(const bootcfg_entry *e, const char *iso_path, char *out,
                   size_t cap);

/* Nonzero when the entry could be what a boot of the ISO means: it names a
 * kernel, and not one of the tools install media carries beside the OS (a
 * memory tester, hardware detection, a .c32 module). The rule bootcfg_read
 * picks by, for a caller that judges every entry (media/biosboot.c). */
int bootcfg_entry_bootable(const bootcfg_entry *e);

/* Nonzero when the entry's cmdline carries ${iso_path}: it finds its own ISO
 * as a file, given the path. */
int bootcfg_takes_iso_path(const bootcfg_entry *e);

#endif /* FALCON_BOOTCFG_H */
