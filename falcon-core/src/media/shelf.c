#include <media/shelf.h>

#include <crypto/crypto.h>
#include <lib/string.h>
#include <lib/ucs2.h>

/* ---- finding the images ---- */

static int name_ends_efi(const fs_dirent *e)
{
    /* ".efi" on its own is a name, not a UKI. */
    if (e->name_len < 5u)
        return 0;
    return fs_name_ends_ascii(e->name, ".efi");
}

static int dir_find(fs_dir *in, const char *name, fs_dirent *found)
{
    fs_dirent ent;
    int rc;

    while ((rc = fs_dir_next(in, &ent)) == 1) {
        if (fs_name_eq_ascii(ent.name, name)) {
            memcpy(found, &ent, sizeof ent);
            return 1;
        }
    }
    return rc; /* 0 = not found, negative = the driver's error */
}

/* Opens the directory the UKIs live in, leaving it in *d. Returns 1 when one
 * was found, 0 when neither convention is present, negative on a filesystem
 * error. */
static int open_uki_dir(shelf_list *s, fs_vol *fs, fs_dir *d)
{
    fs_dirent ent;
    int rc;

    if (fs_dir_open_root(fs, &s->dir_a) != FS_OK)
        return -1;
    rc = dir_find(&s->dir_a, "uki", &ent);
    if (rc == 1 && ent.is_dir)
        return (fs_dir_open(fs, d, &ent) == FS_OK) ? 1 : -1;
    if (rc < 0)
        return rc;

    if (fs_dir_open_root(fs, &s->dir_a) != FS_OK)
        return -1;
    rc = dir_find(&s->dir_a, "EFI", &ent);
    if (rc != 1 || !ent.is_dir)
        return (rc < 0) ? rc : 0;
    if (fs_dir_open(fs, &s->dir_b, &ent) != FS_OK)
        return -1;
    rc = dir_find(&s->dir_b, "Linux", &ent);
    if (rc != 1 || !ent.is_dir)
        return (rc < 0) ? rc : 0;
    return (fs_dir_open(fs, d, &ent) == FS_OK) ? 1 : -1;
}

/* dir_b is both open_uki_dir's scratch for the /EFI level and the directory
 * it finally leaves open; that is safe only because every dirent it acts on
 * is copied out before the next open overwrites the iterator. */
int shelf_collect(shelf_list *s, fs_vol *fs)
{
    fs_dir *d = &s->dir_b;
    fs_dirent ent;
    int rc;

    s->n = 0;
    rc = open_uki_dir(s, fs, d);
    if (rc != 1)
        return rc;

    while ((rc = fs_dir_next(d, &ent)) == 1) {
        size_t j;

        if (!ent.is_regular || !name_ends_efi(&ent))
            continue;
        if (s->n >= SHELF_MAX)
            break;

        /* Insertion sort as they arrive; the list is at most a handful.
         * memmove for the shift: the two elements are distinct and cannot
         * overlap, but saying "shift one element up" with the routine that
         * permits overlap costs nothing here and keeps -fanalyzer from
         * having to prove it. */
        j = s->n++;
        while (j > 0 && fs_name_cmp(s->uki[j - 1u].ent.name, ent.name) > 0) {
            memmove(&s->uki[j].ent, &s->uki[j - 1u].ent, sizeof ent);
            j--;
        }
        memcpy(&s->uki[j].ent, &ent, sizeof ent);
    }
    if (rc < 0)
        return rc;
    return (s->n > 0) ? 1 : 0;
}

/* Case-folded ASCII compare over the raw UCS-2 name: mkstick writes these
 * names and they are ASCII by construction, so a name that is not cannot be
 * the appliance. */
int shelf_is_appliance(const fs_dirent *e)
{
    static const char want[] = SHELF_APPLIANCE_PREFIX;
    size_t i;

    if (e->name_len + 1u < sizeof want)
        return 0;
    for (i = 0; i + 1u < sizeof want; i++) {
        uint16_t c = e->name[i];

        if (c >= u'A' && c <= u'Z')
            c = (uint16_t)(c + (u'a' - u'A'));
        if (c != (uint16_t)(unsigned char)want[i])
            return 0;
    }
    return 1;
}

/* ---- labels from .osrel ---- */

/* Reads one named section into s->sect. Returns its length, or 0. The file
 * must be open on s->file. */
static size_t uki_section(shelf_list *s, const char *section, uint64_t fsize,
                          uint64_t have)
{
    size_t off = 0;
    size_t size = 0;

    if (osrel_find(s->head, (size_t)have, (size_t)fsize, section, &off,
                   &size) != OSREL_OK)
        return 0;
    if (size > SHELF_SECT_MAX)
        size = SHELF_SECT_MAX;
    if ((uint64_t)off + (uint64_t)size > fsize)
        return 0;
    if (fs_file_read(&s->file, (uint64_t)off, (uint64_t)size, s->sect) !=
        FS_OK)
        return 0;
    return size;
}

