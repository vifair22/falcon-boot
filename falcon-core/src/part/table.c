/* A disk's partition table, MBR or GPT, as one list. See table.h for the
 * rules and DESIGN Section 3.2 for why they are these.
 *
 * MBR layout, as every BIOS since the PC/AT and Linux's msdos.c read it:
 *     440   disk signature, LE32 (Windows NT's; 0 on a DOS or 9x disk)
 *     446   four 16-byte entries: status, CHS start (3), type, CHS end (3),
 *           start LBA (LE32), sector count (LE32)
 *     510   0x55 0xAA
 * An extended partition's first sector (an EBR) has the same layout: its
 * first data entry is a logical partition relative to that EBR, and its
 * first extended-typed entry links to the next EBR relative to the
 * extended partition's own start. The CHS fields are never read: an LBA
 * disk's CHS values are placeholders, and LBA is what both loaders use.
 *
 * The 4096-byte-sector case needs nothing special: every field is in the
 * first 512 bytes of the block, and every LBA is in the disk's own blocks.
 */

#include <part/table.h>
#include <lib/string.h>

#define MBR_DISKSIG_OFF 440u
#define MBR_TABLE_OFF   446u
#define MBR_ENT_LEN      16u
#define MBR_SIG_OFF     510u

#define ENT_STATUS 0u
#define ENT_TYPE   4u
#define ENT_START  8u
#define ENT_COUNT 12u

#define MBR_TYPE_GPT 0xEEu

typedef struct {
    uint8_t  status;
    uint8_t  type;
    uint32_t start;
    uint32_t count;
} mbr_ent;

typedef struct {
    mbr_ent  e[4];
    uint32_t disk_sig;
} mbr_sec;

int part_mbr_is_extended(uint8_t type)
{
    /* CHS extended, LBA extended (Windows 95's), and Linux's own. */
    return type == 0x05u || type == 0x0Fu || type == 0x85u;
}

/* An MBR or EBR sector, its four entries decoded. `strict` is LBA 0's rule:
 * a 0x55AA sector whose status bytes are not all 0x00 or 0x80 is not a
 * partition table but something else with the signature, most often a FAT
 * boot sector's code, and (by design) a falcon identity record, whose
 * magic's third byte lands in entry 3's status. An EBR is only asked for its
 * signature, as Linux asks.
 *
 * Its own function so the block buffer's frame is gone before the caller
 * goes on to the GPT reader, which has a block buffer of its own: stage 2
 * will link this, and two 4 KiB frames at once is one too many there. */
static int read_mbr(gpt_read_fn read, void *ctx, uint32_t bs, uint64_t lba,
                    int strict, mbr_sec *m)
{
    uint8_t sec[GPT_MAX_SECTOR];
    unsigned i;

    if (read(ctx, lba * bs, bs, sec) != 0)
        return PART_ERR_IO;
    if (sec[MBR_SIG_OFF] != 0x55u || sec[MBR_SIG_OFF + 1u] != 0xAAu)
        return PART_ERR_NO_TABLE;
    for (i = 0; i < 4u; i++) {
        const uint8_t *e = sec + MBR_TABLE_OFF + (size_t)i * MBR_ENT_LEN;

        m->e[i].status = e[ENT_STATUS];
        m->e[i].type = e[ENT_TYPE];
        m->e[i].start = get_le32(e + ENT_START);
        m->e[i].count = get_le32(e + ENT_COUNT);
        if (strict && m->e[i].status != 0x00u && m->e[i].status != 0x80u)
            return PART_ERR_NO_TABLE;
    }
    m->disk_sig = get_le32(sec + MBR_DISKSIG_OFF);
    return PART_OK;
}

/* The next slot of the caller's table, zeroed, or NULL (and `truncated`)
 * when it is full. */
static part_entry *next_slot(part_table *t)
{
    part_entry *p;

    if (t->count >= t->cap) {
        t->truncated = 1;
        return NULL;
    }
    p = &t->parts[t->count++];
    memset(p, 0, sizeof(*p));
    return p;
}

/* ---- GPT ---- */

static void add_gpt(void *cb_ctx, const gpt_part *g)
{
    part_table *t = cb_ctx;
    part_entry *p = next_slot(t);

    if (p == NULL)
        return;
    p->start_lba = g->start_lba;
    p->end_lba = g->end_lba;
    p->number = g->number;
    p->primary = 1;
    memcpy(p->type_guid, g->type, GPT_GUID_LEN);
    memcpy(p->unique, g->unique, GPT_GUID_LEN);
    memcpy(p->name, g->name, sizeof(p->name));
}

