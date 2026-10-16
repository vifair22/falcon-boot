/* falcon-boot mkstick: `station`, the station's installer (docs/DESIGN.md
 * Section 12.9), shared between its three subcommands.
 *
 * station.c holds the dispatch, what the installer finds on the machine's
 * disks (the scan), the FAT reads and mtools writes both install and status
 * need, and `remove` and `status`; stationinst.c is `install`. The
 * decisions (where, what, which name) are stationcore.c's.
 */

#ifndef FALCON_MKSTICK_STATION_H
#define FALCON_MKSTICK_STATION_H

#include <stddef.h>
#include <stdint.h>

#include <common/opt.h>
#include <common/status.h>
#include <mkstick/devio.h>
#include <mkstick/disk.h>
#include <mkstick/records.h>
#include <mkstick/stationcore.h>

#define STN_MAX_DISKS 8u
#define STN_MAX_FOUND 32u

/* The files a station carries (DESIGN Section 12). */
#define STN_ESP_DIR       "EFI/falcon"
#define STN_ESP_SHIM      "shimx64.efi"
/* The loader's name is shim's next stage when shim is there, and
 * stationcore.h's STN_LOADER_NOSHIM when it is not. */
#define STN_ESP_CERT      "falcon-boot.cer"
/* The removable-media path, where an install without shim puts the jumper
 * (apps/esp-jump), and which its firmware entry names. */
#define STN_ESP_BOOT_DIR  "EFI/BOOT"
#define STN_ESP_JUMP      "BOOTX64.EFI"
#define STN_ESP_JUMP_PATH "\\EFI\\BOOT\\BOOTX64.EFI"
/* Beside the install's backup file: the BOOTX64.EFI the jumper replaced. */
#define STN_JUMP_BACKUP_EXT ".BOOTX64.EFI"
#define STN_ESP_MEMTEST   "memtest.efi"
#define STN_BIOS_MEMTEST  "MEMTEST.BIN"
#define STN_FW_LABEL      "falcon"
#define STN_FAT_LABEL     "FALCONSTN"
#define STN_GPT_NAME      "FALCON-STATION"
#define STN_TIMEOUT       5u

/* The first sectors of a new station partition kept in the backup: its
 * reserved sectors, then this many bytes past them. What mkfs.fat and the
 * installer write (both FATs, the root cluster and the files) lies inside
 * them for any size the installer makes, at either sector size: the
 * reserved area alone is 8 MiB on a 4Kn disk (bioscore_reserved_for counts
 * its MiB in 512-byte sectors). */
#define STN_REGION_BACKUP_BYTES (8u * 1024u * 1024u)

/* One disk the scan read. */
typedef struct {
    mks_disk    d;
    mks_records r;
    uint8_t     lba0[512];
    int         is_target;
    int         needs_sig;      /* signature 0, and the INI names it */
    uint32_t    new_sig;
} stn_disk;

typedef struct {
    stn_disk  disks[STN_MAX_DISKS];
    size_t    n_disks;
    stn_found found[STN_MAX_FOUND];
    size_t    n_found;
    /* The shelf, when there is one: its disk (index into disks) and the
     * partition's index in that disk's records. */
    int       shelf_disk;
    int       shelf_part;
    int       shelf_luks;
} stn_scan;

/* Reads the target (disks[0]) and every other disk the machine has (a
 * block-device target, unless `only_named`) and what --also names. An
 * image target's machine is the image and what --also names. */
cli_status stn_scan_disks(stn_scan *s, const char *target,
                          const char *const *also, size_t n_also,
                          int only_named);

/* What the loaders will find on those disks (DESIGN Section 12.3): the
 * partitions whose boot sector names a loader, the other disks whose MBR is
 * worth chaining; with `uefi`, Windows on a disk's ESP. The target's own
 * MBR and falcon's partitions are not offered. */
void stn_scan_found(stn_scan *s, int uefi);

/* The firmware's boot entries on the running machine (efibootmgr), when it
 * booted with UEFI: Windows Boot Manager as a found entry, and the number
 * of an entry already labelled falcon. */
void stn_scan_firmware(stn_scan *s, int *falcon_entry);

/* FAT and the other filesystems core reads, over a partition of a disk or
 * image: a whole file into a buffer. FS_OK or the driver's status. */
