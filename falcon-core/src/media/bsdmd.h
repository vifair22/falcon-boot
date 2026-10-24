/* falcon-boot: making a BSD ISO boot itself as a memory disk
 * (DESIGN Section 6.4).
 *
 * A FreeBSD kernel loses its install medium at ExitBootServices exactly as a
 * Linux one does, and falcon-hook cannot help: it is an initramfs mechanism.
 * What FreeBSD's loader can do instead is load a file as a memory disk and
 * mount root from it, which needs no kernel module and nothing per version:
 *
 *     md_image_load="YES"
 *     md_image_type="md_image"
 *     md_image_name="<path>"
 *     vfs.root.mountfrom="cd9660:/dev/md0"
 *
 * The file it loads is the ISO. The question is where the loader can read
 * that file from, and the answer here is: from the ISO. A second directory
 * record is added to the medium naming a file whose extent is LBA 0 and whose
 * length is the whole image, so the ISO contains itself, and the loader reads
 * it off the same virtual CD it booted from. Nothing else is published and
 * nothing is copied.
 *
 * Two edits, both of which fit the byte-range overlay the virtual CD already
 * serves (falcon-efi/src/iso/overlay.h), so no ISO is ever written to:
 *
 *   1. bsdmd_record: one directory record appended into the free bytes at the
 *      end of a directory extent. No record moves and no length changes.
 *   2. bsdmd_config: the four settings above, written over the opening
 *      comment block of the ISO's /boot/defaults/loader.conf and padded back
 *      to exactly the length it replaced. The same in-place trick D3b uses
 *      for grub.cfg. /boot/loader.conf is the wrong file: it is a few
 *      hundred bytes with no room, and defaults are read first anyway.
 */

#ifndef FALCON_BSDMD_H
#define FALCON_BSDMD_H

#include <stddef.h>
#include <stdint.h>

enum {
    BSDMD_OK          = 0,
    BSDMD_ERR_ARG     = -1,
    BSDMD_ERR_IO      = -2,
    BSDMD_ERR_NOT_ISO = -3,  /* no ISO9660 primary volume descriptor */
    BSDMD_ERR_NO_ROOM = -4,  /* no directory extent has spare bytes */
    BSDMD_ERR_TOO_BIG = -5   /* past what an ISO9660 length field can say */
};

/* The ISO's bytes, at off for len. Zero on success. */
typedef int (*bsdmd_read_fn)(void *ctx, uint64_t off, uint64_t len,
                             void *buf);

/* The file name the added record carries. ISO9660 names are upper case and
 * carry a version, and the loader matches the ISO name verbatim for a record
 * with no Rock Ridge name of its own: measured, `ls /` at the loader prompt
 * showed `FALCONBS.IMG;1` beside the Rock Ridge names of everything else, and
 * `md_image_name` without the `;1` did not resolve. */
#define BSDMD_ISO_NAME  "FALCONBS.IMG;1"

/* A directory record is 33 bytes plus the name, padded to even. */
#define BSDMD_REC_MAX   64u
/* Longer than any opening comment block met so far (442 bytes on FreeBSD,
 * pfSense and OPNsense alike). */
#define BSDMD_CFG_MAX   768u
/* "/boot/" + the name + a terminator. */
#define BSDMD_PATH_MAX  32u

typedef struct {
    uint64_t off;                   /* byte offset in the ISO */
    uint32_t len;
    uint8_t  bytes[BSDMD_REC_MAX];
} bsdmd_record_edit;

/* Builds the directory record that names the ISO as a file inside itself,
 * and the path the loader is to load it from.
 *
 * The root directory is tried first and /boot second, because a record may
 * live anywhere the loader can walk to and the root's last extent sector is
 * occasionally full. Reads the ISO through read/ctx; writes nothing.
 *
 * extent is the sector the file's bytes start at: 0, the medium itself, or
 * a window past its end that serves the medium again. FreeBSD's loader
 * (libsa cd9660.c) zeroes a file's block cache, so a first read of sector 0
 * is taken for a cached block that was never read and copied from a NULL
 * buffer; a caller that can serve such a window gives its sector.
 */
int bsdmd_record(uint64_t iso_size, uint32_t extent, bsdmd_read_fn read,
                 void *ctx, bsdmd_record_edit *out, char *path,
                 size_t path_cap);

/* Writes the four settings over loader.conf's opening comment block.
 *
 * head is the file's first bytes (head_len of them, at least the comment
 * block's length; 1 KiB is plenty). The result is exactly as long as the
 * block it replaces, padded with a comment, and *len_out says how long that
 * is. path is what bsdmd_record returned.
 */
/* `console` is the value for loader.conf's console setting, or NULL to write
 * none.
 *
 * The two loaders pass different things, and the difference is the platform's
 * rather than ours. Under UEFI the firmware publishes a console that
 * FreeBSD's EFI loader inherits, so the installer already talks on whatever
 * the machine has and naming one here would override the firmware's choice.
 * A BIOS machine publishes nothing: FreeBSD's BIOS loader defaults to
 * vidconsole, the installer draws to the screen alone, and a serial log -
 * which is the only thing an automated run can read - stays empty.
 * falcon-bios therefore asks for "comconsole,vidconsole" and falcon-efi for
 * nothing. */
int bsdmd_config(const char *path, const char *console,
                 const char *head, size_t head_len,
                 char *out, size_t cap, uint32_t *len_out);

#endif /* FALCON_BSDMD_H */
