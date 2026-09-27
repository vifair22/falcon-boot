/* falcon-bios: the BIOS station's menu rows. See rows.h. */

#include <station/rows.h>

#include <lib/string.h>

static char lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

/* Names are matched exactly, as stationini_find matches sections: two
 * sections the parser keeps apart are two rows here. */
static int name_eq(const char *a, const char *b)
{
    size_t i;

    if (a[0] == '\0' || b[0] == '\0')
        return 0;
    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return a[i] == b[i];
}

/* File names are matched as FAT and a person match them: case aside. */
static int file_eq(const char *a, const char *b)
{
    size_t i;

    if (a[0] == '\0' || b[0] == '\0')
        return 0;
    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        if (lower(a[i]) != lower(b[i]))
            return 0;
    }
    return a[i] == b[i];
}

/* Copies s into out (cap bytes), cut to fit, always terminated. */
static void copy_cut(char *out, size_t cap, const char *s)
{
    size_t i = 0;

    while (s[i] != '\0' && i + 1u < cap) {
        out[i] = s[i];
        i++;
    }
    out[i] = '\0';
}

static size_t put_dec(char *out, uint32_t v)
{
    char tmp[10];
    size_t n = 0;
    size_t i;

    do {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0 && n < sizeof tmp);
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1u - i];
    out[n] = '\0';
    return n;
}

void rows_init(st_rows *rs)
{
    memset(rs, 0, sizeof *rs);
    rs->def = -1;
}

int rows_find(const st_rows *rs, const char *name)
{
    unsigned i;

    for (i = 0; i < rs->n; i++) {
        if (name_eq(rs->row[i].name, name) || name_eq(rs->row[i].alias, name))
            return (int)i;
    }
    return -1;
}

st_row *rows_add(st_rows *rs, row_kind kind, const char *name)
{
    st_row *r;
    uint32_t k;

    if (rs->n >= ROWS_MAX)
        return (st_row *)0;
    r = &rs->row[rs->n];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->swap = 1;
    copy_cut(r->name, sizeof r->name, name);
    /* A taken name gets a number, cut so the number always fits. */
    for (k = 2; rows_find(rs, r->name) >= 0 && k < 1000u; k++) {
        char num[12];
        size_t nl;
        size_t base;

        num[0] = '-';
        nl = 1u + put_dec(num + 1, k);
        copy_cut(r->name, sizeof r->name - nl, name);
        base = 0;
        while (r->name[base] != '\0')
            base++;
        memcpy(r->name + base, num, nl + 1u);
    }
    rs->n++;
    return r;
}

void rows_name_disk(char *out, const part_disk_id *id, uint32_t part)
{
    char text[PART_DISK_ID_TEXT];
    size_t n;

    part_disk_id_format(id, text);
    /* "mbr:" or "gpt:" and eight hex digits: the whole signature, or the
     * DiskGUID's first group. */
    if (text[0] == '\0') {
        copy_cut(out, STATIONINI_NAME_MAX, "disk");
        n = 4;
    } else {
        memcpy(out, text, 3);
        out[3] = '-';
        memcpy(out + 4, text + 4, 8);
        n = 12;
    }
    if (part != 0) {
        out[n++] = '-';
        out[n++] = 'p';
        n += put_dec(out + n, part);
    }
    out[n] = '\0';
}

