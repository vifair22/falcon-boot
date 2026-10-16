/* falcon-boot mkstick: the pure half of `station install`. See
 * stationcore.h for why it is separate.
 */

#include <mkstick/stationcore.h>

#include <media/strows.h>
#include <pe/pe.h>

#include <stdio.h>
#include <string.h>

const uint8_t stn_guid_bios_boot[16] = {
    /* 21686148-6449-6E6F-744E-656564454649 */
    0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6f, 0x6e,
    0x74, 0x4e, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49
};

#define MBR_TABLE_OFF 446u
#define MBR_ENTRY_LEN 16u
#define MBR_SIG_OFF   440u
#define MBR_CODE_LEN  440u

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
    put_le32(p + 4, (uint32_t)(v >> 32));
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

const char *stn_strerror(stn_status s)
{
    switch (s) {
    case STN_OK:          return "ok";
    case STN_ERR_ARG:     return "bad argument";
    case STN_ERR_NOSLOT:  return "the MBR has no free primary slot";
    case STN_ERR_NOSPACE: return "no free space large enough";
    case STN_ERR_RANGE:   return "past what an MBR entry can hold";
    case STN_ERR_BACKUP:  return "not a station backup";
    case STN_ERR_FULL:    return "too many entries";
    case STN_ERR_INI:     return "STATION.INI did not format";
    }
    return "unknown";
}

/* ---- free space ---- */

static uint64_t round_up(uint64_t v, uint64_t to)
{
    return ((v + to - 1u) / to) * to;
}

uint64_t stn_first_allowed(const part_table *t, uint64_t floor)
{
    if (t != NULL && t->count > 0u && t->first_start != 0u)
        return t->first_start;
    return floor;
}

stn_status stn_find_free(const part_table *t, uint64_t lo, uint64_t hi,
                         uint64_t want, uint64_t align, stn_region *out,
                         uint64_t *largest)
{
    uint64_t starts[128];
    uint64_t ends[128];
    uint32_t n = 0;
    uint32_t i;
    uint64_t cur;
    uint64_t best_start = 0;
    int found = 0;

    if (t == NULL || out == NULL || largest == NULL || want == 0u ||
        align == 0u || lo > hi)
        return STN_ERR_ARG;
    *largest = 0;
    memset(out, 0, sizeof *out);

    /* The taken intervals, sorted by start. 128 is GPT's usual entry count
     * and far past any MBR's chain the table reader keeps. */
    for (i = 0; i < t->count; i++) {
        uint32_t k;

        if (n >= 128u)
            return STN_ERR_FULL;
        k = n++;
        while (k > 0u && starts[k - 1u] > t->parts[i].start_lba) {
            starts[k] = starts[k - 1u];
            ends[k] = ends[k - 1u];
            k--;
        }
        starts[k] = t->parts[i].start_lba;
        ends[k] = t->parts[i].end_lba;
    }

    /* Walk the gaps from lo upwards; the last one that fits is kept. */
    cur = lo;
    for (i = 0; i <= n; i++) {
        uint64_t gap_end = (i < n) ? starts[i] : hi + 1u;

        if (gap_end > hi + 1u)
            gap_end = hi + 1u;
        if (gap_end > cur) {
            uint64_t s = round_up(cur, align);

            if (s < gap_end) {
                uint64_t have = gap_end - s;

                if (have > *largest)
                    *largest = have;
                if (have >= want) {
                    best_start = s;
                    found = 1;
                }
            }
        }
        if (i < n && ends[i] + 1u > cur)
            cur = ends[i] + 1u;
        if (cur > hi)
            break;
    }
    if (!found)
        return STN_ERR_NOSPACE;
    out->start = best_start;
    out->sectors = want;
    return STN_OK;
}

/* ---- the MBR ---- */

unsigned stn_mbr_free_slot(const uint8_t *lba0)
{
    unsigned i;

    if (lba0 == NULL)
        return 0;
    for (i = 0; i < 4u; i++) {
        if (lba0[MBR_TABLE_OFF + (size_t)i * MBR_ENTRY_LEN + 4u] == 0u)
            return i + 1u;
    }
    return 0;
}

