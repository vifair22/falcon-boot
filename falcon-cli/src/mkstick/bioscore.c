/* falcon-boot mkstick: where falcon-bios' stages go on a GPT disk. See
 * bioscore.h for why this is separate from the code that writes them.
 */

#include <mkstick/bioscore.h>

#include <string.h>

/* The GPT's own floor. LBA 0 is the protective MBR, LBA 1 the header, and
 * LBA 2..33 the standard 128-entry array; 34 is the first sector a normal
 * disk declares usable. A disk whose header says something larger is
 * believed (bioscore_check_gap reads the field rather than this constant);
 * this is only the value below which no GPT disk can ever be right. */
#define GPT_FLOOR_LBA 34u

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    put_le32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

const char *bioscore_strerror(bioscore_status s)
{
    switch (s) {
    case BIOSCORE_OK:         return "ok";
    case BIOSCORE_ERR_ARG:    return "bad argument";
    case BIOSCORE_ERR_MBR:    return "LBA 0 does not end 0x55 0xAA";
    case BIOSCORE_ERR_STAGE1: return "stage 1 is not a 512-byte boot sector";
    case BIOSCORE_ERR_SPLICE: return "stage 1 reaches into the partition table";
    case BIOSCORE_ERR_STAGE2: return "stage 2 is past the sectors stage 1 reads";
    case BIOSCORE_ERR_STAGE3: return "stage 3 is past the sectors stage 2 reads";
    case BIOSCORE_ERR_GPT:    return "the stages would land on the GPT";
    case BIOSCORE_ERR_GAP:    return "the stages would land on a partition";
    case BIOSCORE_ERR_BACKUP: return "not a falcon-bios backup";
    }
    return "unknown";
}

bioscore_status bioscore_plan(uint64_t stage2_lba, uint64_t stage2_sectors,
                              uint64_t stage3_sectors, bios_plan *out)
{
    if (out == NULL || stage2_sectors == 0u || stage3_sectors == 0u)
        return BIOSCORE_ERR_ARG;

    /* Stage 1 is LBA 0 and the GPT owns 1..33. A loader built to read stage 2
     * from anywhere in that range would work on the rig's raw image and eat
     * the partition table of the first real stick it met, so it is refused
     * here rather than at the point of the write. */
    if (stage2_lba < GPT_FLOOR_LBA)
        return BIOSCORE_ERR_GPT;

    /* Nothing here can overflow on a real layout; the check is for the
     * layout a typo in biosdefs.h would produce. */
    if (stage2_sectors > UINT64_MAX - stage2_lba)
        return BIOSCORE_ERR_ARG;
    out->stage2_lba = stage2_lba;
    out->stage2_sectors = stage2_sectors;
    out->stage3_lba = stage2_lba + stage2_sectors;
    out->stage3_sectors = stage3_sectors;
    if (stage3_sectors > UINT64_MAX - out->stage3_lba)
        return BIOSCORE_ERR_ARG;
    out->end_lba = out->stage3_lba + stage3_sectors;
    return BIOSCORE_OK;
}

bioscore_status bioscore_check_images(const uint8_t *stage1, size_t stage1_len,
                                      size_t stage2_len, size_t stage3_len,
                                      const bios_plan *p)
{
    if (stage1 == NULL || p == NULL)
        return BIOSCORE_ERR_ARG;

    /* Exactly one sector, because that is what the BIOS loads. A longer
     * image is a build that grew past its own assertion, and a shorter one
     * has no signature to find. */
    if (stage1_len != BIOS_SECTOR_BYTES)
        return BIOSCORE_ERR_STAGE1;
    if (stage1[BIOS_MBR_SIG_OFF] != 0x55u || stage1[BIOS_MBR_SIG_OFF + 1u] != 0xAAu)
        return BIOSCORE_ERR_STAGE1;

    /* The splice keeps bytes 440..511 of the DISK, so anything stage 1 put
     * there would be dropped on the way to the platter and the loader would
     * run with its tail missing. 286 bytes today against a 440-byte budget,
     * so this is headroom rather than a constraint; it is checked because a
     * stage 1 that quietly outgrew it is a boot failure with no message. */
    {
        size_t i;

        for (i = BIOS_MBR_CODE_LEN; i < BIOS_MBR_SIG_OFF; i++) {
            if (stage1[i] != 0u)
                return BIOSCORE_ERR_SPLICE;
        }
    }

    if (stage2_len == 0u ||
        stage2_len > p->stage2_sectors * BIOS_SECTOR_BYTES)
        return BIOSCORE_ERR_STAGE2;
    if (stage3_len == 0u ||
        stage3_len > p->stage3_sectors * BIOS_SECTOR_BYTES)
        return BIOSCORE_ERR_STAGE3;
    return BIOSCORE_OK;
}