int stn_read_file(const char *disk, uint64_t off, uint64_t len,
                  const char *path, char *buf, size_t cap, size_t *got);

/* mtools over a FAT volume at a byte offset of a disk or image: no mount
 * and no root for an image. */
cli_status stn_mtools_mkdir(const char *disk, uint64_t off, const char *dir);
cli_status stn_mtools_put(const char *disk, uint64_t off, const char *src,
                          const char *dst);
cli_status stn_mtools_put_text(const char *disk, uint64_t off,
                               const char *text, size_t len, const char *dst);
cli_status stn_mtools_deltree(const char *disk, uint64_t off,
                              const char *dir);
cli_status stn_mtools_del(const char *disk, uint64_t off, const char *file);
/* A file off the volume (src, under its root) to a host path. */
cli_status stn_mtools_get(const char *disk, uint64_t off, const char *src,
                          const char *dst);

/* Is partition `number` of this disk mounted? Always 0 for an image. */
int stn_part_mounted(const mks_disk *d, uint32_t number, char *where,
                     size_t cap);

/* Nonzero for a disk of the machine the command runs on: a block device
 * that is not a loop device. An image, attached or not, is its own
 * machine's, whose disks and firmware entries this one cannot see. */
int stn_is_machine(const mks_disk *d);

/* The index in r of the partition numbered n, or -1. */
int stn_part_index(const mks_records *r, uint32_t n);


/* The ESP: GPT's type, or MBR 0xEF; -1 when there is none. */
int stn_find_esp(const mks_records *r);

/* The loader's name in \EFI\falcon of the FAT volume at [off, off + len)
 * of disk: the next stage shimx64.efi there names, or stn_noshim_loader's
 * when there is no shim (--no-shim). What install wrote, so what status,
 * totp and update read. */
cli_status stn_esp_loader_name(const char *disk, uint64_t off, uint64_t len,
                               char *out, size_t cap);

/* \EFI\BOOT\BOOTX64.EFI on that volume: *present when it is there, *ours
 * when it is falcon's jumper (stn_is_esp_jump). */
cli_status stn_esp_jump_state(const char *disk, uint64_t off, uint64_t len,
                              int *present, int *ours);

/* Can the firmware entries be read and written for this disk: a disk of
 * this machine, which booted with UEFI, with efibootmgr on PATH. */
int stn_fw_here(const mks_disk *d);

/* This machine's entries labelled falcon, and BootOrder. */
cli_status stn_fw_read(stn_fw *fw);

/* The `falcon` entry naming STN_ESP_JUMP_PATH on partition `part` of disk,
 * first in BootOrder: any falcon entry naming another file deleted, the
 * jumper's made when there is none, BootOrder rewritten when it is not
 * first. *num is its number. */
cli_status stn_fw_jump_entry(const char *disk, uint32_t part, int *num);

extern const cli_cmd stn_cmd_install;

/* `station totp ...`, the UEFI station's TOTP check (totp.c): argv as
 * `station` got it with the word totp taken out. */
cli_status stn_totp_main(int argc, char **argv);

/* A PE image's Authenticode digest, SHA-1 (sha1) or SHA-256: what the
 * firmware and shim measure it as. */
cli_status stn_image_digest(const uint8_t *img, size_t n, int sha1,
                            uint8_t out[32]);

/* `station totp reseal` for mkstick update, run right after it wrote a new
 * loader and before the reboot that would run it: this boot's measurement
 * of the loader it replaced (old_sha1, old_sha256) is taken as the new
 * one's, so the seal holds what the new loader will read (DESIGN Section
 * 12.15). The enrolled [totp]'s PCRs and label are kept; the seed is new,
 * and printed as enroll prints it. The last four are reseal's options, for
 * the rig; NULL for the machine's own TPM and logs. */
typedef struct {
    const char    *disk;
    const uint8_t *old_sha1;
    const uint8_t *old_sha256;
    const char    *owner_auth_file;
    const char    *tpm_socket;
    const char    *event_log;
    const char    *saved_log;
} stn_totp_update;

cli_status stn_totp_reseal_update(const stn_totp_update *u);

#endif /* FALCON_MKSTICK_STATION_H */
