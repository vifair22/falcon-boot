/* falcon-boot mkstick: partitions and their identity records. See records.h.
 */

#include <mkstick/records.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include <common/log.h>
#include <mkstick/bioscore.h>
#include <mkstick/disk.h>

#include <fs/fs.h>
#include <luks/luks.h>
#include <part/part.h>

static const uint8_t luks2_magic[6] = { 'L', 'U', 'K', 'S', 0xba, 0xbe };

/* The MBR type bytes a FAT volume wears, and their hidden forms (the
 * contract's list: docs/notes/station-rework.md section 9). */
static const uint8_t fat_types[] = {
    0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E, 0xEF,
    0x11, 0x14, 0x16, 0x1B, 0x1C, 0x1E
};

int records_boot_candidate(const part_table *t, const part_entry *p)
{
    size_t i;

    if (t == NULL || p == NULL)
        return 0;
    if (t->id.kind == PART_KIND_GPT)
        return gpt_guid_eq(p->type_guid, gpt_guid_esp) ||
               gpt_guid_eq(p->type_guid, falcon_guid_boot);
    for (i = 0; i < sizeof fat_types; i++) {
        if (p->mbr_type == fat_types[i])
            return 1;
    }
    return 0;
}

/* The LUKS2 token. The header is untrusted and so is the token: a token
 * whose fields do not make a record is REC_BAD, not a crash. */
static void read_luks(mks_rec *out, disk_io *io)
{
    static char json[LUKS_JSON_CAP];
    luks_vol vol;
    uint32_t version = 0;
    const char *role = NULL;
    const char *id = NULL;
    size_t role_len = 0;
    size_t id_len = 0;

    out->luks = 1;
    out->where = "LUKS2 token";
    if (luks_open(&vol, disk_io_read, io, json, sizeof json) != LUKS_OK) {
        out->state = REC_BAD;
        return;
    }
    switch (luks_falcon_token(&vol, &version, &role, &role_len, &id,
                              &id_len)) {
    case LUKS_OK:
        break;
    case LUKS_ERR_NOTFOUND:
        out->state = REC_NONE;
        return;
    default:
        out->state = REC_BAD;
        return;
    }
    out->state = (ident_from_strings(version, role, role_len, id, id_len,
                                     &out->rec) == IDENT_OK)
                     ? REC_OK : REC_BAD;
}

static void read_falcon_file(mks_rec *out, disk_io *io)
{
    static fs_vol vol;
    static fs_dir scratch;
    static fs_file file;
    fs_dirent ent;
    const fs_ops *ops;
    char text[IDENT_TEXT_MAX];
    uint64_t size;

    ops = fs_identify(disk_io_read, io, FS_HINT_NONE);
    if (ops == NULL)
        return;
    out->fs = ops->name;
    if (fs_mount(&vol, ops, disk_io_read, io) != FS_OK)
        return;
    if (fs_lookup(&vol, IDENT_FILE, &scratch, &ent) != FS_OK)
        return;
    out->where = IDENT_FILE;
    size = ent.size;
    if (ent.is_dir || size == 0u || size >= sizeof text ||
        fs_file_open(&vol, &file, &ent) != FS_OK ||
        fs_file_read(&file, 0, size, text) != FS_OK) {
        out->state = REC_BAD;
        return;
    }
    out->state = (ident_decode_text(text, (size_t)size, &out->rec) ==
                  IDENT_OK) ? REC_OK : REC_BAD;
}

