/* falcon-boot: GPT partition identification (docs/DESIGN.md Section 3).
 *
 * Why this module exists. On real hardware the stick sits next to internal
 * disks that may well carry an exFAT volume and a LUKS2 volume of their own,
 * so "any exFAT volume is p2" and "any LUKS2 volume is p3" are both wrong.
 * mkstick creates every partition on the stick, so the loader identifies its
 * own partitions by GPT partition type GUID and nothing else.
 *
 * Deliberately EFI-free, like src/exfat: the disk is reached through a byte
 * read callback, so the same parser runs over a firmware BlockIO handle in
 * the loader and over a plain image file in the host tests. The firmware
 * plumbing (PartitionInfo, device paths, handle enumeration) lives next door
 * in src/part/partefi.c, which is the only half that cannot be host-tested.
 */

#ifndef FALCON_PART_H
#define FALCON_PART_H

#include <stdint.h>
#include <stddef.h>

#define GPT_GUID_LEN 16u

/* ---- the falcon partition type GUIDs ----
 *
 * DO NOT CHANGE THESE VALUES. EVER.
 *
 * They are minted once (2026-09-13, uuidgen v4) and they are ABI with every
 * stick that has ever been written: a loader built with a different value
 * cannot find the partitions on a stick stamped with the old one, and a stick
 * stamped with a new value is invisible to every loader already in the field.
 * There is no version negotiation here and no room for one, because the whole
 * point of the GUID is that it is the thing both sides already agree on.
 *
 *   p2  ISO library (exFAT)   f6b6d562-22c3-450a-87aa-0316039657d6
 *   p3  UKI shelf (LUKS2)     9cb02e90-85f4-4af7-9af3-1f9d6a95feaf
 *
 * The other two partitions keep their standard types and are not minted here:
 *
 *   p1  EFI system partition  c12a7328-f81f-11d2-ba4b-00a0c93ec93b
 *       Standard. The firmware must recognise it to boot the stick at all,
 *       so it could never carry a falcon type. The loader does not match on
 *       it either: it is already running off p1 by the time it looks.
 *   p4  Linux LUKS (root)     ca7d7ccb-63ed-4c53-861c-1742536059cc
 *       Standard, and the loader NEVER matches p4 at all, by design. EFI
 *       never mounts root; p4 is opened by the initramfs after the handoff
 *       of DESIGN Section 5.1, and the loader has no business finding it.
 *       A p4 with a falcon type would only invite the loader to try.
 *
 * Stored as the on-disk 16-byte sequence rather than as a textual GUID: the
 * GPT entry, the PartitionInfo protocol and EFI_GUID all use the same
 * mixed-endian layout (Data1/2/3 little-endian, Data4 as written), so one
 * byte array is comparable against every source without a conversion step,
 * and there is exactly one place each constant is written down.
 */
extern const uint8_t falcon_guid_iso[GPT_GUID_LEN];
extern const uint8_t falcon_guid_shelf[GPT_GUID_LEN];
extern const uint8_t gpt_guid_esp[GPT_GUID_LEN];

/* Which of our partitions a scan is looking for. Used by the filter policy
 * below and by src/part/partefi.c; p1 and p4 have no role here on purpose. */
typedef enum {
    PART_ROLE_ISO   = 0,   /* p2, the exFAT ISO library */
    PART_ROLE_SHELF = 1    /* p3, the LUKS2 UKI shelf */
} part_role;

#define PART_ROLE_COUNT 2u

/* The type GUID a role matches, or NULL for a role outside the enum. */
const uint8_t *part_role_guid(part_role role);

/* Nonzero when two 16-byte type GUIDs are the same one. */
int gpt_guid_eq(const uint8_t *a, const uint8_t *b);

/* ---- the GPT parser ---- */

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    GPT_OK              =  0,
    GPT_ERR_IO          = -1, /* read callback failed */
    GPT_ERR_NOT_GPT     = -2, /* no "EFI PART" signature at LBA 1 */
    GPT_ERR_CRC         = -3, /* header or entry array failed its CRC-32 */
    GPT_ERR_BAD_ARG     = -4,
    GPT_ERR_UNSUPPORTED = -5  /* a well-formed GPT whose geometry is outside
                                 what this parser handles; see gpt_parse */
};

