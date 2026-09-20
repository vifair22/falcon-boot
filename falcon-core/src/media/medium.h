/* falcon-boot: a medium as the set of filesystems in it
 * (docs/notes/iso-reader-route.md stage C).
 *
 * A CD is not one filesystem. It is a primary ISO9660 tree, possibly a
 * supplementary Joliet one, possibly Rock Ridge over the primary one,
 * possibly a UDF volume in the same bytes, and possibly an El Torito boot
 * image that is a filesystem of its own at some offset. Under UEFI the
 * firmware's partition driver turns that into a set of handles, which is why
 * the menu's survey publishes a virtual block device and calls
 * ConnectController for every entry it labels. This module does the same job
 * in code we own, over one read callback.
 *
 * ---- this is the per-platform boundary ----
 *
 * Under UEFI, `read` and `ctx` are an open fs_file on the exFAT partition.
 * Under BIOS they are INT 13h behind the same signature
 * (docs/notes/bios-route.md Section 1). Nothing below can tell the
 * difference, which is the property a BIOS build needs and the reason this
 * module is in falcon-core rather than in the loader.
 *
 * ---- what it does NOT do ----
 *
 * It does not decide which volume to boot, and it does not answer what the
 * medium is. The volume set is a fact; classification is family_detect's job
 * over bootfs_probe, and the caller walks the volumes for it. Keeping those
 * apart is what lets a caller ask about a name space the loader would not
 * boot from, which Windows media requires: oscdimg leaves the ISO9660 name
 * space holding a single stub README.TXT and puts install.wim in the UDF one.
 *
 * ---- precedence ----
 *
 * Volumes come back in the order a consumer should prefer them:
 *
 *   1. UDF, when the medium has one. A medium that carries UDF puts its real
 *      content there.
 *   2. ISO9660. Which name space inside it is iso9660.c's decision and is
 *      already made: Rock Ridge when the root's "." record opens with a SUSP
 *      "SP" entry, else Joliet when a supplementary descriptor carries one of
 *      the three UCS-2 escapes, else the primary tree as recorded.
 *   3. The El Torito boot image, and only for questions about the boot image.
 *      It is where /EFI/BOOT/BOOTX64.EFI actually lives.
 *
 * A medium that is none of those (a .img holding a bare FAT or exFAT volume,
 * say) gets one volume identified by the registry at offset zero.
 */

#ifndef FALCON_MEDIUM_H
#define FALCON_MEDIUM_H

#include <stddef.h>
#include <stdint.h>

#include <fs/fs.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    MEDIUM_OK          =  0,
    MEDIUM_ERR_ARG     = -1,
    MEDIUM_ERR_IO      = -2,
    MEDIUM_ERR_NOTHING = -3   /* nothing on it could be mounted */
};

/* How many volumes are kept.
 *
 * Four, which is ISO_MAX_VOLUMES in falcon-efi/src/iso/iso.c, so stage D
 * changes no cap. The most any corpus medium yields is two (an ISO9660 or
 * UDF tree plus the El Torito image); the spare room is for a medium with
 * both trees AND a boot image, plus one. */
#define MEDIUM_MAX_VOLS 4u

typedef enum {
    MEDIUM_VOL_ISO9660 = 0,  /* the primary tree, in whichever name space */
    MEDIUM_VOL_UDF     = 1,
    MEDIUM_VOL_BOOT    = 2,  /* the El Torito boot image */
    MEDIUM_VOL_WHOLE   = 3   /* not an ISO: one filesystem filling the medium */
} medium_vol_kind;

/* The offset shim. Each volume reads through one of these, which turns a
 * medium-relative callback into a volume-relative one by adding `base`. It is
 * the only new plumbing the idea needs. */
typedef struct {
    fs_read_fn parent;
    void      *parent_ctx;
    uint64_t   base;
} medium_src;

/* The read callback every volume uses: forwards to its parent with `base`
 * added, and refuses an offset that would wrap.
 *
 * Exported because a caller that wants a volume's raw bytes rather than its
 * files (hashing an El Torito boot image, say) needs exactly this shim, and a
 * second implementation of "add the base" is a second chance to get it wrong.
 * Pass a medium_src as ctx. */
int medium_src_read(void *ctx, uint64_t off, uint64_t len, void *buf);

typedef struct {
    medium_src      src;
    const fs_ops   *ops;
    medium_vol_kind kind;
    uint64_t        base;   /* byte offset of this volume in the medium */
    fs_vol          vol;
} medium_vol;

/* A medium and everything mountable in it.
 *
 * THIS OBJECT MUST NOT BE MOVED OR COPIED once medium_open has succeeded. A
 * mounted fs_vol holds the address of its own medium_src, which lives inside
 * this struct, so a copy leaves the copy's volumes reading through the
 * original's shim and a stack object that goes out of scope leaves them
 * reading freed memory. It is about 17 KB, so it wants to be a static or a
 * caller-owned allocation anyway; falcon-efi/src/iso/iso.c already keeps its
 * probe volume that way for the same reason.
 *
 * The caller's `read`/`ctx` must also outlive it: nothing here copies the
 * medium, and every volume reads through that callback on every access. */
typedef struct {
    fs_read_fn read;
    void      *ctx;
    uint64_t   size;          /* 0 when the caller could not say */
    medium_vol vol[MEDIUM_MAX_VOLS];
    unsigned   n;
    /* Index of the El Torito boot image, or n when there is none. */
    unsigned   boot;
} medium;

/* Mounts everything mountable in the medium reachable through `read`.
 *
 * `size` is the medium's length in bytes, used to refuse offsets past the end
 * before asking the callback for them; pass 0 when it is not known and the
 * callback's own failure becomes the only bound.
 *
 * MEDIUM_ERR_NOTHING is an ordinary answer about a medium, not damage: a file
 * that is not an image of anything we read yields it, and a caller should
 * treat it as "nothing to say about this". *m is zeroed on every failure.
 *
 * Volumes are mounted eagerly, which is what the firmware's ConnectController
 * does and what a survey wants: the caller is about to ask every one of them
 * a question. */
int medium_open(medium *m, fs_read_fn read, void *ctx, uint64_t size);

/* How many volumes were mounted. Zero only when medium_open failed. */
unsigned medium_count(const medium *m);

/* The mounted volume at `i`, or NULL when `i` is past the set. The fs_vol is
 * the caller's to use with the registry (fs_lookup, bootfs_probe,
 * bootfs_src); it stays mounted until the medium is reopened. */
fs_vol *medium_vol_at(medium *m, unsigned i);

/* What kind of volume `i` is, for a caller that treats the boot image
 * differently from a name space. MEDIUM_VOL_WHOLE for an index past the set,
 * which no caller should be asking about. */
medium_vol_kind medium_kind_at(const medium *m, unsigned i);

/* Byte offset of volume `i` within the medium. Needed by anything that has to
 * turn a volume-relative extent into a medium-relative one, which is what an
 * extent map handed to falcon-hook is. */
uint64_t medium_base_at(const medium *m, unsigned i);

/* The El Torito boot image's volume, or NULL when the medium has none. */
fs_vol *medium_boot_vol(medium *m);

#endif /* FALCON_MEDIUM_H */
