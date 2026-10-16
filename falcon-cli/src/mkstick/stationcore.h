/* falcon-boot mkstick: the arithmetic and the decisions of `station
 * install`, with no I/O in them (docs/DESIGN.md Section 12.9).
 *
 * Why this is a separate file, for the same reason bioscore.c is: an
 * installer that puts a partition into someone's MBR, a stage into the
 * sectors before their Windows partition and a file into their ESP gets
 * every one of those places from a number, and a wrong number there costs
 * the machine's own system rather than a boot of ours. So the part that
 * decides WHERE and WHAT is here, pure and tested against the disks that
 * must be refused as well as the ones that must not, and station.c is the
 * part that opens devices.
 *
 * What lives here:
 *
 *   - free space: where the station partition goes, in space no partition
 *     holds, never by moving one (the installer resizes nothing);
 *   - the MBR entry it takes: a free primary slot, the CHS fields an old
 *     BIOS reads, the disk signature a disk the station names must have;
 *   - what a boot sector is: the loaders a partition's first sector names
 *     (NTLDR, BOOTMGR, IO.SYS, GRUB, ...), and whether another disk's MBR is
 *     worth chaining (DESIGN Section 12.3);
 *   - the entries' names, as the loaders generate them
 *     (DESIGN Section 12), and the STATION.INI the
 *     installer writes from what it found (Section 12.7);
 *   - the backup file's header: which sectors of which disks were saved,
 *     and which of them `remove` puts back.
 */

#ifndef FALCON_MKSTICK_STATIONCORE_H
#define FALCON_MKSTICK_STATIONCORE_H

#include <stddef.h>
#include <stdint.h>

#include <media/stationini.h>
#include <media/vbr.h>
#include <part/table.h>

/* The station partition: 64 MiB by default, FAT32 hidden (0x1C) on MBR.
 * The floor is FAT32's: a FAT of fewer than 65525 clusters is FAT16 by
 * definition, whatever mkfs.fat was asked for (4.2 makes one with a
 * warning, and every FAT reader, falcon-core's included, then reads it as
 * FAT16). At one sector a cluster that is 32 MiB of data behind the
 * reserved sectors and the two FATs on a 512-byte-sector disk, and 256 MiB
 * on a 4Kn one, where a cluster cannot be smaller than a sector: the same
 * arithmetic that makes a 4Kn disk's ESP 260 MB. STN_MAX_MIB bounds --size
 * on either. */
#define STN_DEFAULT_MIB    64u
#define STN_MIN_MIB        40u
#define STN_DEFAULT_MIB_4K 288u
#define STN_MIN_MIB_4K     272u
#define STN_MAX_MIB        4096u
#define STN_MBR_TYPE       0x1Cu

/* The floor and the default for a disk of `sector`-byte sectors (512 or
 * 4096); 0 for any other size, which the installer refuses. */
unsigned stn_min_mib(uint32_t sector);
unsigned stn_default_mib(uint32_t sector);

/* Where a new partition starts: on a MiB boundary when the free space
 * allows it, else on a 4 KiB one. An old disk's free space ends wherever its
 * last cylinder did. In 512-byte sectors; stn_align_for gives the same
 * boundary in a disk's own sectors (a 4Kn disk: 256, and 1). */
#define STN_ALIGN_MIB 2048u
#define STN_ALIGN_4K  8u

uint64_t stn_align_for(uint64_t align512, uint32_t sector);

/* The byte counts DESIGN Section 3.4 warns at: what a BIOS reaches by CHS
 * (1024 cylinders of 255 heads of 63 sectors) and by 28-bit LBA. */
#define STN_REACH_CHS_BYTES   ((uint64_t)1024u * 255u * 63u * 512u)
#define STN_REACH_LBA28_BYTES ((uint64_t)1 << 37)

/* A GPT BIOS boot partition's type ("Hah!IdontNeedEFI"), as bytes, so an
 * existing one is recognised whatever it is named. */
extern const uint8_t stn_guid_bios_boot[16];

typedef enum {
    STN_OK          =  0,
    STN_ERR_ARG     = -1,
    STN_ERR_NOSLOT  = -2,  /* the MBR has no free primary slot */
    STN_ERR_NOSPACE = -3,  /* no free region is large enough */
    STN_ERR_RANGE   = -4,  /* past what an MBR entry can say */
    STN_ERR_BACKUP  = -5,  /* not a station backup this tool wrote */
    STN_ERR_FULL    = -6,  /* a fixed-size list is full */
    STN_ERR_INI     = -7   /* the INI did not format */
} stn_status;