static int read_gpt(part_table *t, gpt_read_fn read, void *ctx, uint32_t bs,
                    uint64_t disk_blocks)
{
    gpt_header h;
    int rc, rc2;

    rc = gpt_read_at(read, ctx, bs, 1, &h, add_gpt, t);
    if (rc == GPT_ERR_IO)
        return PART_ERR_IO;
    if (rc != GPT_OK) {
        /* The primary failed a check, so its entries were never decoded;
         * the backup copy at the disk's last block is the table now. With
         * the disk's length unknown there is no backup to find. A read
         * error is not a failed check and is reported as itself rather
         * than papered over with the other copy. */
        if (disk_blocks < 3u)
            return rc == GPT_ERR_UNSUPPORTED ? PART_ERR_UNSUPPORTED
                                             : PART_ERR_CORRUPT;
        t->count = 0;
        t->truncated = 0;
        rc2 = gpt_read_at(read, ctx, bs, disk_blocks - 1u, &h, add_gpt, t);
        if (rc2 == GPT_ERR_IO)
            return PART_ERR_IO;
        if (rc2 != GPT_OK)
            return (rc == GPT_ERR_UNSUPPORTED && rc2 == GPT_ERR_UNSUPPORTED)
                       ? PART_ERR_UNSUPPORTED : PART_ERR_CORRUPT;
        t->used_backup = 1;
    }
    t->id.kind = PART_KIND_GPT;
    memcpy(t->id.guid, h.disk_guid, GPT_GUID_LEN);
    t->first_usable = h.first_usable;
    t->last_usable = h.last_usable;
    return PART_OK;
}

/* ---- MBR ---- */

/* Walks one extended partition's chain, appending its logical partitions.
 *
 * Bounded twice over, because the chain is a linked list on a disk anyone
 * can have written: each EBR's LBA is remembered and a revisit ends the
 * walk (a loop), and PART_MAX_LOGICAL EBRs end it regardless. A link or a
 * logical partition outside the extended partition ends it too (an
 * escaping chain), keeping what was found before it: Linux keeps those as
 * well, and the partitions before a bad link are no less real for it.
 *
 * One logical per EBR, from its first data entry, as every partitioning
 * tool writes it; a second data entry in the same EBR is not read. */
static int walk_extended(part_table *t, gpt_read_fn read, void *ctx,
                         uint32_t bs, const part_entry *ext, uint32_t *number)
{
    uint64_t seen[PART_MAX_LOGICAL];
    uint32_t nseen = 0;
    uint64_t cur = ext->start_lba;

    for (;;) {
        const mbr_ent *data = NULL, *link = NULL;
        mbr_sec m;
        uint32_t i;
        int rc;

        for (i = 0; i < nseen; i++) {
            if (seen[i] == cur)
                return PART_OK;
        }
        if (nseen == PART_MAX_LOGICAL) {
            t->truncated = 1;
            return PART_OK;
        }
        seen[nseen++] = cur;

        rc = read_mbr(read, ctx, bs, cur, 0, &m);
        if (rc == PART_ERR_IO)
            return rc;
        if (rc != PART_OK)
            return PART_OK;             /* no signature: the chain ends */

        for (i = 0; i < 4u; i++) {
            const mbr_ent *e = &m.e[i];

            if (e->type == 0u || e->count == 0u)
                continue;
            if (part_mbr_is_extended(e->type)) {
                if (link == NULL)
                    link = e;
            } else if (data == NULL) {
                data = e;
            }
        }

        if (data != NULL) {
            uint64_t start = cur + data->start;
            uint64_t end = start + data->count - 1u;
            part_entry *p;

            /* After its own EBR and inside the extended partition. */
            if (data->start == 0u || end > ext->end_lba)
                return PART_OK;
            p = next_slot(t);
            if (p == NULL)
                return PART_OK;
            p->start_lba = start;
            p->end_lba = end;
            p->number = (*number)++;
            p->active = (data->status == 0x80u) ? 1u : 0u;
            p->mbr_type = data->type;
        }

        if (link == NULL)
            return PART_OK;
        /* Relative to the extended partition, not to this EBR. */
        cur = ext->start_lba + link->start;
        if (cur > ext->end_lba)
            return PART_OK;
    }
}

