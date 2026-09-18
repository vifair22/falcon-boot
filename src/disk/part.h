/* falcon-boot: GPT partition identification (docs/DESIGN.md Section 3).
 *
 * Why this module exists. On real hardware the stick sits next to internal
 * disks that may well carry an exFAT volume and a LUKS2 volume of their own,
 * so "any exFAT volume is p2" and "any LUKS2 volume is p3" are both wrong.
 * mkstick creates every partition on the stick, so the loader identifies its
 * own partitions out of the GPT entry: p3 by its falcon type GUID, p2 by the
 * pair (Microsoft basic data type, partition name FALCON-ISO). See
 * part_match_role below for why p2 is the odd one out and why both halves of
 * its rule are required.
 *
 * Deliberately EFI-free, like src/fs/exfat.c: the disk is reached through a byte
 * read callback, so the same parser runs over a firmware BlockIO handle in
 * the loader and over a plain image file in the host tests. The firmware
 * plumbing (PartitionInfo, device paths, handle enumeration) lives next door
 * in src/disk/partefi.c, which is the only half that cannot be host-tested.
 */

#ifndef FALCON_PART_H
#define FALCON_PART_H

#include <stdint.h>
#include <stddef.h>

#define GPT_GUID_LEN 16u

/* A GPT entry's partition name: 36 UTF-16 code units, NUL-PADDED rather than
 * NUL-terminated (UEFI spec 5.3.3, Table 5-6). Nothing on disk guarantees a
 * terminator, so every comparison here walks all 36 units and none of them
 * calls a string routine on the field. */
#define GPT_NAME_LEN 36u

/* ---- the partition types a stick is stamped with ----
 *
 * DO NOT CHANGE THESE VALUES. EVER.
 *
 * They are ABI with every stick that has ever been written: a loader built
 * with a different value cannot find the partitions on a stick stamped with
 * the old one, and a stick stamped with a new value is invisible to every
 * loader already in the field. There is no version negotiation here and no
 * room for one, because the whole point of the constant is that it is the
 * thing both sides already agree on.
 *
 *   p2  ISO library (exFAT)   ebd0a0a2-b9e5-4433-87c0-68b6b72699c7
 *       Microsoft basic data. NOT a falcon-minted GUID, and that is the
 *       decision of 2026-09-13 rather than an oversight: Windows' partmgr
 *       creates no volume for a GPT partition whose type it does not
 *       recognise, so a falcon-typed p2 gets no drive letter, cannot be
 *       dragged and dropped onto from a Windows host (DESIGN Section 1,
 *       in-scope item 2) and cannot be found by the jump binary inside WinPE
 *       (Section 6.3). Measured; see docs/notes/windows-route.md. p2 must be
 *       visible to Windows, so p2 wears a type Windows knows.
 *   p3  UKI shelf (LUKS2)     9cb02e90-85f4-4af7-9af3-1f9d6a95feaf
 *       Minted once (2026-09-13, uuidgen v4) and kept, for the mirror-image
 *       reason: p3 must NOT be visible to Windows. A basic-data-typed LUKS
 *       volume is one Windows offers to format, and the offer is aimed at
 *       the partition holding the stick's only copy of its MOK private key.
 *       An unrecognised type is what keeps Windows' hands off it.
 *
 * The other two partitions keep their standard types:
 *
 *   p1  EFI system partition  c12a7328-f81f-11d2-ba4b-00a0c93ec93b
 *       Standard. The firmware must recognise it to boot the stick at all,
 *       so it could never carry a falcon type. The loader does not match on
 *       it either: it is already running off p1 by the time it looks.
 *   p4  Linux LUKS (root)     ca7d7ccb-63ed-4c53-861c-1742536059cc
 *       Standard, unrecognised by Windows for the same deliberate reason as
 *       p3, and the loader NEVER matches p4 at all, by design. EFI never
 *       mounts root; p4 is opened by the initramfs after the handoff of
 *       DESIGN Section 5.1, and the loader has no business finding it.
 *
 * Stored as the on-disk 16-byte sequence rather than as a textual GUID: the
 * GPT entry, the PartitionInfo protocol and EFI_GUID all use the same
 * mixed-endian layout (Data1/2/3 little-endian, Data4 as written), so one
 * byte array is comparable against every source without a conversion step,
 * and there is exactly one place each constant is written down.
 */