const char *stn_strerror(stn_status s);

/* ---- free space ---- */

typedef struct {
    uint64_t start;    /* first LBA */
    uint64_t sectors;
} stn_region;

/* A region of `want` sectors that no partition of `t` touches, between lo
 * and hi (both inclusive), its start a multiple of `align`. Every entry of
 * the table counts as taken, an MBR extended partition included (a primary
 * cannot sit inside one). The highest-addressed gap that fits wins, so the
 * free space an OS left at the end of the disk is used before a hole
 * between two partitions. STN_ERR_NOSPACE when none fits; *largest is then
 * the most a gap would give at that alignment, for the refusal. */
stn_status stn_find_free(const part_table *t, uint64_t lo, uint64_t hi,
                         uint64_t want, uint64_t align, stn_region *out,
                         uint64_t *largest);

/* The first LBA a station partition may take: the first partition's
 * start, since everything before it is the boot area stage 2 lives in
 * (DESIGN Section 3.3), or `floor` on a disk with no partitions. */
uint64_t stn_first_allowed(const part_table *t, uint64_t floor);

/* ---- the MBR ---- */

/* The first free primary slot (1-4: type byte zero), or 0. */
unsigned stn_mbr_free_slot(const uint8_t *lba0);

/* The geometry the disk's own entries were written with (the ending head
 * and sector of any entry whose CHS is not the 1023/254/63 marker), else
 * 255 heads of 63 sectors. */
void stn_mbr_geometry(const uint8_t *lba0, unsigned *heads, unsigned *spt);

/* Writes one primary entry into `lba0`: slot 1-4, not active, the type,
 * the LBA fields and the CHS ones for that geometry (1023/heads-1/spt past
 * cylinder 1023). Refuses an occupied slot and anything past 32 bits. */
stn_status stn_mbr_put(uint8_t *lba0, unsigned slot, uint8_t type,
                       uint64_t start, uint64_t sectors, unsigned heads,
                       unsigned spt);

uint32_t stn_mbr_sig(const uint8_t *lba0);
void     stn_mbr_set_sig(uint8_t *lba0, uint32_t sig);

/* Nonzero when another disk's LBA 0 is worth offering as a chain (DESIGN
 * Section 12.3): the BIOS station's own rule, media/vbr.c's
 * vbr_mbr_bootable (0x55AA, boot code in the first 440 bytes, an active
 * entry). A disk that merely ends in 0x55AA fails after the jump. */
int stn_mbr_chainable(const uint8_t *lba0);

/* ---- what a partition's boot sector names ---- */

/* What a partition's boot sector starts is media/vbr.c's vbr_identify, the
 * BIOS station's own reading, so a row the installer names is one the
 * loader offers, under the same words (vbr_loader_name). */

/* Nonzero for what the installer makes the default: a Windows or a DOS. */
int stn_is_windows_or_dos(vbr_loader l);

/* ---- entry names (DESIGN Section 12) ---- */

/* The name a loader gives a found entry: mbr-<sig>-p<n> or
 * gpt-<first 8 hex of the DiskGUID>-p<n> for a partition (part > 0),
 * mbr-<sig> or gpt-<8> for a disk's own MBR (part 0). 0 on success. */
int stn_name_disk(const part_disk_id *id, uint32_t part, char *buf,
                  size_t cap);

/* uki-<stem>, the stem being the file name without ".efi"; -1 when the
 * stem has a character a name may not (names are [A-Za-z0-9_-]). */
int stn_name_uki(const char *file, char *buf, size_t cap);

/* ---- what the installer found, and the INI it writes ---- */

typedef enum {
    STN_F_PART = 1,   /* a partition whose boot sector names a loader */
    STN_F_DISK,       /* another disk's own MBR */
    STN_F_UKI,        /* a UKI on the shelf */
    STN_F_FWENTRY,    /* a UEFI firmware boot entry */
    STN_F_EFIWIN      /* \EFI\Microsoft\Boot\bootmgfw.efi on a disk's ESP */
} stn_found_kind;

