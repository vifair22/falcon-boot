/* falcon-bios: what the virtual CD's INT 13h handler needs to know, worked
 * out where it can be tested.
 *
 * The resident handler is 16-bit real-mode assembly, which is the one part
 * of this tree no analyzer and no test reaches. So it is given as little to
 * decide as possible: it looks up a run, does one multiply and one add, and
 * re-issues the read. Everything that could be got wrong - whether the ISO's
 * extents can be served as CD sectors at all, where each run starts on the
 * physical disk, what the drive parameters say - is computed here and is
 * ordinary host-tested C.
 *
 * A RUN is a stretch of the ISO that is contiguous on the physical disk. An
 * ISO copied onto an empty exFAT partition is one run; a fragmented one is
 * several. The handler finds the run holding the requested CD sector by
 * linear scan, which is why the count is small and bounded rather than the
 * 500 extents the extent map allows: a medium in more pieces than this is
 * one to copy again, and the loader says so rather than serving it slowly.
 */

#ifndef FALCON_BIOS_VCDCORE_H
#define FALCON_BIOS_VCDCORE_H

#include <stddef.h>
#include <stdint.h>

/* A CD sector is 2048 bytes. How many disk sectors that is depends on the
 * disk, and the disk address packet's LBA is in the disk's own units, so the
 * ratio is carried in the map rather than assumed: four for the ordinary
 * 512-byte disk, one for a medium that is already a CD. A disk whose sector
 * is larger than a CD's cannot serve one at all and is refused. */
#define VCD_CD_SECTOR 2048u

#define VCD_MAX_RUNS 64u

/* How many stretches of the medium may be served from memory rather than
 * from the disk. This is what `iso/overlay.h` is under UEFI, and it is what
 * makes a medium bootable that is not bootable as it lies on the stick:
 * media/bsdmd.c's two edits are what need it first (DESIGN Section 6.4), and
 * Windows needs five: two Partition Descriptors, boot.wim's File Entry, its
 * patched header, and the tail appended past the ISO's end. */
#define VCD_MAX_OVERRIDES 8u

/* The medium's declared size is rounded up to this many CD sectors, and the
 * sectors past the image's own end read as zeros.
 *
 * Because a BIOS caller reads in fixed-size chunks and the last one runs
 * past the end of an image whose length is not a multiple of that chunk.
 * FreeBSD's loader reads eight sectors at a time, and a 521 MB medium is
 * 266,927 of them: the last read asks for 266,920 through 266,927 and the
 * medium has 266,926 as its last. Refusing that read is refusing the medium;
 * a pressed CD is padded to a whole number of blocks and answers.
 *
 * And the medium answers for one more padding block past what it says it
 * has, which is `n_limit`. Measured, not guessed: with the padding in place
 * the same loader then read the sector AT the declared size - 266,944 of
 * 266,944 - and the refusal cost the boot. Being permissive here costs
 * nothing; a caller that has run off the end gets zeros instead of an error
 * it was not written to handle. */
#define VCD_PAD 64u

typedef struct {
    uint32_t cd_lba;    /* first CD sector this run serves */
    uint32_t n_cd;      /* how many CD sectors it serves */
    uint64_t disk_lba;  /* where they are, in the disk's own sector units */
} vcd_run;

/* An override's bytes are either in conventional memory, where the handler
 * copies them itself, or above a megabyte, where they are out of real mode's
 * reach and the BIOS moves them with INT 15h AH=87h. The second exists for
 * Windows: its patched boot.wim appends a tail measured in megabytes, which
 * is more conventional memory than a machine has to give. */
#define VCD_OVER_EXT 1u   /* bytes are at `phys`, not in segment `seg` */

typedef struct {
    uint32_t cd_lba;   /* first CD sector served from memory */
    uint32_t n_cd;     /* how many */
    uint16_t seg;      /* real-mode segment, when not VCD_OVER_EXT */
    uint16_t flags;
    uint32_t phys;     /* physical address, when VCD_OVER_EXT */
} vcd_override;

typedef struct {
    vcd_run  run[VCD_MAX_RUNS];
    vcd_override over[VCD_MAX_OVERRIDES];
    uint32_t n_over;
    uint32_t n_runs;
    uint32_t n_real;    /* CD sectors the runs and overrides actually hold */
    uint32_t n_cd;      /* what the medium says it has: n_real, padded */
    uint32_t n_limit;   /* the last sector it will answer for at all */
    uint32_t per_cd;    /* disk sectors per CD sector */
    uint8_t  drive;     /* the drive number the handler answers for */
    uint8_t  phys;      /* the physical drive the runs are on */
} vcd_map;

