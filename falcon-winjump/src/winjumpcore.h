/* falcon-winjump: the host-tested half of the driver-mount fallback
 * (DESIGN Section 6.3).
 *
 * When VirtDisk cannot attach the ISO (Vista and 7 WinPE have no ISO support,
 * and their WinPE cannot read the exFAT partition the ISO lives on either),
 * winjump instead presents the ISO to Windows through falcon-vdisk, the
 * kernel driver it carries. The driver needs two things winjump must work
 * out: which physical disk to read, and where on it the ISO's bytes are.
 *
 * The loader already answers both, for the Linux route, in the extent map it
 * builds (falcon-core/src/media/extmap.h): the ISO's runs as byte offsets
 * within a GPT partition named by its unique partition GUID. winjump reads
 * the same map (the loader drops it in boot.wim beside winjump), finds the
 * disk whose GPT carries that partition GUID, and turns the partition-relative
 * runs into absolute disk extents for the driver.
 *
 * This file is the pure arithmetic of those two steps, no Windows in it, so
 * the host tests exercise it: the GPT match over bytes winjump has read, and
 * the run conversion. winjump.c does the I/O (enumerate the physical disks,
 * read their GPTs, feed the driver).
 */

#ifndef FALCON_WINJUMPCORE_H
#define FALCON_WINJUMPCORE_H

#include <stdint.h>
#include <stddef.h>

#include <media/extmap.h>

/* One run the driver serves: bytes of the medium at iso_offset live at
 * disk_offset on the disk, for length bytes. Byte-identical to the driver's
 * struct falcon_vdisk_extent (falcon-vdisk/src/vdiskio.h); winjump.c copies
 * these straight into the SET_BACKING buffer. */
typedef struct {
    uint64_t iso_offset;
    uint64_t disk_offset;
    uint64_t length;
} wj_extent;

enum {
    WJ_OK          = 0,
    WJ_ERR_BAD_ARG = -1,
    WJ_ERR_NOSPACE = -2,   /* the output extent array is too small */
    WJ_ERR_NOMATCH = -3,   /* no GPT partition with that unique GUID */
    WJ_ERR_FORMAT  = -4    /* not a GPT, or a header this does not read */
};

/* Looks for a GPT partition with unique GUID `part_guid` in a disk's already
 * read table: `hdr_sector` is LBA 1 (the GPT header, at least 512 bytes) and
 * `entries`/`entries_len` the partition entry array it points at. On a match,
 * *first_lba is the partition's first LBA and the return is WJ_OK; WJ_ERR_NOMATCH
 * when the table is a GPT but carries no such partition; WJ_ERR_FORMAT when
 * `hdr_sector` is not a GPT header. Pure: winjump.c does the reading. */
int wj_gpt_match(const uint8_t *hdr_sector, const uint8_t *entries,
                 size_t entries_len, const uint8_t part_guid[16],
                 uint64_t *first_lba);

/* Turns an accepted extent map into driver extents. `map`/`map_len` is a map
 * extmap_decode accepted and `hdr` its header; the disk offset of each run is
 * hdr->part_lba * hdr->block_size + the run's partition-relative offset. Fills
 * up to `cap` entries and sets *n; WJ_ERR_NOSPACE when there are more runs
 * than cap. Pure. */
int wj_backing_extents(const extmap_hdr *hdr, const uint8_t *map, size_t map_len,
                       wj_extent *out, uint32_t cap, uint32_t *n);

#endif /* FALCON_WINJUMPCORE_H */
