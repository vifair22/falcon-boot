/* falcon-boot: the UKIs on a mounted shelf, and what the chooser calls them
 * (DESIGN Section 5). Shared by both loaders.
 *
 * The line: this module answers "which images are here, in what order, and
 * what does each row say". Unlocking the shelf, the chooser itself, and
 * booting the pick are the caller's. It was the UEFI loader's alone until
 * falcon-bios needed the same rows, which it had been drawing as bare file
 * names.
 *
 * Caller-allocated, as everywhere else in falcon-core. The list holds
 * directory and file caches read out of the shelf, which on an encrypted p3
 * is plaintext, so the caller wipes it (shelf_wipe) wherever it wipes the
 * rest of what the unlock produced.
 */

#ifndef FALCON_SHELF_H
#define FALCON_SHELF_H

#include <stddef.h>
#include <stdint.h>

#include <fs/fs.h>
#include <media/osrel.h>

/* How many images a chooser offers. A shelf built to DESIGN Section 7.5
 * holds the personal OS, a rollback copy or two, and the appliance. */
#define SHELF_MAX 8u

/* A row's label, in UCS-2 units including the terminator: the loaders'
 * menu row width. */
#define SHELF_LABEL_MAX 512u

/* How the signing-appliance image (DESIGN Section 7.5) is told apart from
 * every other UKI on the shelf: its file name starts with this, ASCII,
 * case-insensitively. A name rather than a manifest because p3 has no
 * manifest and the loader cannot write one; mkstick is what makes it true
 * when it stages the image. */
#define SHELF_APPLIANCE_PREFIX "appliance"

/* How much of each image's head is read to find its sections, and the most
 * of one section that is kept. A UKI's section table sits in its first few
 * hundred bytes; .osrel and .uname are a few hundred bytes at most. */
#define SHELF_HEAD_WINDOW 8192u
#define SHELF_SECT_MAX    4096u

typedef struct {
    fs_dirent ent;
    uint16_t  label[SHELF_LABEL_MAX];
    /* The kernel version out of the image's .uname, empty when it has none.
     * It is what separates two builds of one distro, and it is what a person
     * wants to read: "Arch Linux 7.2.6-arch2-1" beside "Arch Linux
     * 6.18.52-1-lts" says which is the LTS fallback; "(arch-usb.efi)" beside
     * "(arch-lts.efi)" makes them guess. */
    char      uname[OSREL_LABEL_MAX];
    uint8_t   named;   /* the label came from .osrel, not the file name */
    uint8_t   dup;     /* another row says the same thing */
    uint8_t   byver;   /* this row's qualifier is its version, not its name */
} shelf_uki;

typedef struct {
    shelf_uki uki[SHELF_MAX];
    size_t    n;
    /* Scratch. Two directory iterators because /EFI/Linux is two levels
     * down; a file for the sections; the head and one section. */
    fs_dir    dir_a;
    fs_dir    dir_b;
    fs_file   file;
    uint8_t   head[SHELF_HEAD_WINDOW];
    uint8_t   sect[SHELF_SECT_MAX];
} shelf_list;

/* Collects the regular *.efi files in /uki, or in /EFI/Linux when there is no
 * /uki, sorted by name. Anything else in there - a README, a subdirectory, a
 * file called ".efi" - is skipped rather than offered: a menu entry that
 * cannot be a kernel is worse than no entry. /uki is DESIGN Section 8's and
 * mkstick's; /EFI/Linux is the Boot Loader Specification's, so a shelf
 * populated by kernel-install still boots. Returns 1 when there is at least
 * one, 0 when there is none or neither directory is there, negative on a
 * filesystem error. */
int shelf_collect(shelf_list *s, fs_vol *fs);

/* Is this the signing appliance? */
int shelf_is_appliance(const fs_dirent *e);

/* Labels every row and puts them in chooser order.
 *
 * A row reads "Personal OS: " and the image's .osrel PRETTY_NAME (or NAME),
 * which is where sd-boot reads the name it shows, or its file name when it
 * has neither. Rows that would read the same are qualified by kernel version
 * when that separates them and by file name when it does not. The newest
 * kernel sorts first, because the row a chooser opens on is the one Enter
 * boots; the appliance sorts last, because it is never what an ordinary boot
 * is for. Every failure to read an image leaves its file name in place. */
void shelf_label(shelf_list *s, fs_vol *fs);

/* Wipes the list and its caches. Safe to call twice. */
void shelf_wipe(shelf_list *s);

#endif /* FALCON_SHELF_H */
