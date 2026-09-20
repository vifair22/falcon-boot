/* falcon-boot mkstick: the arithmetic of putting falcon-bios on a GPT stick,
 * with no I/O in it (docs/notes/bios-route.md Section 7.3).
 *
 * Why this is a separate file. The BIOS route writes to three places no
 * filesystem owns and no partition covers: the 440 bytes of boot code at the
 * head of LBA 0, and a run of sectors between the end of the GPT entry array
 * and the start of the first partition. Every one of those numbers is a
 * chance to write a stage on top of something that matters - the GPT header
 * one sector up, a partition one sector down - and getting it wrong destroys
 * a stick rather than failing to boot one. So the part that decides WHERE is
 * here, where it is pure and tested, and the part that opens the device is in
 * bios.c.
 *
 * The other half of the contract is falcon-bios/src/biosdefs.h. Stage 1 is
 * 512 bytes and reads a fixed sector range rather than looking anything up,
 * so the LBAs are compile-time constants on both sides. bios.c passes the
 * loader's values in rather than including biosdefs.h, which keeps this file
 * host C that a test can hand any layout to, including the wrong ones.
 */

#ifndef FALCON_MKSTICK_BIOSCORE_H
#define FALCON_MKSTICK_BIOSCORE_H

#include <stddef.h>
#include <stdint.h>

#define BIOS_SECTOR_BYTES 512u

/* The split of LBA 0. Bytes 0..439 are boot code; 440..443 are the disk
 * signature, 444..445 reserved, 446..509 the four MBR partition records and
 * 510..511 the 0xAA55 signature.
 *
 * On a GPT disk those partition records hold the PROTECTIVE entry, the single
 * 0xEE partition that stops an MBR-only tool from seeing free space where the
 * GPT is. It has to survive, which is why an install splices the sector
 * instead of writing stage 1 over it: LBA 0 is shared between the thing that
 * boots the disk and the thing that stops the disk being mistaken for empty. */
#define BIOS_MBR_CODE_LEN 440u
#define BIOS_MBR_SIG_OFF  510u

/* The GPT type for a BIOS boot partition, in sgdisk's textual form, and the
 * slot an install claims.
 *
 * The partition is bookkeeping and nothing reads it: stage 1 goes to a fixed
 * LBA and never parses a table. What it buys is that a later gdisk, parted or
 * installer sees the gap as claimed rather than free. Slot 5 because p1..p4
 * are the falcon layout of DESIGN Section 3 and their NUMBERS are in the
 * install manifest and in people's heads; a BIOS boot partition inserted at
 * the front would renumber every one of them. */
#define BIOS_BOOT_TYPE "21686148-6449-6E6F-744E-656564454649"
#define BIOS_BOOT_SLOT 5u
#define BIOS_BOOT_NAME "FALCON-BIOS"

/* What the install owns, in sectors. Derived from the loader's constants so
 * that a change to biosdefs.h moves the tool with it. */
typedef struct {
    uint64_t stage2_lba;
    uint64_t stage2_sectors;   /* reserved, not the image's size */
    uint64_t stage3_lba;
    uint64_t stage3_sectors;   /* reserved */
    uint64_t end_lba;          /* one past the last sector the install owns */
} bios_plan;

typedef enum {
    BIOSCORE_OK          =  0,
    BIOSCORE_ERR_ARG     = -1,
    BIOSCORE_ERR_MBR     = -2,  /* the disk's LBA 0 has no 0xAA55 */
    BIOSCORE_ERR_STAGE1  = -3,  /* not a 512-byte sector ending 0xAA55 */
    BIOSCORE_ERR_SPLICE  = -4,  /* stage 1 runs into the partition table */
    BIOSCORE_ERR_STAGE2  = -5,  /* image past the sectors stage 1 reads */
    BIOSCORE_ERR_STAGE3  = -6,  /* image past the sectors stage 2 reads */
    BIOSCORE_ERR_GPT     = -7,  /* the plan starts before first usable LBA */
    BIOSCORE_ERR_GAP     = -8,  /* a partition starts inside the plan */
    BIOSCORE_ERR_BACKUP  = -9   /* not a backup this tool wrote */
} bioscore_status;

const char *bioscore_strerror(bioscore_status s);

/* The sector plan for one set of loader constants. Fails on a layout that
 * cannot be written at all: stage 2 on the GPT header, or a run that wraps. */
bioscore_status bioscore_plan(uint64_t stage2_lba, uint64_t stage2_sectors,
                              uint64_t stage3_sectors, bios_plan *out);

/* Do these three images fit the plan, and is stage 1 sane to splice? `stage1`
 * is the whole 512-byte image; the other two are sizes only, since nothing
 * about their contents is this file's business. */
bioscore_status bioscore_check_images(const uint8_t *stage1, size_t stage1_len,
                                      size_t stage2_len, size_t stage3_len,
                                      const bios_plan *p);

/* Does the plan fit the hole this disk actually has?
 *
 * `first_usable` is the GPT header's field and `first_part_lba` the lowest
 * start_lba of any partition in the table (or 0 when there are none, meaning
 * "nothing below the plan to protect"). These are the two checks that stand
 * between a typo and somebody's partition table: below first_usable is the
 * entry array, and at or past first_part_lba is a filesystem. */
bioscore_status bioscore_check_gap(const bios_plan *p, uint64_t first_usable,
                                   uint64_t first_part_lba);

/* Stage 1 laid into the disk's own LBA 0: bytes 0..439 from the image, bytes
 * 440..511 from the disk. `out` may alias `cur`. */
bioscore_status bioscore_splice_mbr(const uint8_t *cur, const uint8_t *stage1,
                                    uint8_t *out);

/* Nonzero when this LBA 0 already carries this stage 1. */
int bioscore_is_installed(const uint8_t *mbr, const uint8_t *stage1);

/* ---- the backup a write is preceded by ----
 *
 * An install overwrites boot code that may be somebody else's, so it saves
 * every sector it is about to touch first. The header exists so a restore
 * does not have to be told the offsets again, and so restoring the wrong file
 * onto a disk's boot sectors fails on the magic rather than succeeds. */
#define BIOS_BACKUP_MAGIC "FALCON-BIOS-BK1"
#define BIOS_BACKUP_HDR   64u
#define BIOS_BACKUP_VER   1u

bioscore_status bioscore_backup_header(uint8_t *out, uint32_t sector_bytes,
                                       uint64_t gap_lba, uint64_t gap_sectors);
bioscore_status bioscore_backup_parse(const uint8_t *hdr, size_t len,
                                      uint32_t *sector_bytes,
                                      uint64_t *gap_lba, uint64_t *gap_sectors);

/* The byte length a backup file must have for the geometry in its header. */
uint64_t bioscore_backup_size(uint32_t sector_bytes, uint64_t gap_sectors);

#endif /* FALCON_MKSTICK_BIOSCORE_H */