void stn_mbr_geometry(const uint8_t *lba0, unsigned *heads, unsigned *spt)
{
    unsigned i;

    *heads = 255u;
    *spt = 63u;
    if (lba0 == NULL)
        return;
    for (i = 0; i < 4u; i++) {
        const uint8_t *e = lba0 + MBR_TABLE_OFF + (size_t)i * MBR_ENTRY_LEN;
        unsigned h = e[5];
        unsigned s = e[6] & 0x3Fu;

        if (e[4] == 0u || s == 0u)
            continue;
        /* An end past cylinder 1023 is written as the largest CHS the
         * geometry has, so its head and sector are still the geometry's. */
        *heads = h + 1u;
        *spt = s;
        return;
    }
}

static void chs(uint8_t *p, uint64_t lba, unsigned heads, unsigned spt)
{
    uint64_t c = lba / ((uint64_t)heads * spt);
    unsigned h = (unsigned)((lba / spt) % heads);
    unsigned s = (unsigned)(lba % spt) + 1u;

    if (c > 1023u) {
        c = 1023u;
        h = heads - 1u;
        s = spt;
    }
    p[0] = (uint8_t)h;
    p[1] = (uint8_t)((s & 0x3Fu) | ((c >> 2) & 0xC0u));
    p[2] = (uint8_t)(c & 0xFFu);
}

stn_status stn_mbr_put(uint8_t *lba0, unsigned slot, uint8_t type,
                       uint64_t start, uint64_t sectors, unsigned heads,
                       unsigned spt)
{
    uint8_t *e;

    if (lba0 == NULL || slot < 1u || slot > 4u || type == 0u ||
        sectors == 0u || heads == 0u || heads > 256u || spt == 0u ||
        spt > 63u)
        return STN_ERR_ARG;
    if (start > 0xFFFFFFFFu || sectors > 0xFFFFFFFFu ||
        start + sectors - 1u > 0xFFFFFFFFu)
        return STN_ERR_RANGE;
    e = lba0 + MBR_TABLE_OFF + (size_t)(slot - 1u) * MBR_ENTRY_LEN;
    if (e[4] != 0u)
        return STN_ERR_NOSLOT;
    memset(e, 0, MBR_ENTRY_LEN);
    chs(e + 1, start, heads, spt);
    e[4] = type;
    chs(e + 5, start + sectors - 1u, heads, spt);
    put_le32(e + 8, (uint32_t)start);
    put_le32(e + 12, (uint32_t)sectors);
    return STN_OK;
}

uint32_t stn_mbr_sig(const uint8_t *lba0)
{
    return get_le32(lba0 + MBR_SIG_OFF);
}

void stn_mbr_set_sig(uint8_t *lba0, uint32_t sig)
{
    put_le32(lba0 + MBR_SIG_OFF, sig);
}

int stn_mbr_chainable(const uint8_t *lba0)
{
    return lba0 != NULL && vbr_mbr_bootable(lba0);
}

/* ---- boot sectors ---- */

int stn_is_windows_or_dos(vbr_loader l)
{
    return l == VBR_LOADER_BOOTMGR || l == VBR_LOADER_NTLDR ||
           l == VBR_LOADER_WIN9X || l == VBR_LOADER_MSDOS ||
           l == VBR_LOADER_FREEDOS;
}

/* ---- names ---- */

/* The id's hex digits as a name part: the signature, or the DiskGUID's
 * first eight, from the formatter the INI's own disk= values come from. */
static int id_hex8(const part_disk_id *id, char out[9])
{
    char text[PART_DISK_ID_TEXT];

    part_disk_id_format(id, text);
    if (strlen(text) < 12u)
        return -1;
    memcpy(out, text + 4, 8);
    out[8] = '\0';
    return 0;
}

