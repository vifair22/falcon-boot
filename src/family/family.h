/* airies-boot: ISO family detection and the descriptor registry
 * (docs/DESIGN.md Section 6.2).
 *
 * Deliberately EFI-free, for the same reason the exFAT driver is: the probe
 * reaches the filesystem only through two callbacks, so the identical code
 * fingerprints the firmware-mounted ESP inside an ISO in the loader and a
 * plain directory tree in a host test.
 *
 * What this module is: a lookup from "what files does this medium have" to a
 * static descriptor. What it is not: a boot path. Nothing here builds a
 * cmdline, loads an initrd or launches anything; the descriptors are data
 * that the blessed direct-boot path consumes later, and until that path
 * exists the templates are inert.
 */

#ifndef AIRIES_FAMILY_H
#define AIRIES_FAMILY_H

#include <stdint.h>
#include <stddef.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    FAMILY_OK          = 0,
    FAMILY_ERR_BAD_ARG = -1,
};

typedef enum {
    FAMILY_UNKNOWN = 0,
    FAMILY_DEBIAN_CASPER,   /* Ubuntu and its derivatives; casper live-boot */
    FAMILY_DEBIAN_LIVE,     /* Debian live images; live-boot, not casper */
    FAMILY_FEDORA_DRACUT,   /* Fedora/RHEL live and installer media */
    FAMILY_ARCH,            /* archiso */
    FAMILY_SYSTEMRESCUE,    /* archiso-derived, its own base directory */
    FAMILY_MEMTEST,         /* memtest86+ / MemTest86 */
    FAMILY_WINDOWS,         /* Windows install media */
} family_id;

/* Three-valued, because "we have not checked" and "no" lead to different
 * decisions: an unknown initramfs capability means a strategy must not be
 * chosen on the assumption that it is there (DESIGN Section 6.2 is explicit
 * that these facts are recorded per family, never assumed). */
typedef enum {
    FAMILY_TRI_UNKNOWN = 0,
    FAMILY_TRI_NO      = 1,
    FAMILY_TRI_YES     = 2,
} family_tri;

/* Registry entry. Data, not code: adding a family is a row here plus a
 * fingerprint, never surgery on a boot path. */
typedef struct {
    family_id   id;
    const char *name;             /* human label, for the menu and messages */

    /* Cmdline fragment for the file-backed-ISO route, appended to whatever
     * cmdline the family already boots with. Placeholders, substituted by
     * the consumer (not by this module):
     *   %ISO%  path of the ISO file on p2, as the target kernel sees it
     *   %DEV%  device the ISO lives on, e.g. /dev/disk/by-label/...
     * NULL when the family needs no fragment at all. */
    const char *cmdline_template;

    /* 1 when the stock initramfs cannot find a file-backed ISO and needs the
     * extent-map/device-mapper injection of DESIGN Section 6.2; 0 when a
     * cmdline fragment (or nothing) is enough. */
    uint8_t     needs_injection;

    /* What the family's stock initramfs is known to carry. exFAT support is
     * what an iso-scan-style search of p2 needs; device-mapper is what the
     * extent-map hook needs. Filled in as each is actually measured on real
     * media; unknown until then, on purpose. */
    family_tri  initramfs_has_exfat;
    family_tri  initramfs_has_dm;
} family_desc;

/* Filesystem probe. Paths are absolute, '/'-separated, ASCII, and carry no
 * trailing slash; matching is expected to be case-insensitive, which both
 * the UEFI File Protocol and the media we fingerprint already are. */
typedef struct {
    /* Nonzero if path exists (file or directory), zero if not. Errors are
     * reported as "does not exist": a fingerprint that cannot be read is a
     * fingerprint that does not match. */
    int (*exists)(void *ctx, const char *path);

    /* Reads up to len bytes at byte offset off. *got receives what was
     * actually delivered, which is short only at end of file. Returns 0 on
     * success, nonzero if the file cannot be read. May be NULL, in which
     * case content fingerprints are skipped and detection falls back to
     * layout alone. */
    int (*read)(void *ctx, const char *path, uint64_t off, void *buf,
                size_t len, size_t *got);

    void *ctx;
} family_probe;

/* Classifies the medium. Never returns NULL: an unrecognised layout yields
 * the FAMILY_UNKNOWN descriptor, which is a real registry entry. */
const family_desc *family_detect(const family_probe *p);

const family_desc *family_by_id(family_id id);

/* The whole registry, for callers that enumerate it (help text, tests). */
const family_desc *family_table(size_t *count);

#endif /* AIRIES_FAMILY_H */
