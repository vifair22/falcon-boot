/* falcon-boot mkstick: where falcon-bios' stages go on a disk. See
 * bioscore.h for why this is separate from the code that writes them.
 */

#include <mkstick/bioscore.h>

#include <string.h>

#include <part/part.h>

#define MIB_SECTORS 2048u

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

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

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
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

static uint64_t sectors_of(size_t len)
{
    return ((uint64_t)len + BIOS_SECTOR_BYTES - 1u) / BIOS_SECTOR_BYTES;
}

static uint64_t sectors_in(size_t len, uint32_t bytes)
{
    return ((uint64_t)len + bytes - 1u) / bytes;
}

const char *bioscore_strerror(bioscore_status s)
{
    switch (s) {
    case BIOSCORE_OK:         return "ok";
    case BIOSCORE_ERR_ARG:    return "bad argument";
    case BIOSCORE_ERR_MBR:    return "LBA 0 does not end 0x55 0xAA";
    case BIOSCORE_ERR_STAGE1: return "stage 1 is not a 512-byte boot sector";
    case BIOSCORE_ERR_SPLICE: return "stage 1 reaches into the partition table";
    case BIOSCORE_ERR_STAGE2: return "stage 2 is larger than stage 1 may read";
    case BIOSCORE_ERR_GPT:    return "stage 2 would land on the GPT";
    case BIOSCORE_ERR_GAP:    return "stage 2 would reach the first partition";
    case BIOSCORE_ERR_BACKUP: return "not a falcon-bios backup";
    case BIOSCORE_ERR_SECTOR: return "the disk's sectors are neither 512 nor "
                                     "4096 bytes";
    case BIOSCORE_ERR_FAT:    return "the boot partition is not a FAT volume "
                                     "with the disk's sector size";
    case BIOSCORE_ERR_RECORD: return "the boot record's stage 3 or marker "
                                     "sector is one of FAT's own";
    case BIOSCORE_ERR_RESERVED:
        return "stage 3 does not fit the boot partition's reserved sectors";
    }
    return "unknown";
}

/* ---- FAT ---- */

bioscore_status bioscore_fat_bpb(const uint8_t *bs, bios_fat *out)
{
    uint16_t bytes;
    uint16_t fat16_size;

    if (bs == NULL || out == NULL)
        return BIOSCORE_ERR_ARG;
    memset(out, 0, sizeof *out);

    /* The jump every FAT boot sector opens with, and the 0x55AA it closes
     * with. Weak on their own; the BPB fields below do the rest. */
    if (!((bs[0] == 0xEBu && bs[2] == 0x90u) || bs[0] == 0xE9u))
        return BIOSCORE_ERR_FAT;
    if (bs[510] != 0x55u || bs[511] != 0xAAu)
        return BIOSCORE_ERR_FAT;
    bytes = get_le16(bs + 0x0B);
    if (bytes != 512u && bytes != 1024u && bytes != 2048u && bytes != 4096u)
        return BIOSCORE_ERR_FAT;
    out->sector = bytes;
    if (bs[0x0D] == 0u || bs[0x10] == 0u)   /* sectors/cluster, FAT count */
        return BIOSCORE_ERR_FAT;
    out->reserved = get_le16(bs + 0x0E);
    if (out->reserved == 0u)
        return BIOSCORE_ERR_FAT;

    /* FAT32 has no 16-bit FAT size; it is the one field that tells the
     * widths apart without counting clusters, and the only width with an
     * FSInfo sector and a backup boot sector to keep clear of. */
    fat16_size = get_le16(bs + 0x16);
    if (fat16_size == 0u) {
        out->fat32 = 1;
        out->fsinfo = get_le16(bs + 0x30);
        out->backup = get_le16(bs + 0x32);
        if (out->fsinfo == 0xFFFFu)
            out->fsinfo = 0;
        if (out->backup == 0xFFFFu)
            out->backup = 0;
    }
    return BIOSCORE_OK;
}

