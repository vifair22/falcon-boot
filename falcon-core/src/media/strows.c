/* falcon-boot: the station's list, as data. See strows.h. */

#include <media/strows.h>

#include <lib/string.h>

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int name_eq(const char *a, const char *b)
{
    size_t i;

    if (a[0] == '\0' || b[0] == '\0')
        return 0;
    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++)
        if (lower(a[i]) != lower(b[i]))
            return 0;
    return a[i] == b[i];
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i;

    for (i = 0; src[i] != '\0' && i + 1u < cap; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* A path as the loader keeps it: '\' separated, whatever the INI wrote. */
static void copy_path(char *dst, size_t cap, const char *src)
{
    size_t i;

    for (i = 0; src[i] != '\0' && i + 1u < cap; i++)
        dst[i] = src[i] == '/' ? '\\' : src[i];
    dst[i] = '\0';
}

static void ascii_label(uint16_t *label, size_t cap, const char *s)
{
    size_t i;

    for (i = 0; s[i] != '\0' && i + 1u < cap; i++)
        label[i] = (uint16_t)(unsigned char)s[i];
    label[i] = 0;
}

void strows_init(strows *r)
{
    memset(r, 0, sizeof *r);
}

strow *strows_add(strows *r, strow_kind kind)
{
    strow *w;

    if (r->n >= STROWS_MAX)
        return (strow *)0;
    w = &r->row[r->n++];
    memset(w, 0, sizeof *w);
    w->kind = kind;
    return w;
}

/* The disk's id as text, and its short form: "gpt:" and the GUID's first
 * eight digits, or the whole "mbr:" and signature. */
static size_t disk_short(const part_disk_id *d, char out[16])
{
    char text[PART_DISK_ID_TEXT];
    size_t n;

    part_disk_id_format(d, text);
    for (n = 0; text[n] != '\0' && n < 12u; n++)
        out[n] = text[n];
    out[n] = '\0';
    return n;
}

static size_t put_dec(char *p, uint32_t v)
{
    char d[10];
    size_t n = 0;
    size_t i;

    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    for (i = 0; i < n; i++)
        p[i] = d[n - 1u - i];
    return n;
}

void strows_name_part(char out[STATIONINI_NAME_MAX], const part_disk_id *d,
                      uint32_t part)
{
    char id[16];
    size_t n = disk_short(d, id);

    out[0] = '\0';
    if (n != 12u)
        return;
    memcpy(out, id, 12u);
    out[3] = '-';
    n = 12u;
    if (part != 0) {
        out[n++] = '-';
        out[n++] = 'p';
        n += put_dec(out + n, part);
    }
    out[n] = '\0';
}

void strows_name_nxt(char out[STATIONINI_NAME_MAX], uint16_t option)
{
    static const char hex[] = "0123456789abcdef";
    unsigned i;

    memcpy(out, "nxt-", 4);
    for (i = 0; i < 4u; i++)
        out[4u + i] = hex[(option >> (12u - 4u * i)) & 0xFu];
    out[8] = '\0';
}

void strows_name_uki(char out[STATIONINI_NAME_MAX], const char *file)
{
    size_t len = 0;
    size_t n = 4;
    size_t i;

    while (file[len] != '\0')
        len++;
    if (len > 4u && file[len - 4u] == '.' && lower(file[len - 3u]) == 'e' &&
        lower(file[len - 2u]) == 'f' && lower(file[len - 1u]) == 'i')
        len -= 4u;
    memcpy(out, "uki-", 4);
    for (i = 0; i < len && n + 1u < STATIONINI_NAME_MAX; i++) {
        char c = file[i];

        out[n++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-')
                       ? c
                       : '-';
    }
    out[n] = '\0';
}

void strows_label_part(uint16_t *label, size_t cap, const uint16_t *what,
                       const part_disk_id *d, uint32_t part)
{
    char tail[64];
    char id[16];
    size_t n = 0;
    size_t t = 0;
    size_t i;

    if (cap == 0)
        return;
    for (i = 0; what[i] != 0 && n + 1u < cap; i++)
        label[n++] = what[i];
    disk_short(d, id);
    copy_str(tail, sizeof tail, " (disk ");
    t = 7u;
    for (i = 0; id[i] != '\0'; i++)
        tail[t++] = id[i];
    if (part != 0) {
        copy_str(tail + t, sizeof tail - t, ", partition ");
        t += 12u;
        t += put_dec(tail + t, part);
    }
    tail[t++] = ')';
    tail[t] = '\0';
    for (i = 0; tail[i] != '\0' && n + 1u < cap; i++)
        label[n++] = (uint16_t)(unsigned char)tail[i];
    label[n] = 0;
}

int strows_path_eq(const char *a, const char *b)
{
    size_t i;

    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        char x = a[i] == '/' ? '\\' : lower(a[i]);
        char y = b[i] == '/' ? '\\' : lower(b[i]);

        if (x != y)
            return 0;
    }
    return a[i] == b[i];
}

static int part_match(const strow *w, const stationini_entry *e)
{
    return w->has_part && part_disk_id_eq(&w->disk, &e->disk) &&
           (e->part == 0 || w->part == e->part);
}

static strow *first(strows *r, strow_kind kind, const stationini_entry *e,
                    const char *path)
{
    size_t i;

    for (i = 0; i < r->n; i++) {
        strow *w = &r->row[i];

        if (w->kind != kind)
            continue;
        if (kind == STROW_UKI) {
            if (strows_path_eq(w->file, e->uki))
                return w;
            continue;
        }
        if (e->has_disk) {
            if (!part_match(w, e))
                continue;
        } else if (!w->own_esp) {
            continue;
        }
        if (path == (const char *)0 || strows_path_eq(w->file, path))
            return w;
    }
    return (strow *)0;
}

/* BIOS: the chain row of that disk and exactly that partition (0: the
 * disk's MBR), or a new one, looked for when it is chosen. */
static strow *match_chain(strows *r, const stationini_entry *e)
{
    strow_kind kind = e->part != 0 ? STROW_VBR : STROW_MBR;
    strow *w;
    size_t i;

    for (i = 0; i < r->n; i++) {
        w = &r->row[i];
        if (w->kind == kind && w->has_part &&
            part_disk_id_eq(&w->disk, &e->disk) && w->part == e->part)
            return w;
    }
    w = strows_add(r, kind);
    if (w == (strow *)0)
        return w;
    w->has_part = 1;
    w->disk = e->disk;
    w->part = e->part;
    strows_name_part(w->name, &e->disk, e->part);
    strows_label_part(w->label, STROWS_LABEL_MAX,
                      e->part != 0 ? u"Partition" : u"Boot code", &e->disk,
                      e->part);
    w->added = 1;
    w->ref_none = 1;
    return w;
}

/* The row a section changes, or a new one it adds; NULL when it names
 * nothing and adds nothing. */
static strow *match(strows *r, const stationini_entry *e, strows_fw fw)
{
    strow *w;

    if (e->uki[0] != '\0') {
        w = first(r, STROW_UKI, e, (const char *)0);
        if (w == (strow *)0 && (w = strows_add(r, STROW_UKI)) != (strow *)0) {
            copy_str(w->file, sizeof w->file, e->uki);
            strows_name_uki(w->name, e->uki);
            ascii_label(w->label, STROWS_LABEL_MAX, e->uki);
            w->added = 1;
            w->ref_none = 1;
        }
        return w;
    }
    if (fw == STROWS_BIOS) {
        if (e->efi[0] != '\0' || e->method == STATIONINI_METHOD_EFI)
            return (strow *)0;
        if (e->has_disk)
            return match_chain(r, e);
        return strows_find(r, e->name);
    }
    if (e->has_disk) {
        const char *path = e->efi[0] != '\0' ? e->efi : STROWS_BOOTMGFW;

        if (e->method == STATIONINI_METHOD_EFI || e->efi[0] != '\0') {
            w = first(r, STROW_ESP, e, path);
            if (w != (strow *)0)
                return w;
            w = strows_add(r, STROW_ESP);
            if (w == (strow *)0)
                return w;
            copy_path(w->file, sizeof w->file, path);
            if (e->efi[0] == '\0')
                strows_label_part(w->label, STROWS_LABEL_MAX,
                                  u"Windows Boot Manager", &e->disk, e->part);
            else
                ascii_label(w->label, STROWS_LABEL_MAX, e->efi);
        } else {
            w = first(r, STROW_NXT, e, (const char *)0);
            if (w == (strow *)0)
                w = first(r, STROW_ESP, e, (const char *)0);
            if (w != (strow *)0)
                return w;
            w = strows_add(r, STROW_NXT);
            if (w == (strow *)0)
                return w;
            ascii_label(w->label, STROWS_LABEL_MAX, e->name);
        }
        w->has_part = 1;
        w->disk = e->disk;
        w->part = e->part;
        strows_name_part(w->name, &e->disk, e->part);
        w->added = 1;
        w->ref_none = 1;
        return w;
    }
    if (e->efi[0] != '\0') {
        w = first(r, STROW_ESP, e, e->efi);
        if (w == (strow *)0 && (w = strows_add(r, STROW_ESP)) != (strow *)0) {
            copy_path(w->file, sizeof w->file, e->efi);
            ascii_label(w->label, STROWS_LABEL_MAX, e->efi);
            w->own_esp = 1;
            w->added = 1;
            w->ref_none = 1;
        }
        return w;
    }
    return strows_find(r, e->name);
}

void strows_apply(strows *r, const stationini *c, strows_fw fw)
{
    unsigned i;

    if (r == (strows *)0 || c == (const stationini *)0)
        return;
    for (i = 0; i < c->n_entries; i++) {
        const stationini_entry *e = &c->entries[i];
        strow *w = match(r, e, fw);

        if (w == (strow *)0)
            continue;
        copy_str(w->section, sizeof w->section, e->name);
        if (e->label[0] != '\0')
            ascii_label(w->label, STROWS_LABEL_MAX, e->label);
        if (e->hide == STATIONINI_TRI_YES)
            w->hidden = 1;
        else if (e->hide == STATIONINI_TRI_NO)
            w->hidden = 0;
        if (e->swap == STATIONINI_TRI_NO)
            w->no_swap = 1;
        else if (e->swap == STATIONINI_TRI_YES)
            w->no_swap = 0;
    }
}

static long find_index(const strows *r, const char *name)
{
    size_t i;

    if (name == (const char *)0 || name[0] == '\0')
        return -1;
    for (i = 0; i < r->n; i++)
        if (name_eq(r->row[i].section, name))
            return (long)i;
    for (i = 0; i < r->n; i++)
        if (name_eq(r->row[i].name, name))
            return (long)i;
    return -1;
}

strow *strows_find(strows *r, const char *name)
{
    long i = find_index(r, name);

    return i < 0 ? (strow *)0 : &r->row[i];
}

static int in_view(const size_t *view, size_t n, size_t idx)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (view[i] == idx)
            return 1;
    return 0;
}

