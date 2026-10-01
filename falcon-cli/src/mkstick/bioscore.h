/* falcon-boot mkstick: the arithmetic of putting falcon-bios on a disk, with
 * no I/O in it (docs/DESIGN.md Section 3.3; the layout contract is
 * docs/notes/station-rework.md section 9).
 *
 * Why this is a separate file. The BIOS route writes to places no file
 * owns: the boot code at the head of LBA 0, a run of sectors between LBA 0
 * (or the GPT's entry array) and the first partition, and the reserved
 * sectors of the `boot` partition, which FAT keeps its own boot sector,
 * FSInfo and their backups among. Every one of those numbers is a chance to
 * write a stage over something that matters, and getting it wrong destroys
 * a disk rather than failing to boot one. So the part that decides WHERE is
 * here, pure and tested, and the part that opens the device is bios.c.
 *
 * The other half of the contract is falcon-bios/src/start/biosdefs.h. bios.c
 * passes its values in rather than this file including it, which keeps this
 * host C that a test can hand any layout to, including the wrong ones.
 *
 * The layout, as the plan below computes it:
 *
 *   LBA 0, bytes 0..439     stage 1, with stage 2's first LBA (LE32) and its
 *                           length in sectors (LE16) patched in at the
 *                           contract's offsets; 440..511 (disk signature,
 *                           table, 0x55AA) are the disk's and never written
 *   stage 2                 LBA 34 on GPT (past the entry array), LBA 1 on
 *                           MBR; its real length, ending before the first
 *                           partition
 *   stage 3                 the `boot` partition's reserved sectors, from the
 *                           sector its identity record names
 *   the next-boot marker    one reserved sector of the same partition, named
 *                           by the record too
 */

#ifndef FALCON_MKSTICK_BIOSCORE_H
#define FALCON_MKSTICK_BIOSCORE_H

#include <stddef.h>
#include <stdint.h>

#include <part/table.h>

#define BIOS_SECTOR_BYTES 512u

/* The largest disk sector an install handles: a 4Kn disk (DESIGN Section
 * 3.4). The BIOS reads and the record's sectors are the disk's own, so on
 * one every count below is of 4096-byte sectors. */
#define BIOS_SECTOR_MAX 4096u

/* The split of LBA 0. Bytes 0..439 are boot code; 440..443 are the disk
 * signature, 444..445 reserved, 446..509 the four MBR partition records and
 * 510..511 the 0xAA55 signature. On GPT the records hold the protective 0xEE
 * entry, on MBR the disk's partitions; either way they survive, which is why
 * an install splices the sector instead of writing stage 1 over it. */
#define BIOS_MBR_CODE_LEN 440u
#define BIOS_MBR_SIG_OFF  510u

/* The GPT type for a BIOS boot partition, in sgdisk's textual form, and the
 * slot `mkstick bios --reserve` claims it in.
 *
 * The partition is bookkeeping for stage 2 alone (stage 3 lives in the boot
 * partition): stage 1 goes to the LBA written into it and never parses a
 * table. What it buys is that a later gdisk, parted or installer sees the gap
 * as claimed rather than free. Slot 5 because p1..p4 are the falcon layout of
 * DESIGN Section 3 and a partition inserted at the front would renumber them. */
#define BIOS_BOOT_TYPE "21686148-6449-6E6F-744E-656564454649"
#define BIOS_BOOT_SLOT 5u
#define BIOS_BOOT_NAME "FALCON-BIOS"