uint32_t bioscore_reserved_for(uint32_t stage3_sector, size_t stage3_len)
{
    uint64_t room = 2u * sectors_of(stage3_len);
    uint64_t total;

    room = ((room + MIB_SECTORS - 1u) / MIB_SECTORS) * MIB_SECTORS;
    if (room < MIB_SECTORS)
        room = MIB_SECTORS;
    total = (uint64_t)stage3_sector + room;
    if (stage3_sector == 0u || total > 0xFFFFu)
        return 0;
    return (uint32_t)total;
}

/* Is this reserved sector one FAT itself uses? The boot sector, and on
 * FAT32 the FSInfo sector and the backup copies of both. */
static int fat_owns(const bios_fat *f, uint64_t s)
{
    if (s == 0u)
        return 1;
    if (f->fat32) {
        if (f->fsinfo != 0u && s == f->fsinfo)
            return 1;
        if (f->backup != 0u && (s == f->backup || s == f->backup + 1u))
            return 1;
    }
    return 0;
}

bioscore_status bioscore_plan(const bios_disk *d, uint64_t stage2_gpt_lba,
                              uint64_t stage2_mbr_lba, uint64_t stage2_max,
                              size_t stage2_len, size_t stage3_len,
                              bios_plan *out)
{
    uint64_t s2_lba;
    uint64_t s2_n;
    uint64_t s3_n;
    uint64_t s;

    if (d == NULL || out == NULL || stage2_len == 0u || stage3_len == 0u ||
        stage2_max == 0u)
        return BIOSCORE_ERR_ARG;
    memset(out, 0, sizeof *out);

    /* Everything here counts the disk's sectors, because that is what
     * stage 1's packet, the record and a BIOS's AH=42h count: 512 bytes, or
     * 4096 on a 4Kn disk (DESIGN Section 3.4). */
    if (d->sector_bytes != BIOS_SECTOR_BYTES &&
        d->sector_bytes != BIOS_SECTOR_MAX)
        return BIOSCORE_ERR_SECTOR;

    /* ---- stage 2: after LBA 0, before the first partition ---- */
    /* stage2_max bounds the bytes stage 1 reads to STAGE2_LOAD, in 512-byte
     * units; whole disk sectors of stage 2 must fit it. */
    s2_n = sectors_in(stage2_len, d->sector_bytes);
    if (s2_n * d->sector_bytes > stage2_max * BIOS_SECTOR_BYTES ||
        s2_n > 0xFFFFu)
        return BIOSCORE_ERR_STAGE2;
    if (d->kind == PART_KIND_GPT) {
        s2_lba = stage2_gpt_lba;
        /* LBA 1 is the header and the entry array follows it; below the
         * header's own first usable LBA is still the table. */
        if (s2_lba < 2u || s2_lba < d->first_usable)
            return BIOSCORE_ERR_GPT;
    } else if (d->kind == PART_KIND_MBR) {
        s2_lba = stage2_mbr_lba;
        if (s2_lba < 1u)
            return BIOSCORE_ERR_ARG;
    } else {
        return BIOSCORE_ERR_ARG;
    }
    if (d->first_part_lba == 0u || s2_lba + s2_n > d->first_part_lba)
        return BIOSCORE_ERR_GAP;

    /* ---- stage 3 and the marker: the boot partition's reserved sectors ---- */
    if (d->fat.reserved == 0u || d->boot_start == 0u ||
        d->fat.reserved > d->boot_sectors ||
        d->fat.sector != d->sector_bytes)
        return BIOSCORE_ERR_FAT;
    if (d->stage3_sector == 0u || d->stage3_sector >= d->fat.reserved)
        return BIOSCORE_ERR_RECORD;
    if (d->marker_sector != 0u &&
        (d->marker_sector >= d->fat.reserved ||
         fat_owns(&d->fat, d->marker_sector)))
        return BIOSCORE_ERR_RECORD;

    s3_n = sectors_in(stage3_len, d->sector_bytes);
    if (s3_n > (uint64_t)d->fat.reserved - d->stage3_sector)
        return BIOSCORE_ERR_RESERVED;
    for (s = d->stage3_sector; s < d->stage3_sector + s3_n; s++) {
        if (fat_owns(&d->fat, s) || s == d->marker_sector)
            return BIOSCORE_ERR_RECORD;
    }

    out->stage2_lba = s2_lba;
    out->stage2_sectors = s2_n;
    out->stage3_lba = d->boot_start + d->stage3_sector;
    out->stage3_sectors = s3_n;
    out->marker_lba = (d->marker_sector != 0u)
                          ? d->boot_start + d->marker_sector : 0u;
    return BIOSCORE_OK;
}