/* Reads the display name and the kernel version in one open.
 *
 * .uname is read even when it is not needed yet, because whether it IS needed
 * depends on the OTHER entries: it is what separates two images of the same
 * distro, and that is only known once every row has a label. One open per
 * entry rather than two passes of I/O over the shelf. Costs two small reads,
 * not a 90 MB one: a window off the front to find the section, then the
 * section. */
static int uki_osrel_label(shelf_list *s, fs_vol *fs, const fs_dirent *ent,
                           char *out, size_t cap, char *uname, size_t ucap)
{
    uint64_t have;
    uint64_t fsize;
    size_t n;
    int named = 0;

    out[0] = '\0';
    uname[0] = '\0';
    if (fs_file_open(fs, &s->file, ent) != FS_OK)
        return 0;

    fsize = fs_file_size(&s->file);
    have = fsize < (uint64_t)SHELF_HEAD_WINDOW ? fsize
                                               : (uint64_t)SHELF_HEAD_WINDOW;
    if (fs_file_read(&s->file, 0, have, s->head) != FS_OK)
        return 0;

    n = uki_section(s, OSREL_SECT_OSREL, fsize, have);
    if (n != 0) {
        /* One condition rather than a chain, because both keys mean the same
         * thing to us. Short-circuit order IS the preference: PRETTY_NAME
         * wins and NAME is only consulted when it did not answer.
         * osrel_value empties out on failure, so the second call never sees
         * a half-written first attempt. */
        if (osrel_value((const char *)s->sect, n, "PRETTY_NAME", out, cap) ==
                OSREL_OK ||
            osrel_value((const char *)s->sect, n, "NAME", out, cap) ==
                OSREL_OK)
            named = 1;
    }
    if (!named)
        out[0] = '\0';

    /* .uname is a bare version string, not KEY=value, so it is copied rather
     * than parsed. Stops at the first byte that cannot be in one, which
     * covers the trailing NUL and the newline some writers add. */
    n = uki_section(s, OSREL_SECT_UNAME, fsize, have);
    if (n != 0) {
        size_t i = 0;

        while (i < n && i + 1u < ucap) {
            char c = (char)s->sect[i];

            if (c < 0x21 || c > 0x7E)
                break;
            uname[i] = c;
            i++;
        }
        uname[i] = '\0';
    }
    return named;
}

