/* falcon-boot: the El Torito boot catalog (DESIGN Section 6, the ISO path).
 *
 * Why this exists. Under UEFI the firmware's own partition driver walks this
 * catalog and publishes the boot image as a volume, so the loader has never
 * needed to read it: it reads the ISO's own tree instead. That is also why
 * the menu's survey has to publish a virtual device and call
 * ConnectController for every entry it labels, which is the firmware surface
 * the ISO reader route is moving off. Reading the catalog ourselves is what
 * lets falcon-core assemble the same set of volumes the firmware would, from
 * one read callback, on any platform.
 *
 * This module PARSES AND REPORTS. It does not decide which entry to boot, and
 * it deliberately gives the caller no help in doing so beyond the facts on
 * the medium, for reasons measured across all 36 corpus ISOs on 2026-09-19:
 *
 *   Platform ID is not a selector. OPNsense 25.1 declares its UEFI FAT boot
 *   image in a section header reading `91 00 01 00`: final header, platform
 *   0x00 (x86), one entry. FreeBSD 15.1's is `91 ef 01 00`, byte-identical
 *   otherwise (same LBA 20, same 4096 sectors, same FAT12). OPNsense
 *   mis-masters the field, the firmware does not care, and a consumer that
 *   filtered on ELT_PLATFORM_UEFI would find no boot volume there at all. So
 *   the caller walks the entries and probes each one; a parser stricter than
 *   the firmware loses media the firmware handles.
 *
 *   `sectors` is not a size. It is 0 on both Arch snapshots and on Void, and
 *   1 on both Windows ISOs and both ESXi ones, where one 512-byte sector is
 *   plainly not the whole FAT volume. The field says what the firmware should
 *   LOAD, and for a UEFI entry the firmware ignores it and mounts the volume
 *   instead. Size the volume from its own filesystem, never from here, and do
 *   not reject an entry whose count is zero.
 *
 * Also measured in that sweep, as the shape a consumer should expect rather
 * than require: every corpus medium carries exactly two bootable entries, an
 * x86 default entry whose image is raw code (isolinux, etfsboot.com) and one
 * final section header holding a single entry whose image is FAT. No medium
 * uses emulation; all 72 entries are ELT_MEDIA_NONE. None of that is assumed
 * below, because 36 media are not the world.
 *
 * EFI-free, like every other falcon-core module: the medium arrives through a
 * read callback and every buffer is the caller's.
 */

#ifndef FALCON_ELTORITO_H
#define FALCON_ELTORITO_H

#include <stddef.h>
#include <stdint.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    ELT_OK             =  0,
    ELT_ERR_ARG        = -1,
    ELT_ERR_IO         = -2,
    ELT_ERR_NOT_ISO    = -3,  /* no boot record descriptor at LBA 17 */
    ELT_ERR_NO_CATALOG = -4,  /* a boot record, but not El Torito's */
    ELT_ERR_CORRUPT    = -5   /* the validation entry does not check out */
};

/* An ISO9660 logical sector. The catalog's LBAs are in these, and so is the
 * fixed position of the boot record descriptor. */
#define ELT_SECTOR 2048u

/* Boot media type (El Torito 1.0 section 2.1, byte 1 of an entry, low
 * nibble). Emulation means the firmware presents the image as a floppy or a
 * hard disk; ELT_MEDIA_NONE means it is loaded and entered as it is, which is
 * what every entry in the corpus uses. */
typedef enum {
    ELT_MEDIA_NONE  = 0,
    ELT_MEDIA_1200K = 1,
    ELT_MEDIA_1440K = 2,
    ELT_MEDIA_2880K = 3,
    ELT_MEDIA_HDD   = 4
} elt_media;

/* Platform IDs the spec names. Reported, never branched on; see the header
 * comment for the medium that makes that a rule rather than a preference. */
#define ELT_PLATFORM_X86  0x00u
#define ELT_PLATFORM_PPC  0x01u
#define ELT_PLATFORM_MAC  0x02u
#define ELT_PLATFORM_UEFI 0xEFu

/* How many bootable entries are kept. The corpus maximum is two; the bound is
 * what keeps a medium with a corrupt or hostile section count costing a fixed
 * amount of work rather than an unbounded amount, and `truncated` says when
 * one hit it. */
#define ELT_MAX_ENTRIES 8u

typedef struct {
    /* Load address of the boot image, in 2048-byte sectors from the start of
     * the medium. */
    uint32_t lba;
    /* The catalog's sector count, in 512-byte units, EXACTLY as recorded.
     * Not a size: see the header comment. Zero is normal. */
    uint16_t sectors;
    uint8_t  platform;    /* ELT_PLATFORM_*, or whatever the medium says */
    uint8_t  media;       /* elt_media, or whatever the medium says */
    /* 1 for the initial/default entry, 0 for a section entry. The default
     * entry is the one a BIOS loads at 0x7C00; section entries are what the
     * platform-specific stages live in. */
    uint8_t  is_default;
} elt_entry;

typedef struct {
    elt_entry entry[ELT_MAX_ENTRIES];
    unsigned  n;
    /* The catalog held more bootable entries than fitted, or claimed a
     * section count that ran off the end of the sector. Either way what is
     * here is a prefix, not the whole story. */
    uint8_t   truncated;
    /* The validation entry's platform ID and the LBA the catalog was read
     * from, for a caller that wants to report where it looked. */
    uint8_t   validation_platform;
    uint32_t  catalog_lba;
} elt_catalog;

/* Read len bytes at byte offset off, medium-relative. Returns 0 on success,
 * nonzero on failure. Same shape as fs_read_fn, deliberately: a caller that
 * has a volume already has this. */
typedef int (*elt_read_fn)(void *ctx, uint64_t off, uint64_t len, void *buf);

/* Parses the boot catalog of the medium reachable through `read`.
 *
 * `size` is the medium's length in bytes and is used only to refuse offsets
 * past the end before asking the callback for them; pass 0 when it is not
 * known, and the callback's own failure becomes the only bound.
 *
 * Every field of *out is set on success, and *out is zeroed on every failure
 * so a caller that ignores the status reads an empty catalog rather than
 * whatever was on its stack.
 *
 * ELT_ERR_NOT_ISO and ELT_ERR_NO_CATALOG are ordinary answers about a medium,
 * not damage: a data CD has neither and a caller should treat them as "no
 * boot image here". ELT_ERR_CORRUPT means the boot record pointed at a sector
 * whose validation entry is not one, which is a medium that says it has a
 * catalog and does not.
 *
 * An entry with the boot indicator clear (0x00, "not bootable") is skipped
 * rather than reported: the spec allows it and nothing can be done with one.
 * A catalog whose every entry is non-bootable therefore succeeds with n == 0. */
int elt_parse(elt_catalog *out, elt_read_fn read, void *ctx, uint64_t size);

/* The entry a BIOS boots: the default entry unless it says 0xEF, else the
 * first entry that says x86. The platform byte is reported rather than
 * trusted for filtering (the header comment says why), so the default entry
 * comes first and the platform is only a fallback. NULL when the catalog
 * holds no BIOS boot image. falcon-bios's virtual CD serves this entry, and
 * media/biosboot.c judges a medium by it, so both read one rule. */
const elt_entry *elt_bios_entry(const elt_catalog *cat);

#endif /* FALCON_ELTORITO_H */