bioscore_status bioscore_check_gap(const bios_plan *p, uint64_t first_usable,
                                   uint64_t first_part_lba)
{
    if (p == NULL)
        return BIOSCORE_ERR_ARG;

    /* Below the header's own floor is the entry array. Believe the header
     * rather than the usual 34: a disk made with a larger array, or with 4 KiB
     * sectors, puts first usable somewhere else entirely. */
    if (first_usable < GPT_FLOOR_LBA)
        return BIOSCORE_ERR_GPT;
    if (p->stage2_lba < first_usable)
        return BIOSCORE_ERR_GPT;

    /* end_lba is one past the last sector written, so equality is a fit:
     * a plan ending at 2048 and a partition starting at 2048 do not touch.
     * first_part_lba of 0 means a table with no partitions in it, where
     * there is nothing below to protect. */
    if (first_part_lba != 0u && p->end_lba > first_part_lba)
        return BIOSCORE_ERR_GAP;
    return BIOSCORE_OK;
}

bioscore_status bioscore_splice_mbr(const uint8_t *cur, const uint8_t *stage1,
                                    uint8_t *out)
{
    if (cur == NULL || stage1 == NULL || out == NULL)
        return BIOSCORE_ERR_ARG;
    if (cur[BIOS_MBR_SIG_OFF] != 0x55u || cur[BIOS_MBR_SIG_OFF + 1u] != 0xAAu)
        return BIOSCORE_ERR_MBR;

    /* memmove rather than memcpy: out is allowed to be cur, which is what
     * every caller does - it read the sector to check it, and now wants the
     * same buffer written back. */
    if (out != cur)
        memmove(out, cur, BIOS_SECTOR_BYTES);
    memmove(out, stage1, BIOS_MBR_CODE_LEN);
    return BIOSCORE_OK;
}

int bioscore_is_installed(const uint8_t *mbr, const uint8_t *stage1)
{
    if (mbr == NULL || stage1 == NULL)
        return 0;
    return memcmp(mbr, stage1, BIOS_MBR_CODE_LEN) == 0;
}

int bioscore_is_reserve(const gpt_part *p)
{
    if (p == NULL)
        return 0;
    return p->number == BIOS_BOOT_SLOT && gpt_name_eq(p->name, BIOS_BOOT_NAME);
}

bioscore_status bioscore_backup_header(uint8_t *out, uint32_t sector_bytes,
                                       uint64_t gap_lba, uint64_t gap_sectors)
{
    if (out == NULL || sector_bytes == 0u || gap_sectors == 0u)
        return BIOSCORE_ERR_ARG;
    memset(out, 0, BIOS_BACKUP_HDR);
    memcpy(out, BIOS_BACKUP_MAGIC, sizeof BIOS_BACKUP_MAGIC - 1u);
    put_le32(out + 16, BIOS_BACKUP_VER);
    put_le32(out + 20, sector_bytes);
    put_le64(out + 24, gap_lba);
    put_le64(out + 32, gap_sectors);
    return BIOSCORE_OK;
}

bioscore_status bioscore_backup_parse(const uint8_t *hdr, size_t len,
                                      uint32_t *sector_bytes,
                                      uint64_t *gap_lba, uint64_t *gap_sectors)
{
    if (hdr == NULL || sector_bytes == NULL || gap_lba == NULL ||
        gap_sectors == NULL)
        return BIOSCORE_ERR_ARG;
    if (len < BIOS_BACKUP_HDR)
        return BIOSCORE_ERR_BACKUP;
    if (memcmp(hdr, BIOS_BACKUP_MAGIC, sizeof BIOS_BACKUP_MAGIC - 1u) != 0)
        return BIOSCORE_ERR_BACKUP;
    if (get_le32(hdr + 16) != BIOS_BACKUP_VER)
        return BIOSCORE_ERR_BACKUP;

    *sector_bytes = get_le32(hdr + 20);
    *gap_lba = get_le64(hdr + 24);
    *gap_sectors = get_le64(hdr + 32);
    if (*sector_bytes == 0u || *gap_sectors == 0u)
        return BIOSCORE_ERR_BACKUP;
    return BIOSCORE_OK;
}

uint64_t bioscore_backup_size(uint32_t sector_bytes, uint64_t gap_sectors)
{
    /* The header, then LBA 0, then the gap. LBA 0 is stored whole even though
     * only 440 bytes of it are ever replaced, because a restore that puts
     * back the entire sector is one a human can also do with dd. */
    return (uint64_t)BIOS_BACKUP_HDR + sector_bytes +
           sector_bytes * gap_sectors;
}
