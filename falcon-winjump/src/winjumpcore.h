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
 * within the ISO library's partition, which the map names by the library's
 * install ID and its first LBA. winjump reads the same map (the loader drops
 * it in boot.wim beside winjump), finds the disk that holds that library the
 * way falcon-hook does (falcon-core/src/media/extfind.h: the exFAT or FAT
 * volume at the map's LBA whose /.falcon record carries the map's install
 * ID, read over raw sectors, since this WinPE cannot mount exFAT), and
 * turns the
 * partition-relative runs into absolute disk extents for the driver.
 * falcon-vdisk does the same from the kernel for 2000/XP setup.
 *
 * This file is the part of those steps with no Windows in it, so the host
 * tests exercise it: the disk match over a sector reader, and the run
 * conversion. winjump.c and vdisk.c do the I/O (enumerate the physical
 * disks, read sectors, feed the driver).
 */

#ifndef FALCON_WINJUMPCORE_H
#define FALCON_WINJUMPCORE_H

#include <stdint.h>
#include <stddef.h>

#include <ident/identfs.h>
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
    WJ_ERR_NOMATCH = -3,   /* not the disk that holds the map's library */
    WJ_ERR_FORMAT  = -4    /* not a map this reads */
};

/* A raw disk read as Windows allows one: `len` bytes at byte offset `off`,
 * both whole sectors of the disk. 0 on success. */
typedef int (*wj_sector_read)(void *ctx, uint64_t off, uint32_t len,
                              void *buf);

/* What the match needs besides the disk: a bounce buffer for the reads the
 * filesystem driver makes that are not whole sectors (a sector or more;
 * WJ_BOUNCE covers a 4096-byte sector with room to spare), and the record
 * reader's scratch. Both kilobytes, so the caller's statics, never a
 * kernel thread's stack. */
#define WJ_BOUNCE 16384u
typedef struct {
    uint8_t          bounce[WJ_BOUNCE];
    ident_fs_scratch rec;
} wj_scratch;

/* Does the disk behind `read` hold the ISO library the map names? Reads are
 * made in the map's block size, which a matching disk has; a disk of another
 * sector size fails its reads and is not the one. WJ_OK or WJ_ERR_NOMATCH;
 * WJ_ERR_BAD_ARG. */
int wj_disk_holds(const extmap_hdr *hdr, wj_sector_read read, void *ctx,
                  wj_scratch *s);

/* Turns an accepted extent map into driver extents. `map`/`map_len` is a map
 * extmap_decode accepted and `hdr` its header; the disk offset of each run is
 * hdr->part_lba * hdr->block_size + the run's partition-relative offset. Fills
 * up to `cap` entries and sets *n; WJ_ERR_NOSPACE when there are more runs
 * than cap. Pure. */
int wj_backing_extents(const extmap_hdr *hdr, const uint8_t *map, size_t map_len,
                       wj_extent *out, uint32_t cap, uint32_t *n);

/* One line of a key=value file (falcon-winjump.cfg, windows.cfg), split in
 * place: blanks around the key and the value are dropped, and a blank line,
 * a '#' comment, a line with no '=' or an empty key is not a pair. 1 with
 * *key and *value pointing into `line`, else 0. */
int wj_kv_line(char *line, char **key, char **value);

#endif /* FALCON_WINJUMPCORE_H */