enum {
    VCD_OK            = 0,
    VCD_FROM_MEMORY   = 1,   /* vcd_span only: served by an override */
    VCD_FROM_ZERO     = 2,   /* vcd_span only: past the image, inside the
                                medium, so it reads as zeros */
    VCD_FROM_EXTENDED = 3,   /* vcd_span only: an override above a megabyte */
    VCD_ERR_BAD_ARG   = -1,
    VCD_ERR_ALIGN     = -2,  /* an extent is not a whole number of CD sectors */
    VCD_ERR_TOO_MANY  = -3,  /* more runs than the handler will scan */
    VCD_ERR_EMPTY     = -4
};

/* One extent of the ISO on its partition, as fs_extents reports them:
 * partition-relative byte offset and length. Declared here rather than taken
 * from fs.h so this file and its tests do not need the filesystem registry. */
typedef struct {
    uint64_t off;
    uint64_t len;
} vcd_extent;

/* Builds the run table. `part_lba` is the partition's first 512-byte LBA on
 * the disk, `sector_size` the disk's own sector size.
 *
 * Adjacent extents that are contiguous on the disk are merged, because an
 * ISO copied onto a fresh partition arrives as many extents that are one
 * run, and the handler's scan is what that saves. */
int vcd_build(vcd_map *m, const vcd_extent *ext, size_t n_ext,
              uint64_t part_lba, uint32_t sector_size, uint64_t iso_size,
              uint8_t drive, uint8_t phys);

/* Where a CD sector is on the disk, and how many of the requested run of
 * them are contiguous from there. This is exactly what the handler does, in
 * C, so a test can check the arithmetic the assembly copies.
 *
 * 0 with *disk_lba and *n_cd set (n_cd is at most `want`), or nonzero when
 * the sector is past the end of the medium. */
int vcd_locate(const vcd_map *m, uint32_t cd_lba, uint32_t want,
               uint64_t *disk_lba, uint32_t *n_cd);

/* Adds a stretch served from memory. The bytes are the caller's and must
 * stay where they are; `seg` is a real-mode segment, because what reads them
 * is the resident handler.
 *
 * VCD_ERR_TOO_MANY past VCD_MAX_OVERRIDES, VCD_ERR_BAD_ARG for a stretch
 * that runs off the end of the medium or overlaps one already added. */
int vcd_override_add(vcd_map *m, uint32_t cd_lba, uint32_t n_cd, uint16_t seg);

/* The same, for bytes above a megabyte. `phys` is their physical address,
 * which the handler hands to the BIOS rather than addressing itself. */
int vcd_override_add_ext(vcd_map *m, uint32_t cd_lba, uint32_t n_cd,
                         uint32_t phys);

/* Grows the medium by `extra` CD sectors, so an override may cover ground
 * the image itself does not.
 *
 * Windows is why: its patched boot.wim is longer than the original, and the
 * difference is served past the ISO's last sector. The medium a BIOS sees is
 * the ISO plus that, which is what its UDF descriptors are rewritten to say.
 */
int vcd_extend(vcd_map *m, uint32_t extra);

/* Adds a run mapping n_cd CD sectors at cd_lba to disk_lba, the way vcd_build
 * lays down the base ISO's own runs. It is how a stretch of the medium grown
 * past the image (vcd_extend) is served from the disk rather than from memory:
 * the relocated Windows boot.wim of an ISO 9660-only medium points its single
 * extent at appended sectors that redirect, run for run, to where the original
 * boot.wim already sits, so its bulk goes out over INT 13h and not a sector at
 * a time from memory over INT 15h. The run must lie within the medium (grow it
 * first) and not overlap one already present. */
int vcd_add_run(vcd_map *m, uint32_t cd_lba, uint32_t n_cd, uint64_t disk_lba);

/* How the next stretch of a read is served, which is the first thing the
 * handler asks of each pass.
 *
 * VCD_FROM_MEMORY with *seg set and *n_cd sectors from an override,
 * VCD_FROM_EXTENDED with *phys set for one above a megabyte,
 * VCD_FROM_ZERO for *n_cd sectors of the medium's tail padding, or VCD_OK
 * with *n_cd sectors that come from the disk and stop short of the next
 * override. Nonzero and negative past the end of the medium. */
int vcd_span(const vcd_map *m, uint32_t cd_lba, uint32_t want,
             uint16_t *seg, uint32_t *phys, uint32_t *n_cd);

#endif /* FALCON_BIOS_VCDCORE_H */