typedef enum {
    BIOSCORE_OK          =  0,
    BIOSCORE_ERR_ARG     = -1,
    BIOSCORE_ERR_MBR     = -2,  /* the disk's LBA 0 has no 0xAA55 */
    BIOSCORE_ERR_STAGE1  = -3,  /* not a 512-byte sector ending 0xAA55 */
    BIOSCORE_ERR_SPLICE  = -4,  /* stage 1 runs into the partition table */
    BIOSCORE_ERR_STAGE2  = -5,  /* stage 2 is larger than stage 1 may read */
    BIOSCORE_ERR_GPT     = -7,  /* stage 2 would land on the GPT */
    BIOSCORE_ERR_GAP     = -8,  /* stage 2 would reach the first partition */
    BIOSCORE_ERR_BACKUP  = -9,  /* not a backup this tool wrote */
    BIOSCORE_ERR_SECTOR  = -10, /* not a 512-byte-sector disk */
    BIOSCORE_ERR_FAT     = -11, /* the boot partition is not a usable FAT */
    BIOSCORE_ERR_RECORD  = -12, /* the record's sectors collide with FAT's */
    BIOSCORE_ERR_RESERVED = -13 /* stage 3 does not fit the reserved sectors */
} bioscore_status;

const char *bioscore_strerror(bioscore_status s);

/* ---- the boot partition's FAT boot sector ---- */

typedef struct {
    uint32_t sector;       /* bytes per sector: 512, 1024, 2048 or 4096 */
    uint32_t reserved;     /* BPB reserved sector count */
    uint32_t fsinfo;       /* FAT32: the FSInfo sector; 0 on FAT12/16 */
    uint32_t backup;       /* FAT32: the backup boot sector; 0 when none */
    int      fat32;
} bios_fat;

/* Reads what the plan needs out of a FAT boot sector (its first 512 bytes
 * are enough, whatever its sector size): the sector size, the reserved
 * count, and on FAT32 the FSInfo and backup boot sector numbers.
 * BIOSCORE_ERR_FAT for anything that is not a FAT boot sector. A FAT of
 * 4096-byte sectors is what mkfs.fat makes on a 4Kn disk: it carries a boot
 * record (the UEFI loader reads it), but no BIOS stages, which the caller
 * checks. */
bioscore_status bioscore_fat_bpb(const uint8_t *bs, bios_fat *out);

/* The reserved sector count a `boot` partition is formatted with
 * (`mkfs.fat -R`): stage 3's first sector, then room for stage 3 of twice
 * the size of the one given, rounded up to a whole MiB and never less than
 * one. The margin is so a later, larger stage 3 still fits without
 * reformatting the partition (the stick's is about 200 KB today, 1 MiB of
 * room is five times that). 0 when the result would not fit the BPB's
 * 16-bit field. */
uint32_t bioscore_reserved_for(uint32_t stage3_sector, size_t stage3_len);

/* ---- the plan ---- */

/* Everything the plan needs to know about the disk, gathered by the caller
 * from the table (part_read_table), the boot partition's identity record and
 * its boot sector. */
typedef struct {
    uint32_t  sector_bytes;
    part_kind kind;
    uint64_t  first_usable;    /* GPT: the header's; MBR: 1 */
    uint64_t  first_part_lba;  /* the lowest start of any partition except
                                  the FALCON-BIOS reservation */
    uint64_t  boot_start;      /* the boot partition's first LBA */
    uint64_t  boot_sectors;    /* its length */
    bios_fat  fat;             /* its boot sector's facts */
    uint16_t  stage3_sector;   /* from its record */
    uint16_t  marker_sector;   /* from its record; 0 for none */
} bios_disk;

/* What the install writes, in absolute LBAs. Each run is exactly the
 * sectors written: a stage padded to a whole sector, nothing beyond. */
typedef struct {
    uint64_t stage2_lba;
    uint64_t stage2_sectors;
    uint64_t stage3_lba;
    uint64_t stage3_sectors;
    uint64_t marker_lba;       /* 0 when the record names no marker */
} bios_plan;

/* The plan for this disk. stage2_gpt_lba and stage2_mbr_lba are the
 * contract's places for stage 2 (biosdefs.h STAGE2_LBA, STAGE2_LBA_MBR),
 * in the disk's sectors, and stage2_max its reservation (STAGE2_SECTORS) in
 * 512-byte sectors, since it bounds the memory stage 1 reads into: stage 1's
 * read is bounded by it, whatever the gap would allow. Sectors are 512 or
 * 4096 bytes, and the boot partition's FAT must use the disk's. Refuses a stage 2 below the GPT's first
 * usable LBA or reaching the first partition, a record whose stage 3 or
 * marker sector lands on one of FAT's own sectors or outside the reserved
 * area, and a stage 3 that does not fit between its first sector and the end
 * of the reserved area (with the marker outside that run). */