static void read_one(mks_rec *out, const char *path, const part_entry *p,
                     uint32_t sector)
{
    uint8_t first[BIOS_SECTOR_BYTES];
    disk_io io;
    int rc;

    memset(out, 0, sizeof *out);
    if (disk_io_open(&io, path, p->start_lba * sector,
                     (p->end_lba - p->start_lba + 1u) * sector) != CLI_OK) {
        cli_error_clear();
        return;
    }
    if (disk_io_read(&io, 0, sizeof first, first) != 0) {
        disk_io_close(&io);
        return;
    }

    if (memcmp(first, luks2_magic, sizeof luks2_magic) == 0) {
        read_luks(out, &io);
        disk_io_close(&io);
        return;
    }

    rc = ident_decode_bin(first, &out->rec);
    if (rc != IDENT_ERR_NONE) {
        out->where = "boot sector";
        out->state = (rc == IDENT_OK) ? REC_OK : REC_BAD;
        disk_io_close(&io);
        return;
    }

    read_falcon_file(out, &io);
    disk_io_close(&io);
}

cli_status records_read(mks_records *r, const char *path, uint32_t sector,
                        uint64_t size)
{
    disk_io io;
    uint32_t i;
    cli_status rc;

    memset(r, 0, sizeof *r);
    r->table.parts = r->parts;
    r->table.cap = MKS_TABLE_MAX;

    rc = disk_io_open(&io, path, 0, size);
    if (rc != CLI_OK)
        return rc;
    r->rc = part_read_table(&r->table, disk_io_read, &io, sector,
                            size / sector);
    disk_io_close(&io);
    if (r->rc != PART_OK) {
        r->table.count = 0;
        return CLI_OK;
    }
    for (i = 0; i < r->table.count; i++) {
        /* An MBR extended partition is a container of logical ones, not a
         * volume; its first sector is the chain's first EBR. */
        if (r->table.id.kind == PART_KIND_MBR &&
            part_mbr_is_extended(r->parts[i].mbr_type))
            continue;
        read_one(&r->recs[i], path, &r->parts[i], sector);
    }
    return CLI_OK;
}

int records_find(const mks_records *r, ident_role role, int from)
{
    uint32_t i;

    if (from < 0)
        from = 0;
    for (i = (uint32_t)from; i < r->table.count; i++) {
        if (r->recs[i].state == REC_OK && r->recs[i].rec.role == role)
            return (int)i;
    }
    return -1;
}

int records_boot(const mks_records *r)
{
    uint32_t i;

    for (i = 0; i < r->table.count; i++) {
        if (records_boot_candidate(&r->table, &r->parts[i]) &&
            r->recs[i].state == REC_OK &&
            r->recs[i].rec.role == IDENT_ROLE_BOOT)
            return (int)i;
    }
    return -1;
}

static cli_status write_at(int fd, const char *path, uint64_t off,
                           const ident_rec *rec)
{
    uint8_t sec[BIOS_SECTOR_BYTES];
    bios_fat fat;

    if (pread(fd, sec, sizeof sec, (off_t)off) != (ssize_t)sizeof sec)
        return set_error_errno(CLI_ERR_IO, "reading %s at %llu", path,
                               (unsigned long long)off);
    if (bioscore_fat_bpb(sec, &fat) != BIOSCORE_OK)
        return set_error(CLI_ERR_INVALID,
                         "%s at byte %llu is not a FAT boot sector; the boot "
                         "record goes in one", path, (unsigned long long)off);
    ident_encode_bin(rec, sec);
    if (pwrite(fd, sec, sizeof sec, (off_t)off) != (ssize_t)sizeof sec)
        return set_error_errno(CLI_ERR_IO, "writing %s at %llu", path,
                               (unsigned long long)off);
    return CLI_OK;
}

