/* falcon-boot: the GPT parser and the partition type GUIDs (docs/DESIGN.md
 * Sections 3.1 and 3.2).
 *
 * What a partition IS is its identity record (falcon-core/src/ident), not
 * anything here: the types below only narrow the search, and the table-
 * neutral view every consumer uses is part/table.h, which calls this parser
 * for a GPT disk.
 *
 * Deliberately EFI-free, like falcon-core/src/fs/exfat.c: the disk is reached
 * through a byte read callback, so the same parser runs over a firmware BlockIO
 * handle in the loader and over a plain image file in the host tests.
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
 * They are ABI with every stick that has ever been written: the readers
 * narrow their search for records by them (stage 2 reads a boot record only
 * off an ESP- or falcon-boot-typed GPT partition), and Windows decides by
 * them which partitions get a volume. A changed value strands every stick
 * already in the field.
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
/* A station's `boot` partition on a GPT disk (docs/DESIGN.md Section 13.2):
 * d0acb249-f895-4e0d-ab0d-cfbb6f48ae36, minted 2026-09-26. Unrecognised by
 * Windows, so it gets no volume; stage 2 reads only ESP- and this-typed
 * partitions on GPT when it looks for the `boot` record. */
extern const uint8_t falcon_guid_boot[GPT_GUID_LEN];

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

    /* The header's usable range (spec Table 5-5), filled by gpt_parse.
     *
     * Not about partitions at all, which is why it sits beside them rather
     * than inside a gpt_part: it is what the disk says may be partitioned,
     * and the interesting number is the space OUTSIDE it. first_usable is
     * normally 34 (one protective MBR, one header, 32 blocks of entry
     * array), while the first partition is aligned to 2048, and the sectors
     * in between belong to nobody. That gap is where falcon-bios' stages go
     * (falcon-cli/src/mkstick/bioscore.h), so the tool that writes them has
     * to read this rather than assume the usual numbers. */
    uint64_t  first_usable;
    uint64_t  last_usable;
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

/* The header fields a caller past gpt_parse needs: part/table.c keeps the
 * disk's GUID (DESIGN Section 3.2) and reads the backup copy, which is why
 * the header's own LBA is a parameter below rather than fixed at 1. */
typedef struct {
    uint64_t my_lba;
    uint64_t alt_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t  disk_guid[GPT_GUID_LEN];
} gpt_header;

/* Called once per used entry, in array order, only after both CRCs have
 * passed. */
typedef void (*gpt_entry_fn)(void *cb_ctx, const gpt_part *p);

/* gpt_parse's engine, for a header at any LBA: the header must say it lives
 * at `lba` (MyLBA), so a primary is read at 1 and a backup at the disk's
 * last block. The same checks and status codes as gpt_parse; *h is filled
 * once the header's own CRC has passed, the entries only once the array's
 * has too. `fn` may be NULL to validate without decoding. */
int gpt_read_at(gpt_read_fn read, void *ctx, uint32_t block_size, uint64_t lba,
                gpt_header *h, gpt_entry_fn fn, void *cb_ctx);

/* The partition starting at exactly this LBA, or NULL. The starting LBA is
 * what a HARDDRIVE device path node carries, so it is the key that ties a
 * firmware partition handle back to its entry in the parent disk's table. */
const gpt_part *gpt_find_start(const gpt_table *t, uint64_t start_lba);

#endif /* FALCON_PART_H */
