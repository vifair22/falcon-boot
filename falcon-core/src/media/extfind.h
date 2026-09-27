/* falcon-core: finding the partition an extent map names (docs/DESIGN.md
 * Sections 3.1, 6.4).
 *
 * The map (media/extmap.h) carries the ISO library's install ID, the
 * partition's first LBA and the disk's logical block size. Its three readers
 * each hold a list of disks and nothing else: falcon-hook inside a distro's
 * initramfs, falcon-winjump in WinPE and falcon-vdisk in 2000/XP text-mode
 * setup. None of them has the library mounted by anything that could tell
 * it which disk it is (WinPE and XP setup cannot read exFAT at all), so each
 * asks this, one disk at a time: is the volume at the map's LBA on this disk
 * the library the map names? The answer is the record, read the way the
 * loaders read it (ident/identfs.h), so the readers find p2 exactly as the
 * loaders do and no partition table is consulted: the LBA says where to
 * look, the record says whether it is ours. An MBR stick and a GPT one are
 * the same question.
 *
 * Why the record and not a cheaper test. A start LBA alone is not a name:
 * every disk partitioned by a current tool has a partition at 2048, and a
 * second falcon stick has one at the same LBA with an exFAT volume on it.
 * The install ID is what tells two sticks apart, and it lives in the file.
 *
 * EFI-free and allocation-free; the scratch is the caller's.
 */

#ifndef FALCON_EXTFIND_H
#define FALCON_EXTFIND_H

#include <stdint.h>

#include <fs/fs.h>
#include <ident/identfs.h>
#include <media/extmap.h>

/* Nonzero when r is the record of the library h names: role library and
 * the same install ID. */
int extmap_names(const extmap_hdr *h, const ident_rec *r);

/* Whether the disk behind read/ctx holds the library h names. `read` is
 * over the whole disk, in bytes from LBA 0; `block_size` is the disk's
 * logical block size, or 0 when the caller cannot learn it (then the map's
 * is assumed). A disk whose block size differs from the map's is not the
 * one: part_lba means a different byte offset there.
 *
 * EXTMAP_OK when it does; EXTMAP_ERR_NOMATCH when it does not, which is
 * every disk but one and includes a disk too short for the offset, a
 * volume with no record, and a record of another install or role;
 * EXTMAP_ERR_BAD_ARG. Nothing stays mounted: s is scratch only. */
int extmap_find_on_disk(const extmap_hdr *h, fs_read_fn read, void *ctx,
                        uint32_t block_size, ident_fs_scratch *s);

#endif /* FALCON_EXTFIND_H */