extern const uint8_t gpt_guid_msbasic[GPT_GUID_LEN];
extern const uint8_t falcon_guid_shelf[GPT_GUID_LEN];
extern const uint8_t gpt_guid_esp[GPT_GUID_LEN];

/* p2's discriminator, since its type GUID is one half the world shares.
 * mkstick writes this into the GPT entry's name field and the loader matches
 * it there; MKS_LABEL_P2 in src/cli/mkstick/mkstick.h is the same string and
 * is checked against this one at startup. */
#define FALCON_PART_NAME_ISO "FALCON-ISO"

/* Which of our partitions a scan is looking for. Used by the filter policy
 * below and by src/disk/partefi.c; p1 and p4 have no role here on purpose. */
typedef enum {
    PART_ROLE_ISO   = 0,   /* p2, the exFAT ISO library */
    PART_ROLE_SHELF = 1    /* p3, the LUKS2 UKI shelf */
} part_role;

#define PART_ROLE_COUNT 2u

/* The GPT partition name a role is identified by, or NULL for a role that is
 * identified by its type GUID alone (p3) and for a role outside the enum. */
const char *part_role_name(part_role role);

/* Nonzero when two 16-byte type GUIDs are the same one. */
int gpt_guid_eq(const uint8_t *a, const uint8_t *b);

/* Nonzero when a GPT entry's 36-unit name field holds exactly this ASCII
 * string, padded with NULs to the full 36 units.
 *
 * Exact and CASE-SENSITIVE, deliberately. The field is attacker-editable data
 * off removable media, so this walks all GPT_NAME_LEN units, reads no further
 * than that, and assumes no terminator: a field packed with 36 non-NUL units
 * simply fails, and so does one with our name followed by anything other than
 * NUL padding. Case-folding was considered and refused - doing it honestly
 * over UTF-16 needs a Unicode table this freestanding binary has no business
 * carrying, and folding only the ASCII range while calling the result
 * case-insensitive is the kind of half-rule that surprises somebody later. We
 * write the name ourselves (mkstick), so we can afford to match it exactly. */
int gpt_name_eq(const uint16_t *name, const char *ascii);

/* THE identification rule of DESIGN Section 3, in one place so the firmware
 * side (src/disk/partefi.c, off the PartitionInfo protocol) and the host side
 * (a parsed gpt_part) cannot drift apart:
 *
 *   p2  Microsoft basic data type AND the name FALCON-ISO. BOTH, because the
 *       type alone is worn by every Windows data partition in the machine,
 *       and the name alone on a partition of some unrelated type is far more
 *       likely to be a coincidence (or a hostile label on somebody's disk)
 *       than it is to be ours. Requiring both costs nothing - we write both -
 *       and a wrong guess here aims the ISO scan at a stranger's volume.
 *   p3  its falcon type GUID, which no one else wears.
 *
 * `name` may be NULL when the caller could not establish one; p2 then never
 * matches, because half the rule is missing. */
int part_match_role(const uint8_t *type, const uint16_t *name, part_role role);

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
    uint16_t name[GPT_NAME_LEN];     /* the entry's UTF-16 name, decoded from
                                        little-endian; NUL-padded, and NOT
                                        necessarily NUL-terminated */
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

/* Does this decoded entry satisfy part_match_role for the role? */
int gpt_part_is_role(const gpt_part *p, part_role role);

/* Nonzero when the table holds at least one partition of this role. Drives
 * the fallback rule of DESIGN Section 3: the loader restricts itself to
 * falcon partitions only when some exist. */
int gpt_has_role(const gpt_table *t, part_role role);

#endif /* FALCON_PART_H */
