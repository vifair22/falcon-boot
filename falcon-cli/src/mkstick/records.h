/* falcon-boot mkstick: a disk's partitions and the identity record each one
 * carries (docs/DESIGN.md Sections 3.1 and 3.2).
 *
 * Read with the loaders' own code: falcon-core's part_read_table for the
 * table, ident for the record's three encodings, luks for the token and the
 * fs registry for /.falcon. Nothing here mounts anything or needs root on an
 * image file, and nothing writes except records_write_boot.
 *
 * Where each record is read from, in the order tried:
 *
 *   - a LUKS2 header: its `falcon` token (a LUKS2 volume is never also a
 *     FAT boot sector, and its token is readable without a key);
 *   - a boot sector carrying the binary record at 0x1DC (the `boot`
 *     partition, FAT);
 *   - a filesystem the registry mounts: /.falcon at its root (exFAT p2, a
 *     plaintext ext4 shelf).
 */

#ifndef FALCON_MKSTICK_RECORDS_H
#define FALCON_MKSTICK_RECORDS_H

#include <stddef.h>
#include <stdint.h>

#include <common/status.h>
#include <ident/ident.h>
#include <part/table.h>

#define MKS_TABLE_MAX 128u

typedef enum {
    REC_NONE = 0,   /* no record here */
    REC_OK,         /* a record, decoded */
    REC_BAD         /* something record-shaped that does not decode */
} rec_state;

typedef struct {
    rec_state   state;
    ident_rec   rec;
    const char *where;   /* "boot sector", "LUKS2 token", "/.falcon" */
    int         luks;    /* the partition opens with a LUKS2 header */
    const char *fs;      /* the filesystem the registry found, or NULL */
} mks_rec;

typedef struct {
    int          rc;                        /* part_read_table's */
    part_table   table;
    part_entry   parts[MKS_TABLE_MAX];
    mks_rec      recs[MKS_TABLE_MAX];
} mks_records;

/* Reads the table of the disk or image at `path` and every partition's
 * record. CLI_OK whenever the disk could be opened; a disk with no table, or
 * a damaged one, reports it in r->rc (PART_ERR_*) with no partitions. */
cli_status records_read(mks_records *r, const char *path, uint32_t sector,
                        uint64_t size);

/* The index of the first partition carrying a record of this role, or -1.
 * `from` starts the search, so a caller can look for a second one. */
int records_find(const mks_records *r, ident_role role, int from);

/* Nonzero when this entry is one a loader reads for a `boot` record: on GPT
 * the ESP or the falcon boot type, on MBR a FAT type byte or its hidden
 * form (docs/notes/station-rework.md section 9). */
int records_boot_candidate(const part_table *t, const part_entry *p);

/* The index of the boot partition: the first candidate whose record decodes
 * with role boot. -1 when there is none. */
int records_boot(const mks_records *r);

/* Writes a `boot` record into the FAT boot sector at byte `base` of `path`
 * (a partition device, or a disk or image with the partition's offset), and
 * into FAT32's backup boot sector too, so fsck.fat, which compares the two
 * and offers to copy the backup over, cannot drop it. Refuses a sector that
 * is not a FAT boot sector. */
cli_status records_write_boot(const char *path, uint64_t base,
                              const ident_rec *rec);

/* A boot record's placement as a command line gives it (`mkstick ident
 * --write-boot-at`, for a volume with no partition table: jump's floppy):
 * a byte offset, decimal or 0x hex and a multiple of 512, and the stage 3
 * and marker sectors, relative to the volume, below 65536. Stage 3 is at
 * least sector 1; a marker of 0 is none. Fills *off, and the record's
 * version, role and sectors; the install ID is left to the caller. 1 on
 * success; 0 with *why naming what was wrong. */
int records_boot_args(const char *offset, const char *stage3,
                      const char *marker, uint64_t *off, ident_rec *rec,
                      const char **why);

/* A fresh install ID from the kernel's random source. */
cli_status records_new_id(uint8_t id[IDENT_ID_LEN]);

/* 32 lowercase hex digits and a NUL, and back. */
void records_id_hex(const uint8_t id[IDENT_ID_LEN], char out[33]);
int  records_id_parse(const char *hex, uint8_t id[IDENT_ID_LEN]);

#endif /* FALCON_MKSTICK_RECORDS_H */