static int name_char(uint16_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

void rows_name_uki(char *out, const uint16_t *file)
{
    size_t len = 0;
    size_t n = 4;
    size_t i;

    while (file[len] != 0)
        len++;
    if (len > 4u && file[len - 4u] == '.' &&
        lower((char)(file[len - 3u] & 0x7Fu)) == 'e' &&
        lower((char)(file[len - 2u] & 0x7Fu)) == 'f' &&
        lower((char)(file[len - 1u] & 0x7Fu)) == 'i')
        len -= 4u;
    memcpy(out, "uki-", 4);
    for (i = 0; i < len && n + 1u < STATIONINI_NAME_MAX; i++)
        out[n++] = name_char(file[i]) ? (char)file[i] : '_';
    out[n] = '\0';
}

void rows_label_ascii(uint16_t *out, const char *s)
{
    size_t i = 0;

    while (s[i] != '\0' && i + 1u < ROW_LABEL_MAX) {
        out[i] = (uint16_t)(uint8_t)s[i];
        i++;
    }
    out[i] = 0;
}

void rows_label_disk(uint16_t *out, const char *what, const part_disk_id *id,
                     uint32_t part)
{
    char text[ROW_LABEL_MAX];
    char idt[PART_DISK_ID_TEXT];
    size_t n = 0;

    part_disk_id_format(id, idt);
    copy_cut(text, sizeof text, what);
    while (text[n] != '\0')
        n++;
    copy_cut(text + n, sizeof text - n, " (disk ");
    while (text[n] != '\0')
        n++;
    copy_cut(text + n, sizeof text - n, idt);
    while (text[n] != '\0')
        n++;
    if (part != 0) {
        char num[12];

        copy_cut(text + n, sizeof text - n, ", partition ");
        while (text[n] != '\0')
            n++;
        (void)put_dec(num, part);
        copy_cut(text + n, sizeof text - n, num);
        while (text[n] != '\0')
            n++;
    }
    copy_cut(text + n, sizeof text - n, ")");
    rows_label_ascii(out, text);
}

/* The last path component of s, so "uki = /uki/arch.efi" and "uki =
 * arch.efi" name one file. */
static const char *base_name(const char *s)
{
    const char *b = s;

    for (; *s != '\0'; s++) {
        if (*s == '/' || *s == '\\')
            b = s + 1;
    }
    return b;
}

static int find_disk_row(const st_rows *rs, unsigned disk, uint32_t part)
{
    unsigned i;

    for (i = 0; i < rs->n; i++) {
        const st_row *r = &rs->row[i];

        if ((r->kind == ROW_PART || r->kind == ROW_DISK) && r->disk == disk &&
            r->part == part)
            return (int)i;
    }
    return -1;
}

static int find_uki_row(const st_rows *rs, const char *file)
{
    unsigned i;

    for (i = 0; i < rs->n; i++) {
        if (rs->row[i].kind == ROW_UKI &&
            file_eq(base_name(rs->row[i].uki), base_name(file)))
            return (int)i;
    }
    return -1;
}

static void change(st_row *r, const stationini_entry *e)
{
    if (e->label[0] != '\0')
        rows_label_ascii(r->label, e->label);
    if (e->hide != STATIONINI_TRI_UNSET)
        r->hide = e->hide == STATIONINI_TRI_YES;
    if (e->swap != STATIONINI_TRI_UNSET)
        r->swap = e->swap == STATIONINI_TRI_YES;
    if (!name_eq(r->name, e->name))
        copy_cut(r->alias, sizeof r->alias, e->name);
}

static void add_disk(st_rows *rs, const stationini_entry *e, int disk,
                     rows_note_fn note, void *ctx)
{
    st_row *r = rows_add(rs, e->part != 0 ? ROW_PART : ROW_DISK, e->name);

    if (r == (st_row *)0) {
        note(e->name, "the menu is full", ctx);
        return;
    }
    r->disk = (unsigned)disk;
    r->part = e->part;
    r->added = 1;
    rows_label_disk(r->label, e->part != 0 ? "Partition" : "Boot code",
                    &e->disk, e->part);
    change(r, e);
}

void rows_apply_ini(st_rows *rs, const stationini *c, unsigned shelves,
                    rows_disk_fn disk_of, rows_note_fn note, void *ctx)
{
    unsigned i;

    for (i = 0; i < c->n_entries; i++) {
        const stationini_entry *e = &c->entries[i];
        int at;

        if (e->has_disk) {
            int disk = disk_of(&e->disk, ctx);

            if (disk < 0) {
                note(e->name, "its disk is not attached", ctx);
                continue;
            }
            at = find_disk_row(rs, (unsigned)disk, e->part);
            if (at >= 0)
                change(&rs->row[at], e);
            else
                add_disk(rs, e, disk, note, ctx);
            continue;
        }
        if (e->uki[0] != '\0') {
            st_row *r;

            at = find_uki_row(rs, e->uki);
            if (at >= 0) {
                change(&rs->row[at], e);
                continue;
            }
            if (shelves == 0) {
                note(e->name, "there is no shelf for its uki", ctx);
                continue;
            }
            r = rows_add(rs, ROW_UKI, e->name);
            if (r == (st_row *)0) {
                note(e->name, "the menu is full", ctx);
                continue;
            }
            r->shelf = 0;
            r->added = 1;
            copy_cut(r->uki, sizeof r->uki, e->uki);
            rows_label_ascii(r->label, base_name(e->uki));
            change(r, e);
            continue;
        }
        if (e->efi[0] != '\0') {
            note(e->name, "efi entries are the UEFI loader's", ctx);
            continue;
        }
        at = rows_find(rs, e->name);
        if (at >= 0)
            change(&rs->row[at], e);
        else
            note(e->name, "it names no entry this loader found", ctx);
    }
}

static int shown_has(const st_rows *rs, unsigned idx)
{
    unsigned i;

    for (i = 0; i < rs->n_shown; i++) {
        if (rs->shown[i] == idx)
            return 1;
    }
    return 0;
}

void rows_arrange(st_rows *rs, const stationini *c)
{
    unsigned i;

    rs->n_shown = 0;
    rs->def = -1;
    rs->timeout = 0;
    for (i = 0; c != (const stationini *)0 && i < c->n_order; i++) {
        int at = rows_find(rs, c->order[i]);

        if (at >= 0 && !rs->row[at].hide && !shown_has(rs, (unsigned)at))
            rs->shown[rs->n_shown++] = (unsigned)at;
    }
    for (i = 0; i < rs->n; i++) {
        if (!rs->row[i].hide && !shown_has(rs, i))
            rs->shown[rs->n_shown++] = i;
    }
    if (c == (const stationini *)0 || c->default_name[0] == '\0')
        return;
    /* The default is shown even when a section hides it: a countdown runs
     * on a row the person can see and stop. */
    {
        int at = rows_find(rs, c->default_name);

        if (at >= 0 && rs->row[at].hide && rs->n_shown < ROWS_MAX)
            rs->shown[rs->n_shown++] = (unsigned)at;
    }
    for (i = 0; i < rs->n_shown; i++) {
        const st_row *r = &rs->row[rs->shown[i]];

        if (name_eq(r->name, c->default_name) ||
            name_eq(r->alias, c->default_name)) {
            rs->def = (int)i;
            rs->timeout = c->timeout;
            return;
        }
    }
}