int stn_name_disk(const part_disk_id *id, uint32_t part, char *buf,
                  size_t cap)
{
    char hex[9];
    int n;

    if (id == NULL || buf == NULL || id_hex8(id, hex) != 0)
        return -1;
    if (part > 0u)
        n = snprintf(buf, cap, "%s-%s-p%u",
                     id->kind == PART_KIND_GPT ? "gpt" : "mbr", hex,
                     (unsigned)part);
    else
        n = snprintf(buf, cap, "%s-%s",
                     id->kind == PART_KIND_GPT ? "gpt" : "mbr", hex);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static int name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

int stn_name_uki(const char *file, char *buf, size_t cap)
{
    size_t len;
    size_t i;
    int n;

    if (file == NULL || buf == NULL)
        return -1;
    len = strlen(file);
    if (len > 4u && (memcmp(file + len - 4u, ".efi", 4) == 0 ||
                     memcmp(file + len - 4u, ".EFI", 4) == 0))
        len -= 4u;
    if (len == 0u)
        return -1;
    for (i = 0; i < len; i++) {
        if (!name_char(file[i]))
            return -1;
    }
    n = snprintf(buf, cap, "uki-%.*s", (int)len, file);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

void stn_found_label(stn_found *f)
{
    const char *what;
    uint32_t part;

    if (f == NULL)
        return;
    switch (f->kind) {
    case STN_F_PART:
        what = vbr_loader_name(f->os);
        if (what == NULL)
            what = STROWS_LABEL_PARTITION;
        part = f->part;
        break;
    case STN_F_DISK:
        what = STROWS_LABEL_BOOTCODE;
        part = 0;
        break;
    case STN_F_EFIWIN:
        what = STROWS_LABEL_BOOTMGR;
        part = f->part;
        break;
    case STN_F_UKI:
    case STN_F_FWENTRY:
    default:
        return;
    }
    strows_label_part_ascii(f->label, sizeof f->label, what, &f->disk, part);
}

/* ---- the INI ---- */

static const char *family(const stn_found *f)
{
    if (f->kind == STN_F_DISK)
        return "disk";
    if (f->kind == STN_F_EFIWIN)
        return "windows";
    switch (f->os) {
    case VBR_LOADER_BOOTMGR:
    case VBR_LOADER_NTLDR:
    case VBR_LOADER_WIN9X:
        return "windows";
    case VBR_LOADER_MSDOS:
    case VBR_LOADER_FREEDOS:
        return "dos";
    case VBR_LOADER_GRUB:
        return "grub";
    case VBR_LOADER_SYSLINUX:
        return "syslinux";
    case VBR_LOADER_NONE:
    default:
        break;
    }
    return "other";
}

static int windows_like(const stn_found *f)
{
    if (f->kind == STN_F_EFIWIN)
        return 1;
    if (f->kind == STN_F_PART || f->kind == STN_F_FWENTRY)
        return stn_is_windows_or_dos(f->os);
    return 0;
}

stn_status stn_ini_build(const stn_found *f, size_t n, unsigned timeout,
                         stationini *out)
{
    const char *pick = NULL;
    size_t i;
    int pass;

    if (out == NULL || (f == NULL && n > 0u))
        return STN_ERR_ARG;
    memset(out, 0, sizeof *out);

    for (i = 0; i < n; i++) {
        stationini_entry *e;
        const char *fam;
        unsigned seen = 0;
        unsigned k;

        if (f[i].kind != STN_F_PART && f[i].kind != STN_F_DISK &&
            f[i].kind != STN_F_EFIWIN)
            continue;
        if (out->n_entries >= STATIONINI_ENTRIES_MAX)
            return STN_ERR_FULL;
        fam = family(&f[i]);
        for (k = 0; k < out->n_entries; k++) {
            if (strncmp(out->entries[k].name, fam, strlen(fam)) == 0 &&
                (out->entries[k].name[strlen(fam)] == '\0' ||
                 out->entries[k].name[strlen(fam)] == '-'))
                seen++;
        }
        e = &out->entries[out->n_entries++];
        if (seen == 0u)
            (void)snprintf(e->name, sizeof e->name, "%s", fam);
        else
            (void)snprintf(e->name, sizeof e->name, "%s-%u", fam, seen + 1u);
        e->has_disk = 1;
        e->disk = f[i].disk;
        e->part = (f[i].kind == STN_F_PART) ? f[i].part : 0u;
        if (f[i].kind == STN_F_EFIWIN)
            e->method = STATIONINI_METHOD_FIRMWARE;
        (void)snprintf(e->label, sizeof e->label, "%s", f[i].label);
    }

    /* The default: a Windows or DOS, else Linux, else anything else the
     * machine boots; the section's name when it has one, else the name the
     * loader gives what it finds. */
    for (pass = 0; pass < 3 && pick == NULL; pass++) {
        unsigned sect = 0;

        for (i = 0; i < n && pick == NULL; i++) {
            int has_section = (f[i].kind == STN_F_PART ||
                               f[i].kind == STN_F_DISK ||
                               f[i].kind == STN_F_EFIWIN);
            int want = (pass == 0) ? windows_like(&f[i])
                     : (pass == 1) ? (f[i].kind == STN_F_UKI)
                     : (f[i].kind == STN_F_PART);

            if (want && f[i].name[0] != '\0' && !has_section)
                pick = f[i].name;
            else if (want && has_section)
                pick = out->entries[sect].name;
            if (has_section)
                sect++;
        }
    }
    if (pick != NULL) {
        for (i = 0; i < n; i++) {
            stationini_entry *e;

            if (f[i].kind != STN_F_UKI || !f[i].locked ||
                strcmp(f[i].name, pick) != 0)
                continue;
            if (out->n_entries >= STATIONINI_ENTRIES_MAX)
                return STN_ERR_FULL;
            e = &out->entries[out->n_entries++];
            (void)snprintf(e->name, sizeof e->name, "linux");
            (void)snprintf(e->uki, sizeof e->uki, "%s", f[i].label);
            pick = e->name;
            break;
        }
        (void)snprintf(out->default_name, sizeof out->default_name, "%s",
                       pick);
        out->timeout = timeout;
    }
    return STN_OK;
}

/* ---- the backup ---- */

#define BK_OFF_VER     16u
#define BK_OFF_SECTOR  20u
#define BK_OFF_FLAGS   24u
#define BK_OFF_NDISKS  28u
#define BK_OFF_NRUNS   32u
#define BK_OFF_STPART  36u
#define BK_OFF_ESP     40u
#define BK_OFF_EF02    44u
#define BK_OFF_FW      48u
#define BK_OFF_ID      52u
#define BK_OFF_RUNS    128u
#define BK_RUN_LEN     24u
#define BK_OFF_DISKS   512u

stn_status stn_backup_encode(const stn_backup *b, uint8_t *hdr)
{
    uint32_t i;

    if (b == NULL || hdr == NULL || b->n_disks == 0u ||
        b->n_disks > STN_BK_MAX_DISKS || b->n_runs == 0u ||
        b->n_runs > STN_BK_MAX_RUNS || b->sector == 0u)
        return STN_ERR_ARG;
    memset(hdr, 0, STN_BK_HDR);
    memcpy(hdr, STN_BK_MAGIC, sizeof STN_BK_MAGIC - 1u);
    put_le32(hdr + BK_OFF_VER, STN_BK_VER);
    put_le32(hdr + BK_OFF_SECTOR, b->sector);
    put_le32(hdr + BK_OFF_FLAGS, b->flags);
    put_le32(hdr + BK_OFF_NDISKS, b->n_disks);
    put_le32(hdr + BK_OFF_NRUNS, b->n_runs);
    put_le32(hdr + BK_OFF_STPART, b->station_part);
    put_le32(hdr + BK_OFF_ESP, b->esp_part);
    put_le32(hdr + BK_OFF_EF02, b->ef02_part);
    put_le32(hdr + BK_OFF_FW, b->fw_entry);
    memcpy(hdr + BK_OFF_ID, b->install_id, 16);
    for (i = 0; i < b->n_runs; i++) {
        uint8_t *r = hdr + BK_OFF_RUNS + (size_t)i * BK_RUN_LEN;

        if (b->runs[i].disk >= b->n_disks || b->runs[i].sectors == 0u ||
            b->runs[i].sectors > STN_BK_RUN_MAX)
            return STN_ERR_ARG;
        put_le16(r, (uint16_t)b->runs[i].disk);
        put_le16(r + 2, (uint16_t)b->runs[i].kind);
        put_le32(r + 4, 0);
        put_le64(r + 8, b->runs[i].lba);
        put_le64(r + 16, b->runs[i].sectors);
    }
    for (i = 0; i < b->n_disks; i++) {
        size_t len = strlen(b->disks[i]);

        if (len == 0u || len >= STN_BK_PATH)
            return STN_ERR_ARG;
        memcpy(hdr + BK_OFF_DISKS + (size_t)i * STN_BK_PATH, b->disks[i], len);
    }
    return STN_OK;
}

stn_status stn_backup_decode(const uint8_t *hdr, size_t len, stn_backup *b)
{
    uint32_t i;

    if (hdr == NULL || b == NULL)
        return STN_ERR_ARG;
    memset(b, 0, sizeof *b);
    if (len < STN_BK_HDR ||
        memcmp(hdr, STN_BK_MAGIC, sizeof STN_BK_MAGIC - 1u) != 0 ||
        get_le32(hdr + BK_OFF_VER) != STN_BK_VER)
        return STN_ERR_BACKUP;
    b->sector = get_le32(hdr + BK_OFF_SECTOR);
    b->flags = get_le32(hdr + BK_OFF_FLAGS);
    b->n_disks = get_le32(hdr + BK_OFF_NDISKS);
    b->n_runs = get_le32(hdr + BK_OFF_NRUNS);
    b->station_part = get_le32(hdr + BK_OFF_STPART);
    b->esp_part = get_le32(hdr + BK_OFF_ESP);
    b->ef02_part = get_le32(hdr + BK_OFF_EF02);
    b->fw_entry = get_le32(hdr + BK_OFF_FW);
    memcpy(b->install_id, hdr + BK_OFF_ID, 16);
    if ((b->sector != 512u && b->sector != 4096u) || b->n_disks == 0u ||
        b->n_disks > STN_BK_MAX_DISKS || b->n_runs == 0u ||
        b->n_runs > STN_BK_MAX_RUNS)
        return STN_ERR_BACKUP;
    for (i = 0; i < b->n_runs; i++) {
        const uint8_t *r = hdr + BK_OFF_RUNS + (size_t)i * BK_RUN_LEN;

        b->runs[i].disk = (uint32_t)r[0] | ((uint32_t)r[1] << 8);
        b->runs[i].kind = (uint32_t)r[2] | ((uint32_t)r[3] << 8);
        b->runs[i].lba = get_le64(r + 8);
        b->runs[i].sectors = get_le64(r + 16);
        if (b->runs[i].disk >= b->n_disks || b->runs[i].sectors == 0u ||
            b->runs[i].sectors > STN_BK_RUN_MAX ||
            b->runs[i].kind < STN_RUN_LBA0 || b->runs[i].kind > STN_RUN_SIG)
            return STN_ERR_BACKUP;
    }
    for (i = 0; i < b->n_disks; i++) {
        const char *p = (const char *)hdr + BK_OFF_DISKS + (size_t)i * STN_BK_PATH;

        if (p[0] == '\0' || memchr(p, '\0', STN_BK_PATH) == NULL)
            return STN_ERR_BACKUP;
        memcpy(b->disks[i], p, STN_BK_PATH);
    }
    return STN_OK;
}

int stn_backup_disk(stn_backup *b, const char *path)
{
    uint32_t i;
    size_t len;

    if (b == NULL || path == NULL)
        return -1;
    len = strlen(path);
    if (len == 0u || len >= STN_BK_PATH)
        return -1;
    for (i = 0; i < b->n_disks; i++) {
        if (strcmp(b->disks[i], path) == 0)
            return (int)i;
    }
    if (b->n_disks >= STN_BK_MAX_DISKS)
        return -1;
    memcpy(b->disks[b->n_disks], path, len + 1u);
    return (int)b->n_disks++;
}

stn_status stn_backup_run(stn_backup *b, uint32_t disk, stn_run_kind kind,
                          uint64_t lba, uint64_t sectors)
{
    if (b == NULL || disk >= b->n_disks || sectors == 0u ||
        sectors > STN_BK_RUN_MAX)
        return STN_ERR_ARG;
    if (b->n_runs >= STN_BK_MAX_RUNS)
        return STN_ERR_FULL;
    b->runs[b->n_runs].disk = disk;
    b->runs[b->n_runs].kind = (uint32_t)kind;
    b->runs[b->n_runs].lba = lba;
    b->runs[b->n_runs].sectors = sectors;
    b->n_runs++;
    return STN_OK;
}

int stn_run_restores(const stn_run *r, int full)
{
    if (r == NULL)
        return 0;
    switch ((stn_run_kind)r->kind) {
    case STN_RUN_LBA0:
    case STN_RUN_GAP:
    case STN_RUN_ESP_BOOT:
        return 1;
    case STN_RUN_GPT:
    case STN_RUN_GPT_BACKUP:
    case STN_RUN_REGION:
    case STN_RUN_SIG:
        return full;
    }
    return 0;
}

int stn_run_whole_lba0(const stn_run *r, int full)
{
    return r != NULL && r->kind == STN_RUN_LBA0 && full;
}

unsigned stn_min_mib(uint32_t sector)
{
    return sector == 512u ? STN_MIN_MIB
         : sector == 4096u ? STN_MIN_MIB_4K : 0u;
}

unsigned stn_default_mib(uint32_t sector)
{
    return sector == 512u ? STN_DEFAULT_MIB
         : sector == 4096u ? STN_DEFAULT_MIB_4K : 0u;
}

uint64_t stn_align_for(uint64_t align512, uint32_t sector)
{
    uint64_t n;

    if (sector < 512u || sector % 512u != 0u)
        return 0;
    n = align512 / (sector / 512u);
    return n == 0u ? 1u : n;
}

stn_reach stn_reach_of(uint64_t end)
{
    if (end > STN_REACH_LBA28_BYTES)
        return STN_REACH_PAST_LBA28;
    if (end > STN_REACH_CHS_BYTES)
        return STN_REACH_PAST_CHS;
    return STN_REACH_OK;
}

const char *stn_noshim_loader(int has_new, int has_old)
{
    return (has_old && !has_new) ? STN_LOADER_OLD : STN_LOADER_NOSHIM;
}

int stn_is_esp_jump(const uint8_t *img, size_t n)
{
    static const char want[] = STN_JUMP_SBAT ",";
    const char *row;
    const char *end;
    pe_hdr h;
    size_t off = 0;
    size_t len = 0;

    if (img == NULL || pe_parse(img, n, &h) != PE_OK ||
        pe_find_section(&h, img, n, ".sbat", &off, &len) != PE_OK)
        return 0;
    /* The first row is shim's own "sbat,1,..."; the component is the
     * second. */
    row = (const char *)img + off;
    end = row + len;
    row = memchr(row, '\n', (size_t)(end - row));
    if (row == NULL)
        return 0;
    row++;
    return (size_t)(end - row) >= sizeof want - 1u &&
           memcmp(row, want, sizeof want - 1u) == 0;
}

stn_jump_action stn_jump_plan(int shim, int present, int ours)
{
    if (shim)
        return STN_JUMP_NONE;
    if (!present)
        return STN_JUMP_WRITE;
    return ours ? STN_JUMP_RESIGN : STN_JUMP_TAKE;
}

static int hex4(const char *p, uint16_t *out)
{
    unsigned v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        char ch = p[i];
        unsigned d;

        if (ch >= '0' && ch <= '9')
            d = (unsigned)(ch - '0');
        else if (ch >= 'A' && ch <= 'F')
            d = (unsigned)(ch - 'A') + 10u;
        else if (ch >= 'a' && ch <= 'f')
            d = (unsigned)(ch - 'a') + 10u;
        else
            return -1;
        v = v * 16u + d;
    }
    *out = (uint16_t)v;
    return 0;
}

/* The file an entry's device path names: File(...) where efibootmgr
 * prints the node by name, else what follows the last ")/" of a path that
 * prints it bare. */
static void fw_file(const char *dp, size_t len, char *out, size_t cap)
{
    const char *f = NULL;
    const char *end = dp + len;
    const char *p;
    size_t n;

    out[0] = '\0';
    for (p = dp; p + 5 <= end; p++) {
        if (memcmp(p, "File(", 5) == 0) {
            f = p + 5;
            break;
        }
    }
    if (f != NULL) {
        for (p = f; p < end && *p != ')'; p++)
            ;
    } else {
        for (p = end; p > dp + 1; p--) {
            if (p[-2] == ')' && p[-1] == '/')
                break;
        }
        if (p <= dp + 1 || *p != '\\')
            return;
        f = p;
        for (p = f; p < end && *p != ' ' && *p != '\t'; p++)
            ;
    }
    n = (size_t)(p - f);
    if (n >= cap)
        n = cap - 1u;
    memcpy(out, f, n);
    out[n] = '\0';
}

int stn_fw_parse(const char *text, const char *label, stn_fw *out)
{
    size_t lab = label != NULL ? strlen(label) : 0;
    const char *line = text;

    if (text == NULL || label == NULL || out == NULL)
        return -1;
    memset(out, 0, sizeof *out);
    while (*line != '\0') {
        const char *eol = strchr(line, '\n');
        size_t len = eol != NULL ? (size_t)(eol - line) : strlen(line);
        uint16_t num;

        if (len > 10u && memcmp(line, "BootOrder:", 10) == 0) {
            const char *p = line + 10;
            const char *e = line + len;

            while (p < e && out->n_order < STN_FW_MAX) {
                while (p < e && (*p == ' ' || *p == ','))
                    p++;
                if (e - p < 4 || hex4(p, &num) != 0)
                    break;
                out->order[out->n_order++] = num;
                p += 4;
            }
        } else if (len > 9u && memcmp(line, "Boot", 4) == 0 &&
                   hex4(line + 4, &num) == 0 &&
                   (line[8] == '*' || line[8] == ' ')) {
            const char *d = line + 9;
            const char *e = line + len;

            while (d < e && (*d == ' ' || *d == '*'))
                d++;
            if ((size_t)(e - d) >= lab && memcmp(d, label, lab) == 0 &&
                (d + lab == e || d[lab] == '\t' || d[lab] == ' ') &&
                out->n < STN_FW_MAX) {
                out->num[out->n] = num;
                fw_file(d + lab, (size_t)(e - d) - lab,
                        out->path[out->n], STN_FW_PATH_MAX);
                out->n++;
            }
        }
        line += len;
        if (*line == '\n')
            line++;
    }
    return 0;
}

int stn_fw_path_is(const char *path, const char *want)
{
    size_t i;

    if (path == NULL || want == NULL)
        return 0;
    for (i = 0; path[i] != '\0' && want[i] != '\0'; i++) {
        char a = path[i] == '/' ? '\\' : path[i];
        char b = want[i] == '/' ? '\\' : want[i];

        if (a >= 'a' && a <= 'z')
            a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z')
            b = (char)(b - 'a' + 'A');
        if (a != b)
            return 0;
    }
    return path[i] == '\0' && want[i] == '\0';
}

int stn_fw_find(const stn_fw *fw, const char *want)
{
    unsigned i;

    for (i = 0; fw != NULL && i < fw->n; i++) {
        if (stn_fw_path_is(fw->path[i], want))
            return (int)i;
    }
    return -1;
}

unsigned stn_fw_order_first(const stn_fw *fw, uint16_t num, uint16_t *out,
                            unsigned cap)
{
    unsigned n = 0;
    unsigned i;

    if (cap == 0u)
        return 0;
    out[n++] = num;
    for (i = 0; i < fw->n_order && n < cap; i++) {
        if (fw->order[i] != num)
            out[n++] = fw->order[i];
    }
    return n;
}
