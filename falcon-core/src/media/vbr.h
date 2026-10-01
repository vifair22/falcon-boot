/* falcon-boot: what a boot sector is (docs/DESIGN.md Section 12.3).
 *
 * The loader a partition's boot sector starts, by the file names and
 * messages its code holds, and the words a row gives it; what kind of BPB
 * the sector carries; whether another disk's LBA 0 is one a BIOS would
 * start. One implementation for the BIOS station, which offers what it
 * finds as rows (apps/station/bios_main.c, falcon-bios/src/mbr/chain.c),
 * and for the installer and `station status` (falcon-cli), which write and
 * show those rows: a row the installer names in STATION.INI is the row the
 * loader finds, with the same label.
 *
 * Every sector here is 512 bytes and the caller's. Nothing here reads a
 * disk except through the caller's callback.
 */

#ifndef FALCON_MEDIA_VBR_H
#define FALCON_MEDIA_VBR_H

#include <stddef.h>
#include <stdint.h>

#define VBR_SECTOR      512u
#define VBR_TABLE_OFF   0x1BEu   /* the four 16-byte entries */
#define VBR_ENTRY_LEN   16u
#define VBR_SIG_OFF     510u

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
 * sectors vbr_identify asks again when sector 0 carries nothing and the BPB
 * is FAT32; 0 ends the list. */
extern const uint8_t vbr_fat32_code_sectors[];

/* Reads the partition's sector `sector` (counted from its first) into buf,
 * 512 bytes. 0 on success. */
typedef int (*vbr_read_fn)(void *ctx, uint32_t sector, uint8_t *buf);

/* The loader a partition starts, from its first sector `sec0` (which must
 * end in 0x55AA) and, on FAT32 when that carries nothing, the reserved
 * sectors vbr_fat32_code_sectors names, read through `read` into `scratch`
 * (512 bytes; sec0 is left alone). A code sector is asked only for the
 * name: sector 0's 0x55AA is the signature. A read that fails ends the
 * search with what was found so far, which is nothing. */
vbr_loader vbr_identify(const uint8_t *sec0, vbr_read_fn read, void *ctx,
                        uint8_t *scratch);

/* Whether another disk's LBA 0 is worth offering: signed, boot code in its
 * first 440 bytes (not all zero), and a primary entry marked active. A GPT
 * data disk, a table with no active entry and a UEFI-only Windows disk all
 * end in 0x55AA and fail after the jump; this is what tells them apart. */
int vbr_mbr_bootable(const uint8_t *mbr);

#endif /* FALCON_MEDIA_VBR_H */