/* ---- stage 1 ---- */

bioscore_status bioscore_check_stage1(const uint8_t *stage1, size_t len)
{
    size_t i;

    if (stage1 == NULL)
        return BIOSCORE_ERR_ARG;
    /* Exactly one sector, because that is what the BIOS loads. */
    if (len != BIOS_SECTOR_BYTES)
        return BIOSCORE_ERR_STAGE1;
    if (stage1[BIOS_MBR_SIG_OFF] != 0x55u ||
        stage1[BIOS_MBR_SIG_OFF + 1u] != 0xAAu)
        return BIOSCORE_ERR_STAGE1;

    /* The splice keeps bytes 440..511 of the DISK, so anything stage 1 put
     * there would be dropped on the way to the platter and the loader would
     * run with its tail missing. boot.S refuses to assemble past that budget;
     * a --stage1 file from elsewhere is checked here. */
    for (i = BIOS_MBR_CODE_LEN; i < BIOS_MBR_SIG_OFF; i++) {
        if (stage1[i] != 0u)
            return BIOSCORE_ERR_SPLICE;
    }
    return BIOSCORE_OK;
}

static int fields_fit(uint32_t lba_off, uint32_t sectors_off)
{
    /* Both inside the boot code, and apart from each other. */
    return lba_off + 4u <= BIOS_MBR_CODE_LEN &&
           sectors_off + 2u <= BIOS_MBR_CODE_LEN &&
           (lba_off + 4u <= sectors_off || sectors_off + 2u <= lba_off);
}

bioscore_status bioscore_splice_mbr(const uint8_t *cur, const uint8_t *stage1,
                                    uint32_t lba_off, uint32_t sectors_off,
                                    uint64_t stage2_lba,
                                    uint64_t stage2_sectors, uint8_t *out)
{
    if (cur == NULL || stage1 == NULL || out == NULL ||
        !fields_fit(lba_off, sectors_off) || stage2_lba == 0u ||
        stage2_lba > 0xFFFFFFFFu || stage2_sectors == 0u ||
        stage2_sectors > 0xFFFFu)
        return BIOSCORE_ERR_ARG;
    if (cur[BIOS_MBR_SIG_OFF] != 0x55u || cur[BIOS_MBR_SIG_OFF + 1u] != 0xAAu)
        return BIOSCORE_ERR_MBR;

    /* memmove rather than memcpy: out is allowed to be cur, which is what
     * every caller does. */
    if (out != cur)
        memmove(out, cur, BIOS_SECTOR_BYTES);
    memmove(out, stage1, BIOS_MBR_CODE_LEN);
    put_le32(out + lba_off, (uint32_t)stage2_lba);
    put_le16(out + sectors_off, (uint16_t)stage2_sectors);
    return BIOSCORE_OK;
}

int bioscore_stage1_fields(const uint8_t *mbr, uint32_t lba_off,
                           uint32_t sectors_off, uint64_t *lba,
                           uint64_t *sectors)
{
    if (mbr == NULL || lba == NULL || sectors == NULL ||
        !fields_fit(lba_off, sectors_off))
        return 0;
    *lba = get_le32(mbr + lba_off);
    *sectors = get_le16(mbr + sectors_off);
    return *lba != 0u && *sectors != 0u;
}

