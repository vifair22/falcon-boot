/* falcon-bios: the decisions of a chain to a partition's boot sector or
 * another disk's MBR (docs/DESIGN.md Sections 13.3 and 13.4), apart from
 * the BIOS so the host tests reach them.
 *
 * What a DOS or Windows MBR does before it jumps, done by falcon on
 * buffers the caller owns: which partition table entry is handed over and
 * where it will sit, whether a boot sector's BPB says where it is on the
 * disk and which drive it is on, and what a boot sector is. chain.c reads
 * the sectors, places them and jumps; everything it decides on the way is
 * here. Nothing in this file writes a disk: the BPB is patched in the
 * caller's copy of the sector, which is the copy that runs.
 *
 * Every sector here is 512 bytes. A disk whose sectors are larger is not
 * chained at all (chain.c says so): a 4096-byte boot sector at 0x7C00 would
 * run over stage 2, and no BIOS-booted Windows or DOS lives on one.
 */

#ifndef FALCON_BIOS_VBRCORE_H
#define FALCON_BIOS_VBRCORE_H

#include <stddef.h>
#include <stdint.h>

#define VBR_SECTOR      512u
#define VBR_TABLE_OFF   0x1BEu   /* the four 16-byte entries */
#define VBR_ENTRY_LEN   16u
#define VBR_SIG_OFF     510u

/* Where the MBR copy goes and where DS:SI then points (DS = 0): 0000:0600
 * is where every DOS and Windows MBR relocates itself to before it loads a
 * boot sector over 0x7C00, so a boot sector that reads the entry it was
 * started from finds it at 0x7BE plus sixteen per slot, as it would have. */
#define VBR_MBR_COPY    0x0600u
#define VBR_ENTRY_SI(slot) (VBR_MBR_COPY + VBR_TABLE_OFF + VBR_ENTRY_LEN * (slot))

/* What a boot sector's code is, by the names and messages its loader
 * writes into it. The labels a row carries (vbr_loader_name). */
typedef enum {
    VBR_LOADER_NONE = 0,
    VBR_LOADER_BOOTMGR,   /* Windows Vista and later */
    VBR_LOADER_NTLDR,     /* Windows NT, 2000, XP */
    VBR_LOADER_WIN9X,     /* Windows 95, 98, ME: WINBOOT.SYS */
    VBR_LOADER_FREEDOS,   /* KERNEL.SYS */
    VBR_LOADER_MSDOS,     /* IO.SYS / IBMBIO.COM without WINBOOT */
    VBR_LOADER_GRUB,
    VBR_LOADER_SYSLINUX
} vbr_loader;

/* Whether the sector ends in 0x55AA. */
int vbr_signed(const uint8_t *sec);

/* The loader whose signature this sector carries, first match in the order
 * of the enum; VBR_LOADER_NONE for an unsigned sector or one carrying none
 * (a data partition's boot sector, which says "This is not a bootable
 * disk"). The signatures are the file names and messages each loader's boot
 * code holds, looked for anywhere in the sector. */
vbr_loader vbr_detect(const uint8_t *sec);

/* The row's words for it: "Windows (BOOTMGR)", "Windows (NTLDR)", "Windows
 * 9x", "FreeDOS", "MS-DOS", "GRUB", "SYSLINUX". NULL for NONE. */
const char *vbr_loader_name(vbr_loader l);

/* What kind of BPB the sector carries, which says where its hidden-sectors
 * and drive-number fields are. */
typedef enum {
    VBR_BPB_NONE = 0,
    VBR_BPB_FAT,          /* FAT12 or FAT16: drive at 0x24, sig at 0x26 */
    VBR_BPB_FAT32,        /* drive at 0x40, sig at 0x42 */
    VBR_BPB_NTFS          /* "NTFS    " at 3; drive at 0x24 */
} vbr_bpb;

vbr_bpb vbr_bpb_kind(const uint8_t *sec);

/* FAT32 boot code spans more than sector 0: Windows 98's name is in sector
 * 2 and NT's (NTLDR, BOOTMGR) in sector 12 of the reserved area. The
 * sectors vbr_detect is worth asking again when sector 0 carries nothing
 * and the BPB is FAT32; 0 ends the list. */
extern const uint8_t vbr_fat32_code_sectors[];

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

/* Whether another disk's LBA 0 is worth offering: signed, boot code in its
 * first 440 bytes (not all zero), and a primary entry marked active. A GPT
 * data disk, a table with no active entry and a UEFI-only Windows disk all
 * end in 0x55AA and fail after the jump; this is what tells them apart. */
int vbr_mbr_bootable(const uint8_t *mbr);

#endif /* FALCON_BIOS_VBRCORE_H */
