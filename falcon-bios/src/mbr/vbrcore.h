/* falcon-bios: the decisions of a chain to a partition's boot sector or
 * another disk's MBR (docs/DESIGN.md Sections 13.3 and 13.4), apart from
 * the BIOS so the host tests reach them.
 *
 * What a DOS or Windows MBR does before it jumps, done by falcon on
 * buffers the caller owns: which partition table entry is handed over and
 * where it will sit, whether a boot sector's BPB says where it is on the
 * disk and which drive it is on. chain.c reads
 * the sectors, places them and jumps; everything it decides on the way is
 * here. Nothing in this file writes a disk: the BPB is patched in the
 * caller's copy of the sector, which is the copy that runs. What a boot
 * sector is (its loader, its BPB's kind) is core's media/vbr.c, which the
 * installer shares.
 *
 * Every sector here is 512 bytes. A disk whose sectors are larger is not
 * chained at all (chain.c says so): a 4096-byte boot sector at 0x7C00 would
 * run over stage 2, and no BIOS-booted Windows or DOS lives on one.
 */

#ifndef FALCON_BIOS_VBRCORE_H
#define FALCON_BIOS_VBRCORE_H

#include <stddef.h>
#include <stdint.h>

#include <media/vbr.h>

/* Where the MBR copy goes and where DS:SI then points (DS = 0): 0000:0600
 * is where every DOS and Windows MBR relocates itself to before it loads a
 * boot sector over 0x7C00, so a boot sector that reads the entry it was
 * started from finds it at 0x7BE plus sixteen per slot, as it would have. */
#define VBR_MBR_COPY    0x0600u
#define VBR_ENTRY_SI(slot) (VBR_MBR_COPY + VBR_TABLE_OFF + VBR_ENTRY_LEN * (slot))

/* What vbr_fix_bpb did, for the serial line that says so. */
typedef struct {
    vbr_bpb  bpb;
    uint8_t  hidden_patched;
    uint8_t  drive_patched;
    uint8_t  hidden_too_far;  /* the start is past 32 bits: not patched */
    uint32_t hidden_was;
    uint8_t  drive_was;
} vbr_fix;

/* Makes the BPB say what the boot code will find: its hidden-sectors field
 * (0x1C in every BPB here) the partition's start, and its drive-number
 * field (where the BPB has one: FAT with extended signature 0x28 or 0x29,
 * and NTFS) the drive handed over in DL. DOS, Windows 9x and NT boot
 * sectors find themselves by these two fields, and a disk imaged from
 * another machine, moved behind the drive swap, or restored to another
 * offset carries stale ones. Changes nothing on a sector with no BPB. */
void vbr_fix_bpb(uint8_t *sec, uint64_t start_lba, uint8_t dl, vbr_fix *out);

/* Marks entry `slot` (0-3) active and every other entry inactive, in the
 * caller's copy of LBA 0. -1 when the slot is out of range, the entry is
 * empty (type 0) or an extended container, or its start is not
 * `start_lba`: the table read at boot is not the one the row was built
 * from. */
int vbr_mark_active(uint8_t *mbr, unsigned slot, uint64_t start_lba);

#endif /* FALCON_BIOS_VBRCORE_H */
