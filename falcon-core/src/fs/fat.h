/* falcon-boot: read-only FAT12/16/32 (docs/notes/iso-reader-route.md stage B).
 *
 * Why the loader needs a FAT driver at all, given that exFAT is next door and
 * the firmware has one of its own:
 *
 *   1. The El Torito boot image is a FAT volume on real media. Measured
 *      across the 36 corpus ISOs on 2026-09-19: FAT12 on 26, FAT16 on 6
 *      (Fedora, Rocky, Alma, openSUSE, agama, CachyOS) and FAT32 on both Arch
 *      snapshots. Without this, falcon-core cannot read the volume that
 *      actually boots, and the menu's survey has to keep asking the firmware.
 *   2. p1 is vfat. Nothing in falcon-core can read the stick's own ESP today;
 *      the loader goes through the firmware's Simple File System protocol,
 *      which a BIOS build would not have (docs/notes/bios-route.md).
 *
 * exFAT is a different filesystem and exfat.c does not read this one: no FAT
 * chain, no 8.3 names, no LFN. They share a marketing name and nothing else.
 *
 * Read-only, like every driver in fs/. Caller-allocated storage, a byte read
 * callback, and no allocation anywhere, for the reason part.h gives: the same
 * parser runs over a firmware block device in the loader and over a plain
 * image file in the host tests.
 *
 * ---- what this driver supports, and what it declines ----
 *
 * Supported: FAT12, FAT16 and FAT32; sector sizes 512 to FAT_MAX_SECTOR;
 * long file names (VFAT LFN) with the checksum verified against the 8.3 entry
 * they describe; volume labels from the root directory's label entry, falling
 * back to the BPB's.
 *
 * Declined, with FAT_ERR_UNSUPPORTED rather than a guess, because each is a
 * volume this driver would otherwise mis-read:
 *
 *   - a sector size outside 512..FAT_MAX_SECTOR, or not a power of two;
 *   - sectors per cluster that is zero or not a power of two;
 *   - zero FATs, or zero reserved sectors (the boot sector is reserved by
 *     definition, so zero means the BPB is not one);
 *   - FAT12/16 with a zero root entry count, or FAT32 with a nonzero one;
 *   - a volume whose cluster count implies a type its BPB contradicts.
 *
 * ---- the type rule ----
 *
 * FAT's type is NOT a field. It is derived from the count of data clusters,
 * and the thresholds are exact, not approximate (Microsoft's FAT spec,
 * "FAT Type Determination"): under 4085 is FAT12, under 65525 is FAT16,
 * otherwise FAT32. Getting this off by one reads the FAT with the wrong
 * entry width, which does not fail, it silently follows the wrong chain. The
 * constants are written once, below, for that reason.
 */

#ifndef FALCON_FAT_H
#define FALCON_FAT_H

#include <stddef.h>
#include <stdint.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    FAT_OK              =  0,
    FAT_ERR_IO          = -1,
    FAT_ERR_NOT_FAT     = -2, /* no plausible BPB */
    FAT_ERR_CORRUPT     = -3, /* structure that contradicts itself */
    FAT_ERR_BAD_ARG     = -4,
    FAT_ERR_RANGE       = -5, /* read outside the file */
    FAT_ERR_NOSPACE     = -6, /* caller's array too small */
    FAT_ERR_UNSUPPORTED = -7  /* see the header comment */
};

typedef int (*fat_read_fn)(void *ctx, uint64_t off, uint64_t len, void *buf);

/* The largest sector this driver reads a FAT sector into. 4096 covers every
 * drive we will meet, and matches GPT_MAX_SECTOR next door in part.h. */
#define FAT_MAX_SECTOR 4096u

/* Every read this driver makes after mount is a whole volume sector at a
 * sector-aligned offset, and the boot sector is read as its first 512 bytes.
 * Neither has to be a whole device sector: the loaders put falcon-core's
 * re-blocking layer (blk/rebl.h, DESIGN Section 3.4) under the callback, so
 * a 512-byte-sector volume on a 4096-byte-sector disk reads as one on its
 * own kind of disk. Volume-sector reads are still what keep that layer on
 * its no-copy path whenever the two sizes agree.
 *
 * A directory entry is 32 bytes and every supported sector size is a multiple
 * of 32, so no entry ever straddles a sector boundary. A FAT12 entry can
 * straddle one, and fat_fat_entry reads it a byte at a time for that reason. */

/* A long file name is at most 255 UCS-2 units (20 LFN entries of 13). */
#define FAT_NAME_MAX 255u
/* The most a directory may hold: 65536 entries of 32 bytes, the limit
 * the specification sets. A walk past it is a cluster chain that loops,
 * which would otherwise be walked forever. */
#define FAT_DIR_MAX (65536u * 32u)

typedef enum {
    FAT_TYPE_12 = 12,
    FAT_TYPE_16 = 16,
    FAT_TYPE_32 = 32
} fat_type;

