/* falcon-bios: booting the personal OS from the shelf. See pos.c. */

#ifndef FALCON_BIOS_POS_H
#define FALCON_BIOS_POS_H

#include <start/bios.h>
#include <result.h>
#include <part/table.h>
#include <ident/ident.h>
#include <fs/fs.h>
#include <media/shelf.h>

/* The user backed out, at the prompt or at the chooser. Nothing is on screen
 * worth a keypress. */
#define POS_ABORTED 1

/* Unlocks the shelf (a plaintext one needs no unlocking), offers the UKIs
 * on it, and boots the one chosen, drawing in the page the caller started. Does not return when the boot happens.
 * POS_ABORTED when the user backed out; -1 with *r filled in when it failed.
 * Every secret is wiped either way. */
int pos_run(bios_disk *d, const part_entry *shelf, boot_result *r);

/* pos_run, but when `want` is not NULL the image booted is the first it
 * says yes to, without the chooser (the station's rows for one UKI, DESIGN
 * Section 13.3); none is a failure saying so. */
typedef int (*pos_pick_fn)(const fs_dirent *uki, void *ctx);
int pos_run_pick(bios_disk *d, const part_entry *shelf, pos_pick_fn want,
                 void *ctx, boot_result *r);

/* A plaintext shelf's images, labelled and in chooser order, each handed to
 * `fn` (its file name and label are the caller's to copy; the list is wiped
 * after). The count, or -1 when the shelf is not a plaintext one this
 * mounts: an encrypted shelf cannot be listed before it is unlocked. */
typedef void (*pos_list_fn)(const shelf_uki *uki, void *ctx);
int pos_list(bios_disk *d, const part_entry *shelf, pos_list_fn fn,
             void *ctx);

/* Whether the shelf opens as LUKS2, or mounts as a plaintext filesystem, so
 * the menu knows whether to offer the row. Reads a header or a superblock;
 * asks for nothing. Which partition is the install's shelf is the caller's
 * to decide, by its record (disk/install.h). */
int pos_probe(bios_disk *d, const part_entry *shelf);

/* The install's boot record, handed on to every image booted as the
 * IDENT_CRED credential (DESIGN Section 3.1), as the UEFI loader does. NULL
 * hands none. */
void pos_set_install(const ident_rec *rec);

/* The row's label, and the first line of its screen: which kind of shelf
 * the last pos_probe opened. */
const uint16_t *pos_entry_label(void);

#endif /* FALCON_BIOS_POS_H */
