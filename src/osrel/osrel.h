/* falcon-boot: display labels read from a UKI's .osrel section.
 *
 * A UKI carries an os-release snippet in a PE section called ".osrel", put
 * there by ukify from whatever --os-release was given. This is not an
 * invention of ours: it is how systemd-boot labels Type #2 entries, so
 * reading the same section makes our menu agree with what sd-boot would show,
 * and kernel-install keeps it current without anyone maintaining a second
 * copy of the name.
 *
 * Why not the alternatives. A sidecar file (arch-usb.efi.title) is another
 * artifact to create and keep in step, and it can drift from the image it
 * names. Prettifying the filename cannot tell two kernels apart or say which
 * image is the signing appliance. The section is INTRINSIC: rebuild the UKI
 * and the label follows.
 *
 * EFI-free on purpose, like the filesystem drivers: it takes a buffer and a
 * length and has no idea where they came from, which is what makes it
 * testable off the firmware.
 *
 * THIS IS UX ONLY. Nothing here may influence what is loaded or verified, in
 * the same way src/sb reads Secure Boot state purely to print a banner
 * (DESIGN Section 7.3). A malformed section changes the text on the screen
 * and nothing else: every failure is OSREL_ERR_* and every caller falls back
 * to the filename rather than refusing the entry.
 */

#ifndef FALCON_OSREL_H
#define FALCON_OSREL_H

#include <stddef.h>
#include <stdint.h>

/* Longest label worth carrying. PRETTY_NAME values in the wild are short
 * ("Arch Linux", "Fedora Linux 41 (Workstation Edition)"); anything past this
 * is truncated rather than refused, because a clipped name is still a better
 * answer than a filename. */
#define OSREL_LABEL_MAX 64u

enum {
    OSREL_OK = 0,
    OSREL_ERR_BAD_ARG = -1,   /* NULL buffer, or no room to write a label */
    OSREL_ERR_NOT_PE = -2,    /* not a PE32+ image we recognise */
    OSREL_ERR_NO_SECTION = -3,/* no .osrel section in the image */
    OSREL_ERR_NO_NAME = -4    /* section present but carries no usable key */
};

/* Writes a display label for the image in buf into out.
 *
 * PRETTY_NAME wins; NAME is the fallback, with VERSION_ID appended when it is
 * present and not already part of NAME. That is systemd-boot's own order, so
 * an image labelled one way there is labelled the same way here.
 *
 * buf must be the image's FILE bytes, not a loaded image: section contents are
 * found through PointerToRawData, not VirtualAddress.
 *
 * out is NUL-terminated on success and left empty on every failure, so a
 * caller that ignores the return value still cannot print a half-parsed name.
 */
int osrel_label(const void *buf, size_t len, char *out, size_t cap);

/* Locates the .osrel section's FILE range without reading it.
 *
 * Split out because the loader must not read a 90 MB UKI to draw a menu row.
 * It reads a small window off the front, calls this to learn where the section
 * is, then reads exactly that section.
 *
 * `len` is how many bytes of the file are in buf; `total` is the file's real
 * length. They differ for exactly the caller this exists for, and keeping them
 * apart is the whole contract: the headers and section table are READ here and
 * must be inside buf, while the section content is merely LOCATED and is
 * expected to be outside it. A real UKI puts .osrel about 130 KB in behind a
 * section table at byte 392, so a window of a few KB finds it and cannot
 * contain it.
 *
 * Pass total == len when the whole file is in buf. total < len is
 * OSREL_ERR_BAD_ARG; a window too short to decide is OSREL_ERR_NOT_PE, which
 * the caller treats as "no label" like any other failure. */
int osrel_find(const void *buf, size_t len, size_t total,
               const char *section, size_t *off, size_t *size);

/* The PE section carrying the kernel version, e.g. "7.2.6-arch2-1". ukify and
 * mkinitcpio both write it, and sd-boot shows it beside the pretty name for
 * exactly the reason it matters here: two UKIs of the same distro carry the
 * same PRETTY_NAME and differ only by kernel. */
#define OSREL_SECT_UNAME ".uname"
#define OSREL_SECT_OSREL ".osrel"

/* The key lookup on its own, over a raw os-release text. Exposed because it is
 * the half worth testing exhaustively, and because the appliance's own label
 * comes from the same text without a PE around it. */
int osrel_value(const char *text, size_t len, const char *key,
                char *out, size_t cap);

#endif /* FALCON_OSREL_H */