static void ascii_to_ucs2(uint16_t *dst, size_t cap, const char *src)
{
    size_t i = 0;

    while (src[i] != '\0' && i + 1u < cap) {
        dst[i] = (uint16_t)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

/* Everything a shelf holds is ASCII by construction (mkstick writes those
 * names), so anything else is shown as '?' rather than guessed at: a
 * byte-widened UTF-8 name is not text. */
static void name_to_ucs2(uint16_t *dst, size_t cap, const uint16_t *name,
                         size_t len)
{
    size_t i;

    for (i = 0; i < len && i + 1u < cap; i++)
        dst[i] = (name[i] >= 0x20u && name[i] < 0x7Fu) ? name[i] : u'?';
    dst[i] = 0;
}

/* Exact, not case-folded. Two rows that differ only in case are still two
 * distinguishable rows on screen, and qualifying them would add noise. */
static int ucs2_eq(const uint16_t *a, const uint16_t *b)
{
    size_t i = 0;

    while (a[i] != 0 && a[i] == b[i])
        i++;
    return a[i] == b[i];
}

/* Does another entry with the same label also carry the same kernel version?
 * Then the version cannot separate them and the file name has to. Two copies
 * of one UKI under different names is exactly that case, and it is the shape
 * a rollback copy has. */
static int uname_shared(const shelf_list *s, size_t i)
{
    size_t j;

    for (j = 0; j < s->n; j++) {
        size_t k = 0;

        if (j == i || !s->uki[j].dup)
            continue;
        if (!ucs2_eq(s->uki[i].label, s->uki[j].label))
            continue;
        while (s->uki[i].uname[k] != '\0' &&
               s->uki[i].uname[k] == s->uki[j].uname[k])
            k++;
        if (s->uki[i].uname[k] == s->uki[j].uname[k])
            return 1;
    }
    return 0;
}

/* Chooser order: the newest kernel first, the appliance always last. Between
 * two images without a .uname, and between two with the same one, the
 * shelf's own order stands. */
static int uki_before(const shelf_list *s, size_t a, size_t b)
{
    int app_a = shelf_is_appliance(&s->uki[a].ent);
    int app_b = shelf_is_appliance(&s->uki[b].ent);

    if (app_a != app_b)
        return app_b;
    if (s->uki[a].uname[0] != '\0' && s->uki[b].uname[0] != '\0')
        return osrel_vercmp(s->uki[a].uname, s->uki[b].uname) > 0;
    return s->uki[a].uname[0] != '\0' && s->uki[b].uname[0] == '\0';
}

void shelf_label(shelf_list *s, fs_vol *fs)
{
    static const uint16_t prefix[] = u"Personal OS: ";
    static shelf_uki tmp;
    size_t pl = sizeof(prefix) / sizeof(prefix[0]) - 1u;
    size_t i;

    for (i = 0; i < s->n; i++) {
        char label[OSREL_LABEL_MAX];
        shelf_uki *u = &s->uki[i];

        memcpy(u->label, prefix, pl * sizeof(uint16_t));
        /* named is whether .osrel answered, not whether the row has text:
         * a row showing its file name is already as specific as it gets,
         * and qualifying it would print the name twice. */
        u->named = (uint8_t)uki_osrel_label(s, fs, &u->ent, label,
                                            sizeof label, u->uname,
                                            sizeof u->uname);
        if (u->named)
            ascii_to_ucs2(u->label + pl, SHELF_LABEL_MAX - pl, label);
        else
            name_to_ucs2(u->label + pl, SHELF_LABEL_MAX - pl, u->ent.name,
                         u->ent.name_len);
    }

    /* Stable insertion sort: a handful of rows, and ties keep shelf order. A
     * static temporary because a row is over a kilobyte. */
    for (i = 1; i < s->n; i++) {
        size_t j;

        for (j = i; j > 0 && uki_before(s, j, j - 1u); j--) {
            tmp = s->uki[j];
            s->uki[j] = s->uki[j - 1u];
            s->uki[j - 1u] = tmp;
        }
    }

    /* Two images that say the same thing about themselves are the common
     * case, not the exotic one: every Arch UKI on the shelf carries
     * PRETTY_NAME "Arch Linux", so a rollback copy beside the current one
     * produces two identical, unpickable rows. Every row whose label is not
     * unique is qualified, and only those: qualifying everything would throw
     * away the whole point of reading .osrel.
     *
     * TWO PASSES, and the split is not tidiness. Marking and rewriting in one
     * loop compares later rows against labels earlier iterations have already
     * rewritten: the first of a colliding pair gets qualified, and the second
     * then matches nothing and is left bare. Measured in the rig on
     * 2026-09-15 - "Test Linux (alpha.efi)" above a naked "Test Linux" -
     * which is worse than leaving both alone, because it reads as though the
     * two rows are different KINDS of thing. */
    for (i = 0; i < s->n; i++) {
        size_t j;

        s->uki[i].dup = 0;
        if (!s->uki[i].named)
            continue;               /* already showing its file name */
        for (j = 0; j < s->n && !s->uki[i].dup; j++) {
            if (j != i && ucs2_eq(s->uki[i].label, s->uki[j].label))
                s->uki[i].dup = 1;
        }
    }

    /* The kernel version is the qualifier worth showing, and the file name
     * is the fallback when there is no .uname or two images somehow share
     * one. No brackets on the version: it reads as part of the name, which is
     * what sd-boot shows too.
     *
     * DECIDE for every row, THEN rewrite, for the same reason and with the
     * same bug if not: uname_shared compares labels, and the rewrite below
     * mutates exactly those. Fused into one loop, the last member of a group
     * matches nothing, looks unique, and is handed a kernel version it
     * shares with the rows above it. Measured in the rig on 2026-09-15 with
     * five label fixtures (falcon-efi/tools/mk-test-labels.sh): "Twin Linux
     * (twin-a.efi)" above "Twin Linux 9.9.9-twin", which claims the two are
     * different kernels when they are the same image twice. */
    for (i = 0; i < s->n; i++)
        s->uki[i].byver = (s->uki[i].dup && s->uki[i].uname[0] != '\0' &&
                           !uname_shared(s, i));

    for (i = 0; i < s->n; i++) {
        uint16_t *row = s->uki[i].label;
        size_t at;

        if (!s->uki[i].dup)
            continue;
        at = ucs2_len(row);

        if (s->uki[i].byver) {
            if (at + 2u < SHELF_LABEL_MAX) {
                row[at++] = u' ';
                ascii_to_ucs2(row + at, SHELF_LABEL_MAX - at,
                              s->uki[i].uname);
            }
            continue;
        }

        if (at + 3u < SHELF_LABEL_MAX) {
            row[at++] = u' ';
            row[at++] = u'(';
            name_to_ucs2(row + at, SHELF_LABEL_MAX - at - 2u,
                         s->uki[i].ent.name, s->uki[i].ent.name_len);
            at = ucs2_len(row);
            if (at + 2u < SHELF_LABEL_MAX) {
                row[at++] = u')';
                row[at] = 0;
            }
        }
    }
    crypto_wipe(&tmp, sizeof tmp);
}

void shelf_wipe(shelf_list *s)
{
    crypto_wipe(s, sizeof *s);
}