int bioscore_is_installed(const uint8_t *mbr, const uint8_t *stage1,
                          uint32_t lba_off, uint32_t sectors_off)
{
    uint32_t i;

    if (mbr == NULL || stage1 == NULL || !fields_fit(lba_off, sectors_off))
        return 0;
    for (i = 0; i < BIOS_MBR_CODE_LEN; i++) {
        if ((i >= lba_off && i < lba_off + 4u) ||
            (i >= sectors_off && i < sectors_off + 2u))
            continue;
        if (mbr[i] != stage1[i])
            return 0;
    }
    return 1;
}

int bioscore_is_reserve(const part_table *t, const part_entry *p)
{
    if (t == NULL || p == NULL || t->id.kind != PART_KIND_GPT)
        return 0;
    return gpt_name_eq(p->name, BIOS_BOOT_NAME);
}

/* ---- the backup ---- */

bioscore_status bioscore_backup_header(uint8_t *out, uint32_t sector_bytes,
                                       const bios_run *runs, size_t n)
{
    size_t i;

    if (out == NULL || runs == NULL || sector_bytes == 0u || n == 0u ||
        n > BIOS_BACKUP_MAX_RUNS)
        return BIOSCORE_ERR_ARG;
    for (i = 0; i < n; i++) {
        if (runs[i].sectors == 0u)
            return BIOSCORE_ERR_ARG;
    }
    memset(out, 0, BIOS_BACKUP_HDR);
    memcpy(out, BIOS_BACKUP_MAGIC, sizeof BIOS_BACKUP_MAGIC - 1u);
    put_le32(out + 16, BIOS_BACKUP_VER);
    put_le32(out + 20, sector_bytes);
    put_le32(out + 24, (uint32_t)n);
    for (i = 0; i < n; i++) {
        put_le64(out + 32 + i * 16u, runs[i].lba);
        put_le64(out + 40 + i * 16u, runs[i].sectors);
    }
    return BIOSCORE_OK;
}

bioscore_status bioscore_backup_parse(const uint8_t *hdr, size_t len,
                                      uint32_t *sector_bytes, bios_run *runs,
                                      size_t cap, size_t *n)
{
    uint32_t count;
    size_t i;

    if (hdr == NULL || sector_bytes == NULL || runs == NULL || n == NULL)
        return BIOSCORE_ERR_ARG;
    if (len < BIOS_BACKUP_HDR)
        return BIOSCORE_ERR_BACKUP;
    if (memcmp(hdr, BIOS_BACKUP_MAGIC, sizeof BIOS_BACKUP_MAGIC - 1u) != 0)
        return BIOSCORE_ERR_BACKUP;
    if (get_le32(hdr + 16) != BIOS_BACKUP_VER)
        return BIOSCORE_ERR_BACKUP;

    *sector_bytes = get_le32(hdr + 20);
    count = get_le32(hdr + 24);
    if (*sector_bytes == 0u || count == 0u || count > BIOS_BACKUP_MAX_RUNS ||
        count > cap)
        return BIOSCORE_ERR_BACKUP;
    for (i = 0; i < count; i++) {
        runs[i].lba = get_le64(hdr + 32 + i * 16u);
        runs[i].sectors = get_le64(hdr + 40 + i * 16u);
        /* A run longer than a stage could ever be is a damaged header, and
         * the restore would otherwise allocate whatever it says. */
        if (runs[i].sectors == 0u || runs[i].sectors > 0x10000u)
            return BIOSCORE_ERR_BACKUP;
    }
    *n = count;
    return BIOSCORE_OK;
}

uint64_t bioscore_backup_size(uint32_t sector_bytes, const bios_run *runs,
                              size_t n)
{
    uint64_t total = BIOS_BACKUP_HDR;
    size_t i;

    if (runs == NULL)
        return total;
    for (i = 0; i < n; i++)
        total += runs[i].sectors * sector_bytes;
    return total;
}