typedef struct {
    stn_found_kind kind;
    vbr_loader     os;
    part_disk_id   disk;
    uint32_t       part;
    uint16_t       boot_num;                      /* FWENTRY: Boot#### */
    uint32_t       disk_ix;                       /* which disk the caller
                                                     read it from: two disks
                                                     with signature 0 share
                                                     one id until they are
                                                     given theirs */
    char           name[STATIONINI_NAME_MAX];     /* the loader's name */
    char           label[STATIONINI_LABEL_MAX];   /* UKI: its file name */
    char           title[STATIONINI_LABEL_MAX];   /* UKI: the row's words
                                                     (shelf.c's label), when
                                                     `status` read them */
    uint8_t        locked;                        /* UKI: on an encrypted
                                                     shelf, so no row
                                                     carries it at boot
                                                     until the prompt */
} stn_found;

/* The row label the loaders give what was found (strows.h's words):
 * "<loader> (disk <id>, partition <n>)" for a partition, "Boot code (disk
 * <id>)" for a disk's MBR, "Windows Boot Manager (disk <id>, partition
 * <n>)" for an ESP's bootmgfw.efi. */
void stn_found_label(stn_found *f);

/* STATION.INI from what was found (DESIGN Section 12.7). One section per
 * partition, disk and ESP Windows found, named by what it is (windows, dos,
 * grub, syslinux, disk; -2, -3 when there are several) and carrying its
 * disk, partition and label, so a person edits a label or hides a row
 * without looking up a disk signature. UKIs and firmware entries get none:
 * the loader finds them itself and their generated names are enough to be
 * a default. The default is the first Windows or DOS, else the first UKI,
 * else the first other loader; `timeout` seconds when there is one, and no
 * countdown when there is not. A UKI default on an encrypted shelf is a
 * section of its own, `linux` with `uki = <file>`: the loader has no row
 * named uki-<stem> before the shelf is unlocked, and a `uki` section is
 * the row that prompts for the shelf and boots that file (Section 12.7). */
stn_status stn_ini_build(const stn_found *f, size_t n, unsigned timeout,
                         stationini *out);

/* ---- the backup ----
 *
 * One file per install, written before the first byte of any disk is: a
 * header naming the disks and the runs of sectors saved from each, then the
 * runs' bytes in order. What each run is decides what `remove` does with
 * it (stn_run_restores). */
#define STN_BK_MAGIC     "FALCON-STATION1"
#define STN_BK_HDR       4096u
#define STN_BK_VER       1u
#define STN_BK_MAX_RUNS  16u
#define STN_BK_MAX_DISKS 8u
#define STN_BK_PATH      256u
#define STN_BK_RUN_MAX   0x10000u   /* sectors; a larger run is damage */

typedef enum {
    STN_RUN_LBA0 = 1,     /* the target's LBA 0 */
    STN_RUN_GAP,          /* from the end of the table to the first
                             partition: where stage 2 goes */
    STN_RUN_GPT,          /* the primary GPT: LBA 1 to the array's end */
    STN_RUN_GPT_BACKUP,   /* the backup GPT at the disk's end */
    STN_RUN_REGION,       /* the station partition's first sectors */
    STN_RUN_ESP_BOOT,     /* the ESP's boot sector, or its FAT32 backup */
    STN_RUN_SIG           /* another disk's LBA 0, before its signature */
} stn_run_kind;

#define STN_FLAG_BIOS      0x01u
#define STN_FLAG_UEFI      0x02u
#define STN_FLAG_MADE_PART 0x04u   /* the station partition is ours */
#define STN_FLAG_MADE_EF02 0x08u   /* so is the FALCON-BIOS partition */
#define STN_FLAG_FW_ENTRY  0x10u   /* a firmware entry was created */

typedef struct {
    uint32_t disk;        /* index into stn_backup.disks */
    uint32_t kind;        /* stn_run_kind */
    uint64_t lba;
    uint64_t sectors;
} stn_run;

typedef struct {
    uint32_t sector;
    uint32_t flags;
    uint8_t  install_id[16];
    uint32_t station_part;   /* the station partition's number, 0 none */
    uint32_t esp_part;       /* the ESP's, 0 none */
    uint32_t ef02_part;      /* the FALCON-BIOS partition's, 0 none */
    uint32_t fw_entry;       /* Boot#### created, 0xFFFFFFFF none */
    uint32_t n_disks;        /* disks[0] is the target */
    char     disks[STN_BK_MAX_DISKS][STN_BK_PATH];
    uint32_t n_runs;
    stn_run  runs[STN_BK_MAX_RUNS];
} stn_backup;

