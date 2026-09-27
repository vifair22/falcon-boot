/* falcon-boot: device discovery (docs/DESIGN.md Sections 3.1, 9).
 *
 * ONE pass, one inventory, every consumer reads it.
 *
 * Before this module each path ran its own enumeration loop: the ISO scan
 * looked for exFAT volumes, the personal-OS scan looked for LUKS2 headers,
 * and a third would have looked for something else. Three loops over the same
 * handles is three chances to disagree about what is plugged in, and the
 * failure mode is the quiet one: a device discovered for one purpose and
 * missed for another, with nothing on screen to say so.
 *
 * So the Block I/O handles are enumerated exactly once, at startup, and every
 * handle is classified there and then: is media present, does it carry a
 * LUKS2 header, which registered filesystem driver claims its bytes, and what
 * its identity record says (DESIGN Section 3.1). Classification is eager
 * because it is cheap - a few sector reads per handle - and mounting for use
 * stays lazy: each consumer mounts the rows it cares about, when it cares
 * about them.
 *
 * Which rows are this install's. The loader reads the record in its own boot
 * partition first (LoadedImage->DeviceHandle, the ESP it was read from, whose
 * boot sector carries a `boot` record) and takes that record's install ID.
 * A row belongs to this install only when its own record carries the same
 * ID: the ISO library by the /.falcon file on its exFAT volume, the shelf by
 * the falcon token in its LUKS2 header or by /.falcon on a plaintext ext4.
 * Rows are looked for on every disk, since a station's shelf may sit on
 * another disk than its ESP, but only matching IDs are taken, so a second
 * stick's library or shelf never enters this loader's menu. A boot partition
 * with no record makes no row this install's: there is no fallback to "any
 * exFAT volume" or "any LUKS2 volume" (DESIGN Section 3.1).
 *
 * A rescan is this same entry point run again.
 */

#ifndef FALCON_DISCOVER_H
#define FALCON_DISCOVER_H

#include <efi/uefi.h>
#include <efi/blockio.h>

#include <fs/fs.h>
#include <ident/ident.h>

/* Inventory cap. A machine with more than this many Block I/O handles is a
 * server with a disk shelf, not a laptop with a stick in it; the rows past
 * the cap are counted (discover_skipped) and reported rather than silently
 * dropped. The boot disk's handles are enumerated first, so the cap never
 * costs this install its own library or shelf when they share the loader's
 * disk (a stick, always). Each row is small, but the scan itself costs reads
 * per handle, so an unbounded inventory would also be an unbounded startup
 * delay. */
#define DISCOVER_MAX 32u

typedef struct discover_dev {
    EFI_HANDLE             handle;
    EFI_BLOCK_IO_PROTOCOL *bio;

    uint8_t media_present;
    uint8_t luks2;           /* LUKS2 primary header magic at offset 0 */
    uint8_t has_rec;         /* rec below was read: a LUKS2 falcon token, or
                                /.falcon on the volume `fs` claimed */
    uint8_t ours;            /* has_rec, and rec carries the install ID of
                                the loader's own boot partition */
    ident_rec rec;

    /* The driver that claimed the PLAINTEXT bytes of this device, or NULL.
     * NULL for an encrypted volume by construction: a LUKS2 volume's bytes
     * are nobody's filesystem until it is opened, so the fs probe is not even
     * attempted there and the personal-OS path identifies what is inside
     * after unlocking. */
    const fs_ops *fs;
} discover_dev;

/* What the loader's own boot partition said, for the screen. */
typedef enum {
    DISCOVER_ID_OK = 0,      /* a `boot` record: its install ID is in use */
    DISCOVER_ID_NO_DEVICE,   /* the firmware named no readable device for
                                the loader's image */
    DISCOVER_ID_NONE,        /* the boot partition carries no record */
    DISCOVER_ID_BAD          /* a record that is damaged, of a version this
                                loader does not read, or not role `boot` */
} discover_id_state;

/* Enumerates and classifies. Safe to call again; the inventory is rebuilt.
 * image is the loader's own image handle: its LoadedImage->DeviceHandle is
 * the boot partition whose record names this install. */
void discover_scan(EFI_SYSTEM_TABLE *st, EFI_HANDLE image);

/* The boot partition's record as the last scan read it. *out (may be NULL)
 * is filled on DISCOVER_ID_OK only. */
discover_id_state discover_install(ident_rec *out);

/* The sentence for the screen when the state is not DISCOVER_ID_OK, or NULL:
 * what is missing and what to do. falcon-bios says the same sentences. */
const CHAR16 *discover_install_problem(discover_id_state state);

UINTN discover_count(void);
/* Handles seen after the inventory filled up. Nonzero means the machine had
 * more Block I/O handles than DISCOVER_MAX. */
UINTN discover_skipped(void);
discover_dev *discover_at(UINTN i);

/* Iterates this install's rows of one role: media present and a record of
 * that role carrying the boot partition's install ID. *cursor starts at 0
 * and is opaque; returns NULL when there are no more. */
discover_dev *discover_next(ident_role role, UINTN *cursor);

/* The byte-read callback over a row's Block I/O instance, in the shape every
 * driver in this tree takes. ctx is the discover_dev. Any offset and any
 * length: falcon-core's re-blocking layer (blk/rebl.h, DESIGN Section 3.4)
 * turns the drivers' 512- and 1024-byte reads into whole blocks, so a
 * 4096-byte-block disk mounts and unlocks as a 512-byte one does. */
int discover_read(void *ctx, uint64_t off, uint64_t len, void *buf);

#endif /* FALCON_DISCOVER_H */
