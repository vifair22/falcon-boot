/* GPT partition table parsing, per UEFI specification 5.3 (GUID Partition
 * Table). Field offsets referenced below are offsets into the header block
 * and into a partition entry, taken from Tables 5-5 and 5-6 of that section.
 *
 * EFI-free by construction: everything here reaches the disk through the
 * caller's read callback, so the host tests drive it over a plain file.
 */

#include <part/part.h>
#include <lib/string.h>

/* The partition type GUIDs, in on-disk byte order. The textual form and the
 * reason these are frozen are in part.h; do not edit either copy. */
const uint8_t gpt_guid_msbasic[GPT_GUID_LEN] = {
    /* ebd0a0a2-b9e5-4433-87c0-68b6b72699c7, Microsoft basic data: what p2
     * wears so that Windows will give it a drive letter. Shared with every
     * NTFS/exFAT data partition in the world, which is why matching p2 needs
     * the partition name as well (part_match_role). */
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
    0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7
};

const uint8_t falcon_guid_shelf[GPT_GUID_LEN] = {
    /* 9cb02e90-85f4-4af7-9af3-1f9d6a95feaf */
    0x90, 0x2e, 0xb0, 0x9c, 0xf4, 0x85, 0xf7, 0x4a,
    0x9a, 0xf3, 0x1f, 0x9d, 0x6a, 0x95, 0xfe, 0xaf
};

const uint8_t gpt_guid_esp[GPT_GUID_LEN] = {
    /* c12a7328-f81f-11d2-ba4b-00a0c93ec93b, the standard EFI system
     * partition type. Present so the tests can name it and so nobody is
     * tempted to mint a falcon GUID for p1. */
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

/* GPT header layout (spec Table 5-5). */
#define HDR_SIGNATURE      0u   /* "EFI PART", 8 bytes */
#define HDR_HEADER_SIZE   12u
#define HDR_HEADER_CRC32  16u
#define HDR_MY_LBA        24u
#define HDR_ENTRY_LBA     72u
#define HDR_ENTRY_COUNT   80u
#define HDR_ENTRY_SIZE    84u
#define HDR_ENTRY_CRC32   88u
#define HDR_SIZE_MIN      92u   /* spec: at least this, and at most one block */

/* Partition entry layout (spec Table 5-6). */
#define ENT_TYPE_GUID      0u
#define ENT_UNIQUE_GUID   16u
#define ENT_START_LBA     32u
#define ENT_END_LBA       40u
#define ENT_NAME          56u   /* 36 UTF-16 units, to byte 127 */
#define ENT_SIZE_MIN     128u   /* spec: 128 * 2^n */

#define CRC32_INIT 0xFFFFFFFFu

/* CRC-32 exactly as the GPT uses it (spec 5.3.1): the reflected IEEE 802.3 /
 * zlib polynomial 0xEDB88320, initial and final value 0xFFFFFFFF.
 *
 * This is NOT the CRC-32C (Castagnoli, reflected 0x82F63B78) that
 * falcon-core/src/fs/ext4.c checks its metadata with. The two share a name, a
 * width and nothing else; pointing either implementation at the other's data
 * produces a confident wrong answer, so neither may be "unified" with the
 * other.
 *
 * A four-bit table costs two lookups per byte and 64 bytes of rodata, which
 * is the right trade here: the loader checksums at most a few tens of
 * kilobytes per disk, and the 1 KiB byte-wise table would be pure weight in a
 * binary that has to stay auditable. */
static const uint32_t crc32_nib[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        crc ^= (uint32_t)p[i];
        crc = (crc >> 4) ^ crc32_nib[crc & 0xFu];
        crc = (crc >> 4) ^ crc32_nib[crc & 0xFu];
    }
    return crc;
}

int gpt_guid_eq(const uint8_t *a, const uint8_t *b)
{
    if (a == NULL || b == NULL)
        return 0;
    return memcmp(a, b, GPT_GUID_LEN) == 0;
}