cli_status records_write_boot(const char *path, uint64_t base,
                              const ident_rec *rec)
{
    uint8_t sec[BIOS_SECTOR_BYTES];
    bios_fat fat;
    cli_status rc;
    int fd;

    if (rec == NULL || rec->role != IDENT_ROLE_BOOT)
        return set_error(CLI_ERR_INTERNAL, "not a boot record");
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s for writing", path);
    if (pread(fd, sec, sizeof sec, (off_t)base) != (ssize_t)sizeof sec) {
        (void)close(fd);
        return set_error_errno(CLI_ERR_IO, "reading %s", path);
    }
    if (bioscore_fat_bpb(sec, &fat) != BIOSCORE_OK) {
        (void)close(fd);
        return set_error(CLI_ERR_INVALID,
                         "%s at byte %llu is not a FAT volume", path,
                         (unsigned long long)base);
    }
    /* The record's stage 3 and marker sectors are inside this volume's
     * reserved area, or the record is lying about where they are. */
    if (rec->stage3_sector >= fat.reserved ||
        rec->marker_sector >= fat.reserved) {
        (void)close(fd);
        return set_error(CLI_ERR_RANGE,
                         "the FAT at byte %llu of %s has %u reserved "
                         "sectors; the boot record needs stage 3 at %u and "
                         "the marker at %u inside them (format it with "
                         "mkfs.fat -R)", (unsigned long long)base, path,
                         (unsigned)fat.reserved, (unsigned)rec->stage3_sector,
                         (unsigned)rec->marker_sector);
    }

    /* The record is in the first 512 bytes of the boot sector whatever the
     * FAT's sector size, but the backup is a number of the FAT's sectors. */
    rc = write_at(fd, path, base, rec);
    if (rc == CLI_OK && fat.fat32 && fat.backup != 0u)
        rc = write_at(fd, path, base + (uint64_t)fat.backup * fat.sector,
                      rec);
    if (rc == CLI_OK && fsync(fd) != 0)
        rc = set_error_errno(CLI_ERR_IO, "flushing %s", path);
    (void)close(fd);
    return rc;
}

/* A whole word as an unsigned number no larger than max: decimal, or hex
 * after 0x. Nothing may follow the digits, and no sign may lead them. */
static int number(const char *s, uint64_t max, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (s == NULL || *s < '0' || *s > '9')
        return 0;
    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v > max)
        return 0;
    *out = (uint64_t)v;
    return 1;
}

int records_boot_args(const char *offset, const char *stage3,
                      const char *marker, uint64_t *off, ident_rec *rec,
                      const char **why)
{
    uint64_t s3 = 0;
    uint64_t mk = 0;

    if (off == NULL || rec == NULL || why == NULL)
        return 0;
    if (!number(offset, UINT64_MAX, off) || *off % 512u != 0u) {
        *why = "the byte offset must be a number on a 512-byte boundary";
        return 0;
    }
    if (!number(stage3, 0xFFFFu, &s3) || s3 == 0u) {
        *why = "the stage 3 sector must be a number from 1 to 65535";
        return 0;
    }
    if (!number(marker, 0xFFFFu, &mk)) {
        *why = "the marker sector must be a number below 65536 (0 for none)";
        return 0;
    }
    memset(rec, 0, sizeof *rec);
    rec->version = IDENT_VERSION;
    rec->role = IDENT_ROLE_BOOT;
    rec->stage3_sector = (uint16_t)s3;
    rec->marker_sector = (uint16_t)mk;
    return 1;
}

cli_status records_new_id(uint8_t id[IDENT_ID_LEN])
{
    size_t done = 0;

    while (done < IDENT_ID_LEN) {
        ssize_t n = getrandom(id + done, IDENT_ID_LEN - done, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return set_error_errno(CLI_ERR_IO, "getrandom");
        }
        done += (size_t)n;
    }
    return CLI_OK;
}

void records_id_hex(const uint8_t id[IDENT_ID_LEN], char out[33])
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < IDENT_ID_LEN; i++) {
        out[i * 2u] = hex[id[i] >> 4];
        out[i * 2u + 1u] = hex[id[i] & 0x0Fu];
    }
    out[(size_t)IDENT_ID_LEN * 2u] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int records_id_parse(const char *hex, uint8_t id[IDENT_ID_LEN])
{
    size_t i;

    if (hex == NULL || strlen(hex) != (size_t)IDENT_ID_LEN * 2u)
        return 0;
    for (i = 0; i < IDENT_ID_LEN; i++) {
        int hi = hexval(hex[i * 2u]);
        int lo = hexval(hex[i * 2u + 1u]);

        if (hi < 0 || lo < 0)
            return 0;
        id[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}