/* Read len bytes at byte offset off (disk-relative). Returns 0 on success,
 * nonzero on failure. Offsets and lengths handed to it are always multiples
 * of the block size passed to gpt_parse, which is what lets the EFI side
 * forward it straight to BlockIO->ReadBlocks. */
typedef int (*gpt_read_fn)(void *ctx, uint64_t off, uint64_t len, void *buf);

/* Largest block size this parser reads a header into; the spec caps a GPT
 * header at one block, and 4096 covers every drive we will meet. */
#define GPT_MAX_SECTOR 4096u

/* Largest partition entry array accepted, in bytes. The standard table is
 * 128 entries of 128 bytes = 16 KiB; the cap is twice that so an unusual but
 * sane table still parses while a corrupt size field cannot make us stream
 * gigabytes through the CRC. */
#define GPT_ARRAY_MAX 32768u

/* One used partition, decoded. Unused entries (all-zero type GUID) never
 * appear here, so index in this table is not the partition number; `number`
 * carries the 1-based GPT index the firmware and sgdisk both call p<N>. */
typedef struct {
    uint64_t start_lba;              /* first block, inclusive */
    uint64_t end_lba;                /* last block, INCLUSIVE, per the spec */
    uint32_t number;                 /* 1-based index in the entry array */
    uint8_t  type[GPT_GUID_LEN];     /* partition type GUID */
    uint8_t  unique[GPT_GUID_LEN];   /* unique partition GUID, i.e. PARTUUID;
                                        the tiebreaker DESIGN Section 3 keeps
                                        in the install manifest for the day
                                        two falcon sticks are plugged in */
} gpt_part;

/* Caller supplies the storage, as everywhere else in this loader: the parser
 * never allocates. `cap` entries are filled at most; `truncated` says a disk
 * had more used partitions than fitted. */
typedef struct {
    gpt_part *parts;
    uint32_t  cap;
    uint32_t  count;
    uint8_t   truncated;
} gpt_table;

/* Parses the PRIMARY GPT at LBA 1 of the disk reachable through `read`.
 *
 * Both CRC-32s are verified before a single entry is trusted, which is the
 * whole reason to parse the table ourselves rather than believe whatever the
 * bytes say. The entry array is therefore streamed twice: once to checksum
 * it, once to decode it. Reversing that order would mean acting on entries
 * that the checksum is about to reject.
 *
 * Primary header only, no backup-header fallback, and that is the policy
 * rather than an omission. The firmware has already parsed the same primary
 * table to publish the partition handles we are classifying, so a primary
 * that fails here is a disk the firmware did not enumerate either, and
 * quietly booting off a backup table would hide damage on a stick that wants
 * re-making rather than working around.
 *
 * GPT_ERR_UNSUPPORTED covers geometry this parser declines rather than
 * mis-reads: a header size outside 92..block_size, an entry size below the
 * spec's 128 or not dividing the block size evenly (so no entry ever straddles
 * a block boundary), or an array larger than GPT_ARRAY_MAX.
 *
 * block_size must be a power of two in 512..GPT_MAX_SECTOR. */
int gpt_parse(gpt_table *t, gpt_read_fn read, void *ctx, uint32_t block_size);

/* The partition starting at exactly this LBA, or NULL. The starting LBA is
 * what a HARDDRIVE device path node carries, so it is the key that ties a
 * firmware partition handle back to its entry in the parent disk's table. */
const gpt_part *gpt_find_start(const gpt_table *t, uint64_t start_lba);

/* Nonzero when the table holds at least one partition of this role. Drives
 * the fallback rule of DESIGN Section 3: the loader restricts itself to
 * falcon-typed partitions only when some exist. */
int gpt_has_role(const gpt_table *t, part_role role);

#endif /* FALCON_PART_H */