int gpt_name_eq(const uint16_t *name, const char *ascii)
{
    unsigned i;
    int ended = 0;

    if (name == NULL || ascii == NULL)
        return 0;

    /* All GPT_NAME_LEN units, every time: the field carries no terminator of
     * its own, and the ASCII side is only read until its own NUL (the
     * short-circuit and the conditional below are what keep this from
     * stepping past it). */
    for (i = 0; i < GPT_NAME_LEN; i++) {
        uint16_t want;

        if (!ended && ascii[i] == '\0')
            ended = 1;
        want = ended ? (uint16_t)0 : (uint16_t)(unsigned char)ascii[i];
        if (name[i] != want)
            return 0;
    }
    return 1;
}

const char *part_role_name(part_role role)
{
    if (role == PART_ROLE_ISO)
        return FALCON_PART_NAME_ISO;
    return NULL;
}

int part_match_role(const uint8_t *type, const uint16_t *name, part_role role)
{
    switch (role) {
    case PART_ROLE_ISO:
        /* Both halves, per part.h: the type is the world's and the name is
         * ours, so neither identifies p2 on its own. */
        if (!gpt_guid_eq(type, gpt_guid_msbasic))
            return 0;
        return gpt_name_eq(name, FALCON_PART_NAME_ISO);
    case PART_ROLE_SHELF:
        return gpt_guid_eq(type, falcon_guid_shelf);
    default:
        return 0;
    }
}

int gpt_part_is_role(const gpt_part *p, part_role role)
{
    if (p == NULL)
        return 0;
    return part_match_role(p->type, p->name, role);
}

/* An all-zero type GUID marks an unused entry (spec 5.3.3); such entries are
 * not partitions and never reach the caller's table. */
static int guid_is_zero(const uint8_t *g)
{
    unsigned i;

    for (i = 0; i < GPT_GUID_LEN; i++) {
        if (g[i] != 0)
            return 0;
    }
    return 1;
}