stn_status stn_backup_encode(const stn_backup *b, uint8_t *hdr);
stn_status stn_backup_decode(const uint8_t *hdr, size_t len, stn_backup *b);

/* Adds a disk (returning its index) or a run. */
int        stn_backup_disk(stn_backup *b, const char *path);
stn_status stn_backup_run(stn_backup *b, uint32_t disk, stn_run_kind kind,
                          uint64_t lba, uint64_t sectors);

/* Does `remove` write this run back? The boot code, stage 2's gap and the
 * ESP's boot sector always; with `full` also the partition tables, the
 * station partition's first sectors and the other disks' signatures, which
 * is the disk as it was before the install (the station partition's entry
 * gone with the table it was added to). */
int stn_run_restores(const stn_run *r, int full);

/* Nonzero when an LBA 0 run goes back whole (the table and signature
 * with it) rather than as its boot code alone. */
int stn_run_whole_lba0(const stn_run *r, int full);

/* ---- the ESP's names (DESIGN Section 12.2) ---- */

/* The loader's name in \EFI\falcon with no shim in front of it. */
#define STN_LOADER_NOSHIM "falconx64.efi"
/* The name an install from before the jumper gave it: status and totp
 * still find it, and update renames it (DESIGN Section 12.14). */
#define STN_LOADER_OLD    "grubx64.efi"

/* The no-shim loader's name on an ESP that holds `has_new` (falconx64.efi)
 * and `has_old` (grubx64.efi): the old name only when it alone is there. */
const char *stn_noshim_loader(int has_new, int has_old);

/* SBAT's component name in the jumper's .sbat (apps/esp-jump/efi_sbat.c). */
#define STN_JUMP_SBAT "falcon-esp-jump"

/* Nonzero when img (n bytes, a whole file) is a PE image whose .sbat
 * section's second row names STN_JUMP_SBAT: the jumper falcon writes as
 * \EFI\BOOT\BOOTX64.EFI, and nothing else's. */
int stn_is_esp_jump(const uint8_t *img, size_t n);

typedef enum {
    STN_JUMP_NONE = 0,    /* an install with shim: no jumper */
    STN_JUMP_WRITE,       /* the path is free: write it */
    STN_JUMP_RESIGN,      /* ours is there: sign and write it again */
    STN_JUMP_TAKE         /* another system's is there: save it, then write */
} stn_jump_action;

/* What install and update do with \EFI\BOOT\BOOTX64.EFI. An install
 * without shim always has the jumper there; one with shim never does
 * (DESIGN Section 12.2). */
stn_jump_action stn_jump_plan(int shim, int present, int ours);

/* ---- the firmware's boot entries (efibootmgr -v) ---- */

#define STN_FW_MAX 64u
#define STN_FW_PATH_MAX 128u

typedef struct {
    unsigned n;                       /* entries labelled falcon */
    uint16_t num[STN_FW_MAX];
    char     path[STN_FW_MAX][STN_FW_PATH_MAX];  /* File(...), or "" */
    unsigned n_order;                 /* BootOrder, as listed */
    uint16_t order[STN_FW_MAX];
} stn_fw;

/* Reads `efibootmgr -v`'s output: the entries whose description is
 * `label` (a whole word: "falcon", not "falcon-old") with the file their
 * path names, and BootOrder. 0, or -1 for NULL. */
int stn_fw_parse(const char *text, const char *label, stn_fw *out);

/* Nonzero when an entry's file is `want`: case and the separator ignored,
 * as FAT and the firmware ignore them. */
int stn_fw_path_is(const char *path, const char *want);

/* The index in fw of the falcon entry naming `want`, or -1. */
int stn_fw_find(const stn_fw *fw, const char *want);

/* BootOrder with `num` moved first, into out (cap entries). Returns the
 * count. */
unsigned stn_fw_order_first(const stn_fw *fw, uint16_t num, uint16_t *out,
                            unsigned cap);

/* ---- reach ---- */

typedef enum {
    STN_REACH_OK = 0,
    STN_REACH_PAST_CHS,     /* past 8 GB: a BIOS without extensions */
    STN_REACH_PAST_LBA28    /* past 128 GiB: a BIOS with 28-bit LBA */
} stn_reach;

/* Where a run ending at byte `end` (exclusive) stands. */
stn_reach stn_reach_of(uint64_t end);

#endif /* FALCON_MKSTICK_STATIONCORE_H */
