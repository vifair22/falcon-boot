/* falcon-boot: device discovery (docs/DESIGN.md Section 9).
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
 * handle is classified there and then: is media present, which falcon role
 * does its GPT type GUID give it (Section 3), does it carry a LUKS2 header,
 * and which registered filesystem driver claims its bytes. Classification is
 * eager because it is cheap - a couple of sector reads per handle - and
 * mounting stays lazy, because mounting is not: each consumer mounts the rows
 * it cares about, when it cares about them.
 *
 * A rescan is this same entry point run again. It drops src/part's cached
 * view of the machine first, so media the firmware enumerated after the first
 * look is judged against a current picture rather than a stale one.
 */

#ifndef FALCON_DISCOVER_H
#define FALCON_DISCOVER_H

#include <efi/uefi.h>
#include <efi/blockio.h>

#include <fs/fs.h>
#include <part/part.h>

/* Inventory cap. A machine with more than this many Block I/O handles is a
 * server with a disk shelf, not a laptop with a stick in it; the rows past
 * the cap are counted (discover_skipped) and reported rather than silently
 * dropped. Each row is small, but the scan itself costs reads per handle, so
 * an unbounded inventory would also be an unbounded startup delay. */
#define DISCOVER_MAX 32u

typedef struct discover_dev {
    EFI_HANDLE             handle;
    EFI_BLOCK_IO_PROTOCOL *bio;

    uint8_t media_present;
    /* DESIGN Section 3's scan filter, evaluated once per role: "only
     * falcon-typed partitions if any exist, everything otherwise". Both can
     * be true on a machine with no falcon-typed partition at all, which is
     * exactly the QEMU rig and any hand-made stick. */
    uint8_t accept[PART_ROLE_COUNT];
    uint8_t has_type;        /* type below was established */
    uint8_t luks2;           /* LUKS2 primary header magic at offset 0 */

    EFI_GUID type;           /* GPT partition type GUID */
    fs_hint  role;           /* the falcon role its type names, or
                                FS_HINT_NONE for anything else */

    /* The driver that claimed the PLAINTEXT bytes of this device, or NULL.
     * NULL for an encrypted volume by construction: a LUKS2 volume's bytes
     * are nobody's filesystem until it is opened, so the fs probe is not even
     * attempted there and the personal-OS path identifies what is inside
     * after unlocking. */
    const fs_ops *fs;
} discover_dev;

/* Enumerates and classifies. Safe to call again; the inventory is rebuilt. */
void discover_scan(EFI_SYSTEM_TABLE *st);

UINTN discover_count(void);
/* Handles seen after the inventory filled up. Nonzero means the machine had
 * more Block I/O handles than DISCOVER_MAX. */
UINTN discover_skipped(void);
discover_dev *discover_at(UINTN i);

/* Iterates the rows a role's scan should look at: media present and accepted
 * by DESIGN Section 3's filter. *cursor starts at 0 and is opaque; returns
 * NULL when there are no more. */
discover_dev *discover_next(part_role role, UINTN *cursor);

/* The byte-read callback over a row's Block I/O instance, in the shape every
 * driver in this tree takes. ctx is the discover_dev. Offsets and lengths
 * must be whole blocks, which is what every caller here produces: an
 * unaligned request is a driver bug rather than a device condition, and is
 * reported as a read failure rather than fixed up. */
int discover_read(void *ctx, uint64_t off, uint64_t len, void *buf);

#endif /* FALCON_DISCOVER_H */