int gpt_parse(gpt_table *t, gpt_read_fn read, void *ctx, uint32_t block_size)
{
    uint8_t sec[GPT_MAX_SECTOR];
    uint32_t hdr_size, ent_size, ent_count, per_block, idx, crc, want;
    uint64_t ent_lba, ent_base, array_bytes, done;

    if (t == NULL || t->parts == NULL || t->cap == 0 || read == NULL)
        return GPT_ERR_BAD_ARG;
    if (block_size < 512u || block_size > GPT_MAX_SECTOR ||
        (block_size & (block_size - 1u)) != 0u)
        return GPT_ERR_BAD_ARG;

    t->count = 0;
    t->truncated = 0;

    /* The primary header is always LBA 1, whatever the block size. */
    if (read(ctx, block_size, block_size, sec) != 0)
        return GPT_ERR_IO;
    if (memcmp(sec + HDR_SIGNATURE, "EFI PART", 8) != 0)
        return GPT_ERR_NOT_GPT;

    hdr_size = get_le32(sec + HDR_HEADER_SIZE);
    if (hdr_size < HDR_SIZE_MIN || hdr_size > block_size)
        return GPT_ERR_UNSUPPORTED;

    /* The stored CRC covers the header with its own CRC field zeroed, so the
     * field is saved and then cleared in place before checksumming. */
    want = get_le32(sec + HDR_HEADER_CRC32);
    memset(sec + HDR_HEADER_CRC32, 0, 4);
    crc = crc32_update(CRC32_INIT, sec, hdr_size) ^ 0xFFFFFFFFu;
    if (crc != want)
        return GPT_ERR_CRC;

    /* A header that claims to live somewhere other than where we read it is
     * a backup header copied into place, or corruption the CRC happens to
     * agree with. Either way it does not describe this disk's primary table. */
    if (get_le64(sec + HDR_MY_LBA) != 1u)
        return GPT_ERR_NOT_GPT;

    ent_lba = get_le64(sec + HDR_ENTRY_LBA);
    ent_count = get_le32(sec + HDR_ENTRY_COUNT);
    ent_size = get_le32(sec + HDR_ENTRY_SIZE);
    want = get_le32(sec + HDR_ENTRY_CRC32);

    if (ent_lba < 2u)
        return GPT_ERR_UNSUPPORTED;
    /* Requiring the entry size to divide the block size is what guarantees no
     * entry ever straddles a block boundary, which in turn is what lets the
     * decode loop below work one block at a time with no reassembly buffer.
     * The spec's 128 * 2^n sizes all satisfy it for every real drive; a
     * table that does not is declined rather than mis-parsed. */
    if (ent_size < ENT_SIZE_MIN || ent_size > block_size ||
        block_size % ent_size != 0u)
        return GPT_ERR_UNSUPPORTED;
    array_bytes = (uint64_t)ent_count * ent_size;
    if (array_bytes == 0u || array_bytes > GPT_ARRAY_MAX)
        return GPT_ERR_UNSUPPORTED;

    ent_base = ent_lba * block_size;

    /* Pass 1: checksum the array. Nothing in it is looked at until this
     * passes, which is the entire reason the array is read twice. */
    crc = CRC32_INIT;
    for (done = 0; done < array_bytes; done += block_size) {
        uint64_t left = array_bytes - done;
        uint32_t use = (left < block_size) ? (uint32_t)left : block_size;

        if (read(ctx, ent_base + done, block_size, sec) != 0)
            return GPT_ERR_IO;
        crc = crc32_update(crc, sec, use);
    }
    if ((crc ^ 0xFFFFFFFFu) != want)
        return GPT_ERR_CRC;

    /* Pass 2: decode. */
    per_block = block_size / ent_size;
    idx = 0;
    for (done = 0; done < array_bytes && idx < ent_count; done += block_size) {
        uint32_t j;

        if (read(ctx, ent_base + done, block_size, sec) != 0)
            return GPT_ERR_IO;

        for (j = 0; j < per_block && idx < ent_count; j++) {
            /* Widened before the multiply rather than after: the product is
             * bounded by block_size here, but an offset computation that can
             * only be checked by reasoning is the kind this tree does not
             * leave to the reader (or to clang-tidy). */
            const uint8_t *e = sec + ((size_t)j * ent_size);
            gpt_part *p;
            unsigned k;

            idx++;
            if (guid_is_zero(e + ENT_TYPE_GUID))
                continue;
            if (t->count >= t->cap) {
                t->truncated = 1;
                continue;
            }
            p = &t->parts[t->count++];
            p->number = idx;
            memcpy(p->type, e + ENT_TYPE_GUID, GPT_GUID_LEN);
            memcpy(p->unique, e + ENT_UNIQUE_GUID, GPT_GUID_LEN);
            p->start_lba = get_le64(e + ENT_START_LBA);
            p->end_lba = get_le64(e + ENT_END_LBA);
            /* The name is copied verbatim, terminator or not: identification
             * is p2's half of DESIGN Section 3, and deciding what the units
             * mean is gpt_name_eq's job, not the decoder's. Entirely inside
             * the entry, since ENT_SIZE_MIN is 128 and the field ends at
             * byte 127. */
            for (k = 0; k < GPT_NAME_LEN; k++)
                p->name[k] = get_le16(e + ENT_NAME + (size_t)k * 2u);
        }
    }

    return GPT_OK;
}

const gpt_part *gpt_find_start(const gpt_table *t, uint64_t start_lba)
{
    uint32_t i;

    if (t == NULL)
        return NULL;
    for (i = 0; i < t->count; i++) {
        if (t->parts[i].start_lba == start_lba)
            return &t->parts[i];
    }
    return NULL;
}

int gpt_has_role(const gpt_table *t, part_role role)
{
    uint32_t i;

    if (t == NULL)
        return 0;
    for (i = 0; i < t->count; i++) {
        if (gpt_part_is_role(&t->parts[i], role))
            return 1;
    }
    return 0;
}
