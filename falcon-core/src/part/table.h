/* falcon-core: a disk's partition table, MBR or GPT, as one list
 * (docs/DESIGN.md Section 3.2).
 *
 * Every consumer (both loaders, stage 2, the host tools) reads partitions
 * through this and never through a table-specific parser, so an MBR disk and
 * a GPT disk look the same above it: a list of part_entry with the disk's own
 * identity beside it. What a partition IS comes from its identity record
 * (ident/ident.h), not from anything here; the type fields only narrow where
 * to look.
 *
 * The rules, in the order they are applied:
 *
 *   - LBA 0 is read first. A valid MBR (0x55AA, sane entries) with ANY 0xEE
 *     entry means the disk is GPT, and the MBR's other entries are ignored:
 *     Linux's rule, which keeps a hybrid MBR a Windows-only view.
 *   - GPT: the primary header and array at LBA 1, both CRC-checked. When the
 *     primary fails, the backup at the disk's last LBA (needs disk_blocks),
 *     and used_backup says so.
 *   - MBR: the four primary entries, then the extended chain (types 0x05,
 *     0x0F, 0x85) for logical partitions, numbered as Linux numbers them:
 *     1-4 primary by slot, 5 onward logical in chain order. The chain is
 *     bounded (PART_MAX_LOGICAL) and every link must stay inside the
 *     extended partition, so a looping or escaping chain ends the walk.
 *     The extended partition itself is listed too, as Linux lists it, with
 *     its type byte (part_mbr_is_extended); it is a container, not a
 *     volume, and the logicals inside it are not overlaps.
 *   - Either table: every entry must lie inside the disk (when its length
 *     is known), start after LBA 0, and share no block with another, or
 *     the table is PART_ERR_CORRUPT.
 *   - Neither: PART_ERR_NO_TABLE. A floppy or superfloppy is not an error
 *     to its callers, who then treat the disk as one volume from LBA 0.
 *
 * EFI-free and allocation-free: the disk is a read callback and the caller
 * owns every buffer.
 */

#ifndef FALCON_PART_TABLE_H
#define FALCON_PART_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include <part/part.h>

typedef enum {
    PART_KIND_MBR = 1,
    PART_KIND_GPT = 2
} part_kind;

/* How falcon names a disk (Section 3.2): the GPT DiskGUID, or the MBR disk
 * signature at offset 440. Never a BIOS drive number or a handle order. */
typedef struct {
    part_kind kind;
    uint8_t   guid[GPT_GUID_LEN];   /* GPT: the header's DiskGUID */
    uint32_t  mbr_sig;              /* MBR: offset 440, little endian; 0 is
                                       a disk with none (DOS, Win9x fdisk) */
} part_disk_id;

/* One used partition. For GPT, type_guid/unique/name are the entry's and
 * mbr_type is 0; for MBR, mbr_type/active/primary are the entry's and the
 * GUID fields are zero. */
typedef struct {
    uint64_t start_lba;              /* first block */
    uint64_t end_lba;                /* last block, inclusive */
    uint32_t number;                 /* GPT: 1-based index; MBR: 1-4, 5+ */
    uint8_t  primary;                /* MBR: 1 for slots 1-4; GPT: 1 */
    uint8_t  active;                 /* MBR: boot flag 0x80; GPT: 0 */
    uint8_t  mbr_type;               /* MBR: the type byte; GPT: 0 */
    uint8_t  type_guid[GPT_GUID_LEN];
    uint8_t  unique[GPT_GUID_LEN];
    uint16_t name[GPT_NAME_LEN];
} part_entry;

#define PART_MAX_LOGICAL 64u

typedef struct {
    part_entry  *parts;              /* caller's storage */
    uint32_t     cap;
    uint32_t     count;
    uint8_t      truncated;          /* more partitions than cap */
    uint8_t      used_backup;        /* GPT: the primary failed its CRC */
    part_disk_id id;
    uint64_t     first_usable;       /* GPT: header's range; MBR: 1 and the
                                        last block before disk_blocks, or
                                        0 when disk_blocks is unknown */
    uint64_t     last_usable;
    uint64_t     first_start;        /* lowest partition start: what lies
                                        before it is the boot area (3.3) */
} part_table;

enum {
    PART_OK           =  0,
    PART_ERR_IO       = -1,
    PART_ERR_NO_TABLE = -2,  /* neither an MBR nor a GPT */
    PART_ERR_CORRUPT  = -3,  /* GPT with both copies failing, or either
                                table's entries overlapping or leaving
                                the disk */
    PART_ERR_ARG      = -4,
    PART_ERR_UNSUPPORTED = -5
};

/* Reads the table of the disk behind `read`. block_size is the disk's
 * logical sector size (512 or 4096); disk_blocks its length in blocks, or 0
 * when unknown (then no backup-GPT fallback and no end-of-disk checks). */
int part_read_table(part_table *t, gpt_read_fn read, void *ctx,
                    uint32_t block_size, uint64_t disk_blocks);

/* Nonzero for the MBR types that hold an extended chain: 0x05, 0x0F,
 * 0x85. */
int part_mbr_is_extended(uint8_t type);

/* The entry starting at exactly this LBA, or NULL. */
const part_entry *part_find_start(const part_table *t, uint64_t start_lba);

/* Parses "gpt:<DiskGUID as text>" or "mbr:<8 hex digits>" (STATION.INI's
 * spelling) into `out`; 0 on success, PART_ERR_ARG for anything else. The
 * GUID is the standard 8-4-4-4-12 text, either case in; the output is
 * lowercase. And the reverse, into `buf` (at least
 * PART_DISK_ID_TEXT bytes, NUL-terminated). */
#define PART_DISK_ID_TEXT 42u
int  part_disk_id_parse(const char *s, size_t len, part_disk_id *out);
void part_disk_id_format(const part_disk_id *id, char *buf);

/* Nonzero when both name the same disk. An MBR id with signature 0, or a
 * GPT id whose DiskGUID is all zero, names nothing and never matches. */
int part_disk_id_eq(const part_disk_id *a, const part_disk_id *b);

#endif /* FALCON_PART_TABLE_H */