bioscore_status bioscore_plan(const bios_disk *d, uint64_t stage2_gpt_lba,
                              uint64_t stage2_mbr_lba, uint64_t stage2_max,
                              size_t stage2_len, size_t stage3_len,
                              bios_plan *out);

/* Is stage 1 sane to splice? One 512-byte sector ending 0x55AA with nothing
 * between byte 440 and the signature, since the splice keeps the disk's own
 * bytes there. */
bioscore_status bioscore_check_stage1(const uint8_t *stage1, size_t len);

/* Stage 1 laid into the disk's own LBA 0: bytes 0..439 from the image, then
 * stage 2's LBA and length written at the contract's offsets (lba_off,
 * sectors_off: biosdefs.h STAGE1_S2_LBA_OFF, STAGE1_S2_SECTORS_OFF), bytes
 * 440..511 from the disk. `out` may alias `cur`. */
bioscore_status bioscore_splice_mbr(const uint8_t *cur, const uint8_t *stage1,
                                    uint32_t lba_off, uint32_t sectors_off,
                                    uint64_t stage2_lba,
                                    uint64_t stage2_sectors, uint8_t *out);

/* The stage 2 location an LBA 0 carries, read back from the same offsets.
 * Returns nonzero when both fields are nonzero. */
int bioscore_stage1_fields(const uint8_t *mbr, uint32_t lba_off,
                           uint32_t sectors_off, uint64_t *lba,
                           uint64_t *sectors);

/* Nonzero when this LBA 0 carries this stage 1's code: bytes 0..439 equal,
 * except the six bytes of stage 2's location, which differ per disk. */
int bioscore_is_installed(const uint8_t *mbr, const uint8_t *stage1,
                          uint32_t lba_off, uint32_t sectors_off);

/* Nonzero when this entry is falcon's reservation over stage 2's gap: a GPT
 * partition named BIOS_BOOT_NAME, in whatever slot it took. `mkstick bios
 * --reserve` makes it in slot BIOS_BOOT_SLOT; `station install` in the
 * first free one, which on a disk with four partitions before it is not
 * slot 5, and a check on the slot took that one for a real partition in
 * stage 2's way (mkstick update, 2026-09-30). The one definition, so the
 * commands that make it and the report that reads it back cannot disagree
 * about what it looks like. */
int bioscore_is_reserve(const part_table *t, const part_entry *p);

/* ---- the backup a write is preceded by ----
 *
 * An install overwrites sectors that may be somebody else's, so it saves
 * every sector it is about to write first: LBA 0, the stage 2 run, the
 * stage 3 run and the marker sector. The header lists those runs, so a
 * restore needs no other information, and restoring the wrong file onto a
 * disk fails on the magic rather than succeeding. Version 1 (one gap run
 * from LBA 34) is not read: nothing wrote it past the layout it described. */
#define BIOS_BACKUP_MAGIC    "FALCON-BIOS-BK2"
#define BIOS_BACKUP_HDR      128u
#define BIOS_BACKUP_VER      2u
#define BIOS_BACKUP_MAX_RUNS 6u

typedef struct {
    uint64_t lba;
    uint64_t sectors;
} bios_run;

bioscore_status bioscore_backup_header(uint8_t *out, uint32_t sector_bytes,
                                       const bios_run *runs, size_t n);
bioscore_status bioscore_backup_parse(const uint8_t *hdr, size_t len,
                                      uint32_t *sector_bytes, bios_run *runs,
                                      size_t cap, size_t *n);

/* The byte length a backup file has for these runs: the header, then each
 * run's sectors in order. */
uint64_t bioscore_backup_size(uint32_t sector_bytes, const bios_run *runs,
                              size_t n);

#endif /* FALCON_MKSTICK_BIOSCORE_H */