static int read_mbr_table(part_table *t, const mbr_sec *m, gpt_read_fn read,
                          void *ctx, uint32_t bs, uint64_t disk_blocks)
{
    uint32_t i, nprim, number = 5;

    t->id.kind = PART_KIND_MBR;
    t->id.mbr_sig = m->disk_sig;
    t->first_usable = 1;
    t->last_usable = disk_blocks != 0u ? disk_blocks - 1u : 0u;

    /* The primaries first, numbered by slot: an empty slot 2 leaves a gap
     * and the next primary is still 3, which is what Linux calls it. An
     * entry with a type but no sectors is empty, as Linux treats it. */
    for (i = 0; i < 4u; i++) {
        const mbr_ent *e = &m->e[i];
        part_entry *p;

        if (e->type == 0u || e->count == 0u)
            continue;
        p = next_slot(t);
        if (p == NULL)
            break;
        p->start_lba = e->start;
        p->end_lba = (uint64_t)e->start + e->count - 1u;
        p->number = i + 1u;
        p->primary = 1;
        p->active = (e->status == 0x80u) ? 1u : 0u;
        p->mbr_type = e->type;
    }
    nprim = t->count;
    /* A 0x55AA sector with four empty entries names no partition: a
     * freshly zeroed table, or a superfloppy's boot sector with nothing in
     * the table area. Either way there is no table to hand back. */
    if (nprim == 0u)
        return PART_ERR_NO_TABLE;

    /* A chain is only walked from an extended partition that is itself
     * sane, so the walk never reads past the disk on the container's say. */
    for (i = 0; i < nprim; i++) {
        const part_entry *p = &t->parts[i];

        if (p->start_lba == 0u ||
            (disk_blocks != 0u && p->end_lba >= disk_blocks))
            return PART_ERR_CORRUPT;
    }
    for (i = 0; i < nprim; i++) {
        part_entry ext = t->parts[i];   /* a copy: the walk appends */
        int rc;

        if (!part_mbr_is_extended(ext.mbr_type))
            continue;
        rc = walk_extended(t, read, ctx, bs, &ext, &number);
        if (rc != PART_OK)
            return rc;
    }
    return PART_OK;
}

/* ---- common ---- */

static int is_container(const part_entry *p)
{
    return p->primary && part_mbr_is_extended(p->mbr_type);
}

/* The entries as a whole: each inside the disk and ordered, and no two
 * sharing a block, except an extended partition and the logicals it holds.
 * Only the stored entries are checked; a truncated table's overflow was
 * never seen. */
static int validate(const part_table *t, uint64_t disk_blocks)
{
    uint32_t i, j;

    for (i = 0; i < t->count; i++) {
        const part_entry *a = &t->parts[i];

        if (a->start_lba == 0u || a->end_lba < a->start_lba)
            return PART_ERR_CORRUPT;
        if (disk_blocks != 0u && a->end_lba >= disk_blocks)
            return PART_ERR_CORRUPT;
        for (j = i + 1u; j < t->count; j++) {
            const part_entry *b = &t->parts[j];

            if (a->start_lba > b->end_lba || b->start_lba > a->end_lba)
                continue;
            if ((is_container(a) && !b->primary) ||
                (is_container(b) && !a->primary))
                continue;
            return PART_ERR_CORRUPT;
        }
    }
    return PART_OK;
}

int part_read_table(part_table *t, gpt_read_fn read, void *ctx,
                    uint32_t block_size, uint64_t disk_blocks)
{
    mbr_sec m;
    uint32_t i;
    int rc, gpt = 0;

    if (t == NULL || t->parts == NULL || t->cap == 0u || read == NULL)
        return PART_ERR_ARG;
    if (block_size < 512u || block_size > GPT_MAX_SECTOR ||
        (block_size & (block_size - 1u)) != 0u)
        return PART_ERR_ARG;
    /* Every LBA this reads is below 2^64 / block_size once it is inside a
     * disk this long; an unknown length is bounded by the 32-bit MBR fields
     * and gpt_read_at's own checks. */
    if (disk_blocks > UINT64_MAX / block_size)
        return PART_ERR_ARG;

    t->count = 0;
    t->truncated = 0;
    t->used_backup = 0;
    memset(&t->id, 0, sizeof(t->id));
    t->first_usable = 0;
    t->last_usable = 0;
    t->first_start = 0;

    rc = read_mbr(read, ctx, block_size, 0, 1, &m);
    if (rc != PART_OK)
        return rc;

    for (i = 0; i < 4u; i++) {
        if (m.e[i].type == MBR_TYPE_GPT)
            gpt = 1;
    }
    rc = gpt ? read_gpt(t, read, ctx, block_size, disk_blocks)
             : read_mbr_table(t, &m, read, ctx, block_size, disk_blocks);
    if (rc == PART_OK)
        rc = validate(t, disk_blocks);
    if (rc != PART_OK) {
        t->count = 0;
        return rc;
    }

    for (i = 0; i < t->count; i++) {
        if (i == 0u || t->parts[i].start_lba < t->first_start)
            t->first_start = t->parts[i].start_lba;
    }
    return PART_OK;
}