/* The exact thresholds of the FAT type rule. Named so the one place they are
 * compared reads as the spec does. */
#define FAT_MAX_CLUSTERS_12 4085u
#define FAT_MAX_CLUSTERS_16 65525u

typedef struct {
    fat_read_fn read;
    void       *read_ctx;

    fat_type type;
    uint32_t sector_size;
    uint32_t cluster_size;      /* sector_size * sectors per cluster */
    uint32_t fat_off_sectors;   /* reserved sectors: where FAT 0 starts */
    uint32_t fat_sectors;       /* sectors in ONE FAT */
    uint32_t data_off_sectors;  /* first sector of the data region */
    uint32_t clusters;          /* count of DATA clusters */
    uint32_t root_cluster;      /* FAT32 only */
    uint32_t root_off_sectors;  /* FAT12/16 only: the fixed root region */
    uint32_t root_entries;      /* FAT12/16 only */

    /* The volume's own name, trailing spaces removed, ASCII. Empty when the
     * volume has none. Taken from the root directory's volume label entry if
     * there is one, else the BPB's, which is what every other tool shows. */
    char     label[12];

    /* One-sector cache for FAT lookups. Chain walking asks for one entry at a
     * time and neighbouring clusters land in the same sector, so this turns a
     * sequential walk into one read per sector of FAT rather than one per
     * cluster. ~0 marks it invalid. */
    uint64_t fat_cache_off;
    uint8_t  fat_cache[FAT_MAX_SECTOR];
} fat_vol;

typedef struct {
    uint16_t name[FAT_NAME_MAX + 1];  /* UCS-2, NUL-terminated */
    uint16_t name_len;
    uint8_t  is_dir;
    uint8_t  is_regular;              /* 0 for the volume label entry */
    uint32_t cluster;                 /* first cluster, 0 for an empty file */
    uint32_t size;                    /* FAT stores this as 32 bits */
} fat_dirent;

/* The long-name accumulator is deliberately NOT in here. An entry's LFN
 * records immediately precede its 8.3 record in the same directory, so one
 * fat_dir_next call sees the whole run and the accumulator is local to it.
 * Keeping 512 bytes of it here instead would push fat_dir past FS_DIR_SIZE
 * once the sector buffer is counted. */
typedef struct {
    fat_vol *vol;
    /* The root of a FAT12/16 volume is a fixed region, not a chain, so
     * iteration is over a sector range in that case and over a cluster chain
     * in every other. `fixed` says which. */
    uint8_t  fixed;
    uint32_t cluster;                 /* cluster holding chain_index */
    uint32_t chain_index;             /* which cluster of the directory */
    uint32_t pos;                     /* byte offset within the directory */
    uint32_t limit;                   /* fixed case: bytes in the region */
    uint64_t buf_off;                 /* volume byte offset cached, ~0 bad */
    uint8_t  buf[FAT_MAX_SECTOR];
} fat_dir;

typedef struct {
    fat_vol *vol;
    uint32_t first_cluster;
    uint32_t size;
    /* The last chain position resolved, so a sequential read does not walk
     * from the start for every call. cluster 0 means "not resolved yet". */
    uint32_t seek_index;              /* cluster index within the file */
    uint32_t seek_cluster;
    uint8_t  buf[FAT_MAX_SECTOR];     /* bounce for unaligned edges */
} fat_file;

typedef struct {
    uint64_t vol_off;
    uint64_t len;
} fat_extent;

/* Probe: one read of the boot sector, checking the things that make a BPB a
 * BPB. Cheap and deliberately not a validation; mount is allowed to refuse
 * what this accepted. */
int fat_probe(fat_read_fn read, void *read_ctx);

int fat_mount(fat_vol *v, fat_read_fn read, void *read_ctx);

int fat_dir_open_root(fat_vol *v, fat_dir *d);
int fat_dir_open(fat_vol *v, fat_dir *d, const fat_dirent *e);
/* 1 = entry produced, 0 = end of directory, negative = FAT_ERR_*.
 * "." and ".." are never produced, nor are deleted entries, nor the volume
 * label entry (its text reaches the caller as vol->label instead). */
int fat_dir_next(fat_dir *d, fat_dirent *out);

int fat_file_open(fat_vol *v, fat_file *f, const fat_dirent *e);
/* Wholly inside the file or nothing, like every driver in the tree. */
int fat_file_read(fat_file *f, uint64_t off, uint64_t len, void *buf);

/* The file's physical runs in volume bytes, adjacent clusters coalesced. Same
 * contract as exfat_file_extents: a cap too small is FAT_ERR_NOSPACE with
 * *count set to what was needed. */
int fat_file_extents(fat_vol *v, const fat_dirent *e, fat_extent *out,
                     size_t cap, size_t *count);

#endif /* FALCON_FAT_H */