size_t strows_arrange(const strows *r, const stationini *c, size_t *view,
                      size_t cap)
{
    size_t n = 0;
    size_t i;

    if (c != (const stationini *)0) {
        for (i = 0; i < c->n_order && n < cap; i++) {
            long k = find_index(r, c->order[i]);

            if (k < 0 || r->row[k].hidden ||
                r->row[k].kind == STROW_FWSETUP ||
                in_view(view, n, (size_t)k))
                continue;
            view[n++] = (size_t)k;
        }
    }
    for (i = 0; i < r->n && n < cap; i++) {
        if (r->row[i].hidden || r->row[i].kind == STROW_FWSETUP ||
            in_view(view, n, i))
            continue;
        view[n++] = i;
    }
    for (i = 0; i < r->n && n < cap; i++)
        if (!r->row[i].hidden && r->row[i].kind == STROW_FWSETUP)
            view[n++] = i;
    return n;
}

int strows_default(const strows *r, const stationini *c, const size_t *view,
                   size_t n, size_t *sel)
{
    long k;
    size_t i;

    if (c == (const stationini *)0)
        return 0;
    k = find_index(r, c->default_name);
    if (k < 0)
        return 0;
    for (i = 0; i < n; i++) {
        if (view[i] == (size_t)k) {
            *sel = i;
            return 1;
        }
    }
    return 0;
}