const part_entry *part_find_start(const part_table *t, uint64_t start_lba)
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

/* ---- naming a disk ---- */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* n hex digits (n <= 8) into *out, or nonzero. */
static int hex_u32(const char *s, unsigned n, uint32_t *out)
{
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < n; i++) {
        int d = hex_val(s[i]);

        if (d < 0)
            return -1;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return 0;
}

static const char hex_digits[] = "0123456789abcdef";

static char *put_hex(char *p, uint32_t v, unsigned n)
{
    while (n-- > 0u)
        *p++ = hex_digits[(v >> (n * 4u)) & 0xFu];
    return p;
}

/* The GUID's text groups, 8-4-4-4-12, and how each maps onto the on-disk
 * bytes: the first three are little-endian integers, the last two are
 * bytes as written (the mixed-endian layout part.h describes). */
#define GUID_TEXT_LEN 36u
#define MBR_TEXT_LEN   8u

int part_disk_id_parse(const char *s, size_t len, part_disk_id *out)
{
    uint32_t v;
    unsigned i;

    if (s == NULL || out == NULL)
        return PART_ERR_ARG;
    memset(out, 0, sizeof(*out));

    if (len == 4u + MBR_TEXT_LEN && memcmp(s, "mbr:", 4) == 0) {
        if (hex_u32(s + 4, MBR_TEXT_LEN, &v) != 0)
            return PART_ERR_ARG;
        out->kind = PART_KIND_MBR;
        out->mbr_sig = v;
        return PART_OK;
    }
    if (len == 4u + GUID_TEXT_LEN && memcmp(s, "gpt:", 4) == 0) {
        const char *g = s + 4;

        if (g[8] != '-' || g[13] != '-' || g[18] != '-' || g[23] != '-')
            return PART_ERR_ARG;
        if (hex_u32(g, 8, &v) != 0)
            return PART_ERR_ARG;
        for (i = 0; i < 4u; i++)
            out->guid[i] = (uint8_t)(v >> (i * 8u));
        if (hex_u32(g + 9, 4, &v) != 0)
            return PART_ERR_ARG;
        out->guid[4] = (uint8_t)v;
        out->guid[5] = (uint8_t)(v >> 8);
        if (hex_u32(g + 14, 4, &v) != 0)
            return PART_ERR_ARG;
        out->guid[6] = (uint8_t)v;
        out->guid[7] = (uint8_t)(v >> 8);
        /* The last two groups are bytes in order: 2 of them, then 6. */
        for (i = 0; i < 8u; i++) {
            const char *b = g + (i < 2u ? 19u + i * 2u : 24u + (i - 2u) * 2u);

            if (hex_u32(b, 2, &v) != 0)
                return PART_ERR_ARG;
            out->guid[8u + i] = (uint8_t)v;
        }
        out->kind = PART_KIND_GPT;
        return PART_OK;
    }
    return PART_ERR_ARG;
}

void part_disk_id_format(const part_disk_id *id, char *buf)
{
    char *p = buf;
    unsigned i;

    if (buf == NULL)
        return;
    buf[0] = '\0';
    if (id == NULL)
        return;
    if (id->kind == PART_KIND_MBR) {
        memcpy(p, "mbr:", 4);
        p = put_hex(p + 4, id->mbr_sig, MBR_TEXT_LEN);
        *p = '\0';
        return;
    }
    if (id->kind != PART_KIND_GPT)
        return;
    memcpy(p, "gpt:", 4);
    p = put_hex(p + 4, get_le32(id->guid), 8);
    *p++ = '-';
    p = put_hex(p, get_le16(id->guid + 4), 4);
    *p++ = '-';
    p = put_hex(p, get_le16(id->guid + 6), 4);
    *p++ = '-';
    for (i = 8; i < GPT_GUID_LEN; i++) {
        if (i == 10u)
            *p++ = '-';
        p = put_hex(p, id->guid[i], 2);
    }
    *p = '\0';
}

int part_disk_id_eq(const part_disk_id *a, const part_disk_id *b)
{
    unsigned i;
    int zero = 1;

    if (a == NULL || b == NULL || a->kind != b->kind)
        return 0;
    if (a->kind == PART_KIND_MBR)
        return a->mbr_sig != 0u && a->mbr_sig == b->mbr_sig;
    if (a->kind != PART_KIND_GPT)
        return 0;
    /* An all-zero DiskGUID is as much "no identity" as MBR signature 0. */
    for (i = 0; i < GPT_GUID_LEN; i++) {
        if (a->guid[i] != 0u)
            zero = 0;
    }
    return !zero && memcmp(a->guid, b->guid, GPT_GUID_LEN) == 0;
}
