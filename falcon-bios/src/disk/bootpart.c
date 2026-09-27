/* falcon-bios: finding the `boot` partition. See bootpart.h. */

#include <disk/bootpart.h>

#include <part/part.h>
#include <lib/string.h>

/* The FAT BPB's reserved-sector count, in every FAT width. */
#define BPB_RESERVED 0x0Eu

int bootpart_candidate(const part_entry *p)
{
    if (p == NULL)
        return 0;
    if (p->mbr_type == 0u) {
        /* GPT, or an MBR entry with no type, which is no partition and
         * never reaches here from part_read_table. */
        return gpt_guid_eq(p->type_guid, gpt_guid_esp) ||
               gpt_guid_eq(p->type_guid, falcon_guid_boot);
    }
    switch (p->mbr_type) {
    case 0x01u: case 0x04u: case 0x06u:     /* FAT12, FAT16 <32M, FAT16 */
    case 0x0Bu: case 0x0Cu: case 0x0Eu:     /* FAT32, FAT32 LBA, FAT16 LBA */
    case 0xEFu:                             /* an ESP on an MBR disk */
    case 0x11u: case 0x14u: case 0x16u:     /* the same, hidden */
    case 0x1Bu: case 0x1Cu: case 0x1Eu:
        return 1;
    default:
        return 0;
    }
}

/* The record in one volume's boot sector, if it is a boot record. */
static int read_boot(gpt_read_fn read, void *ctx, uint32_t bs, uint64_t lba,
                     uint8_t *sec, ident_rec *rec)
{
    if (lba > UINT64_MAX / bs)
        return BOOTPART_ERR_ARG;
    if (read(ctx, lba * bs, bs, sec) != 0)
        return BOOTPART_ERR_IO;
    if (ident_decode_bin(sec, rec) != IDENT_OK || rec->role != IDENT_ROLE_BOOT)
        return BOOTPART_ERR_NONE;
    return BOOTPART_OK;
}

int bootpart_find(gpt_read_fn read, void *ctx, uint32_t block_size,
                  uint64_t disk_blocks, part_entry *parts, uint32_t cap,
                  uint8_t *sec, bootpart *out)
{
    part_table t;
    uint32_t i;
    int rc;

    if (read == NULL || parts == NULL || cap == 0u || sec == NULL ||
        out == NULL)
        return BOOTPART_ERR_ARG;

    t.parts = parts;
    t.cap = cap;
    rc = part_read_table(&t, read, ctx, block_size, disk_blocks);
    if (rc == PART_ERR_IO)
        return BOOTPART_ERR_IO;
    if (rc == PART_ERR_ARG)
        return BOOTPART_ERR_ARG;
    if (rc == PART_ERR_NO_TABLE) {
        /* A floppy or a superfloppy: the volume is the disk. */
        rc = read_boot(read, ctx, block_size, 0, sec, &out->rec);
        if (rc == BOOTPART_OK) {
            out->start_lba = 0;
            out->number = 0;
        }
        return rc;
    }
    if (rc != PART_OK)
        return BOOTPART_ERR_TABLE;

    for (i = 0; i < t.count; i++) {
        const part_entry *p = &t.parts[i];

        if (!bootpart_candidate(p))
            continue;
        rc = read_boot(read, ctx, block_size, p->start_lba, sec, &out->rec);
        if (rc == BOOTPART_ERR_IO)
            return rc;
        /* A boot record with no stage 3 is a UEFI station's ESP: on a disk
         * carrying both stations it comes before the station partition in
         * the table, and it is the station partition that holds stage 3. */
        if (rc != BOOTPART_OK || out->rec.stage3_sector == 0u)
            continue;
        out->start_lba = p->start_lba;
        out->number = p->number;
        return BOOTPART_OK;
    }
    return BOOTPART_ERR_NONE;
}

uint32_t bootpart_stage3_room(const uint8_t *bootsec, const ident_rec *rec)
{
    uint32_t reserved;

    if (bootsec == NULL || rec == NULL || rec->stage3_sector == 0u)
        return 0;
    reserved = (uint32_t)bootsec[BPB_RESERVED] |
               ((uint32_t)bootsec[BPB_RESERVED + 1u] << 8);
    if (rec->stage3_sector >= reserved)
        return 0;
    return reserved - rec->stage3_sector;
}
