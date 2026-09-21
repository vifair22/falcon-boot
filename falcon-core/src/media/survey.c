/* falcon-boot: the volume survey and the folder view (survey.h).
 */

#include <media/survey.h>

#include <lib/string.h>
#include <lib/ucs2.h>

static const uint16_t k_iso[] = { '.', 'i', 's', 'o', 0 };
static const uint16_t k_img[] = { '.', 'i', 'm', 'g', 0 };
static const uint16_t k_efi[] = { '.', 'e', 'f', 'i', 0 };

void survey_init(survey_list *l, survey_entry *storage, size_t cap,
                 unsigned accept)
{
    if (l == (survey_list *)0)
        return;
    l->entries = storage;
    l->cap = (storage == (survey_entry *)0) ? 0u : cap;
    l->count = 0;
    l->skipped = 0;
    l->accept = accept;
    l->prefix[0] = 0;
    l->cur_vol = 0;
    l->entries_left = 0;
    l->dirs_left = 0;
}

static void list_add(survey_list *l, const fs_dirent *ent,
                     survey_shape shape, survey_media media)
{
    survey_entry *e;
    size_t pl = ucs2_len(l->prefix);
    size_t nl = ucs2_len(ent->name);

    if (l->count >= l->cap || pl + nl + 1u > SURVEY_PATH_MAX) {
        l->skipped++;
        return;
    }
    e = &l->entries[l->count];
    l->count++;

    memcpy(e->label, l->prefix, pl * sizeof(uint16_t));
    memcpy(e->label + pl, ent->name, (nl + 1u) * sizeof(uint16_t));
    e->dir_len = pl;
    e->vol = l->cur_vol;
    e->shape = shape;
    e->media = media;
    memcpy(&e->ent, ent, sizeof(*ent));
}

/* Case-insensitive, so a stick written by Windows lists the same files. */
static int classify(const survey_list *l, const uint16_t *name,
                    survey_shape *shape, survey_media *media)
{
    if ((l->accept & SURVEY_ACCEPT_ISO) != 0u && ucs2_ends_with(name, k_iso)) {
        *shape = SURVEY_SHAPE_MEDIA;
        *media = SURVEY_MEDIA_CD;
        return 1;
    }
    if ((l->accept & SURVEY_ACCEPT_IMG) != 0u && ucs2_ends_with(name, k_img)) {
        *shape = SURVEY_SHAPE_MEDIA;
        *media = SURVEY_MEDIA_DISK;
        return 1;
    }
    if ((l->accept & SURVEY_ACCEPT_EFI) != 0u && ucs2_ends_with(name, k_efi)) {
        /* Not media, so the kind is the enum's zero and nothing reads it. */
        *shape = SURVEY_SHAPE_PAYLOAD;
        *media = SURVEY_MEDIA_CD;
        return 1;
    }
    return 0;
}

static void walk(survey_list *l, fs_vol *vol, fs_dir *dir, size_t depth)
{
    fs_dirent ent;

    while (l->entries_left > 0u && fs_dir_next(dir, &ent) == 1) {
        survey_shape shape;
        survey_media media;

        l->entries_left--;

        if (ent.name[0] == (uint16_t)'.')
            continue;   /* dotfiles, and the trash directories that follow */

        if (ent.is_dir) {
            fs_dir sub;
            size_t pl = ucs2_len(l->prefix);
            size_t nl = ucs2_len(ent.name);

            if (depth + 1u >= SURVEY_DEPTH_MAX)
                continue;
            /* The name, its separator and the terminator. */
            if (pl + nl + 2u > SURVEY_PATH_MAX)
                continue;
            if (l->dirs_left == 0u)
                continue;
            l->dirs_left--;
            if (fs_dir_open(vol, &sub, &ent) != FS_OK)
                continue;
            /* Extended in place, restored on the way out: one buffer, not
             * one per frame. */
            memcpy(l->prefix + pl, ent.name, nl * sizeof(uint16_t));
            l->prefix[pl + nl] = (uint16_t)'/';
            l->prefix[pl + nl + 1u] = 0;
            walk(l, vol, &sub, depth + 1u);
            l->prefix[pl] = 0;
        } else if (classify(l, ent.name, &shape, &media)) {
            list_add(l, &ent, shape, media);
        }
    }
}

int survey_volume(survey_list *l, fs_vol *vol, size_t vol_index)
{
    fs_dir root;

    if (l == (survey_list *)0 || l->entries == (survey_entry *)0 ||
        vol == (fs_vol *)0)
        return -1;
    if (fs_dir_open_root(vol, &root) != FS_OK)
        return -1;

    l->cur_vol = vol_index;
    l->prefix[0] = 0;
    l->entries_left = SURVEY_ENTRY_BUDGET;
    l->dirs_left = SURVEY_DIR_BUDGET;
    walk(l, vol, &root, 0);
    l->prefix[0] = 0;
    return 0;
}

void survey_sort(survey_list *l)
{
    size_t i, j;

    if (l == (survey_list *)0 || l->entries == (survey_entry *)0)
        return;

    /* Insertion sort. At most a couple of hundred rows off a stick somebody
     * filled by hand. */
    for (i = 1; i < l->count; i++) {
        survey_entry key;

        memcpy(&key, &l->entries[i], sizeof key);
        j = i;
        while (j > 0 &&
               ucs2_casecmp(l->entries[j - 1u].label, key.label) > 0) {
            memcpy(&l->entries[j], &l->entries[j - 1u], sizeof key);
            j--;
        }
        memcpy(&l->entries[j], &key, sizeof key);
    }
}

/* ---- the folder view ---- */

/* At or below the folder on screen? Exact, not the sort's case-insensitive
 * compare: both strings came out of the same walk, so a difference in case is
 * a difference in directory. Reads stay in the buffer because cur_len is
 * shorter than it and a shorter label's NUL mismatches a prefix unit. */
static int under_cur(const uint16_t *label, const uint16_t *cur,
                     size_t cur_len)
{
    size_t i;

    for (i = 0; i < cur_len; i++) {
        if (label[i] != cur[i])
            return 0;
    }
    return 1;
}

/* Length of the next component below the current folder. The walk terminates
 * every directory prefix with '/', so for dir_len > cur_len it is there. */
static size_t comp_len(const uint16_t *label, size_t cur_len)
{
    size_t n = 0;

    while (label[cur_len + n] != (uint16_t)'/' && label[cur_len + n] != 0)
        n++;
    return n;
}

/* Same folder as the cl units at p? The row carries its '/' and a component
 * never contains one, so a shorter row mismatches at its separator. */
static int dir_row_is(const uint16_t *row, const uint16_t *p, size_t cl)
{
    size_t i;

    for (i = 0; i < cl; i++) {
        if (row[i] != p[i])
            return 0;
    }
    return row[cl] == (uint16_t)'/';
}

size_t survey_view(const survey_list *l, const uint16_t *cur, size_t cur_len,
                   survey_row *rows, size_t cap)
{
    size_t n = 0;
    size_t i, j;

    if (l == (const survey_list *)0 || l->entries == (const survey_entry *)0 ||
        rows == (survey_row *)0)
        return 0;

    for (i = 0; i < l->count && n < cap; i++) {
        const uint16_t *label = l->entries[i].label;
        size_t cl;

        if (l->entries[i].dir_len <= cur_len ||
            !under_cur(label, cur, cur_len))
            continue;
        cl = comp_len(label, cur_len);
        for (j = 0; j < n; j++) {
            if (rows[j].kind == SURVEY_ROW_DIR &&
                dir_row_is(rows[j].label, label + cur_len, cl))
                break;
        }
        if (j < n)
            continue;   /* a sibling already put this folder on screen */
        memcpy(rows[n].label, label + cur_len, cl * sizeof(uint16_t));
        rows[n].label[cl] = (uint16_t)'/';
        rows[n].label[cl + 1u] = 0;
        rows[n].kind = SURVEY_ROW_DIR;
        rows[n].index = i;
        n++;
    }

    for (i = 0; i < l->count && n < cap; i++) {
        const uint16_t *label = l->entries[i].label;

        if (l->entries[i].dir_len != cur_len ||
            !under_cur(label, cur, cur_len))
            continue;
        (void)ucs2_copy(rows[n].label, SURVEY_PATH_MAX, label + cur_len);
        rows[n].kind = SURVEY_ROW_FILE;
        rows[n].index = i;
        n++;
    }

    return n;
}

int survey_enter(uint16_t *cur, size_t *cur_len, const uint16_t *row)
{
    size_t n;

    if (cur == (uint16_t *)0 || cur_len == (size_t *)0 ||
        row == (const uint16_t *)0)
        return -1;
    n = ucs2_len(row);
    if (*cur_len + n + 1u > SURVEY_PATH_MAX)
        return -1;
    memcpy(cur + *cur_len, row, (n + 1u) * sizeof(uint16_t));
    *cur_len += n;
    return 0;
}

void survey_up(uint16_t *cur, size_t *cur_len)
{
    size_t n;

    if (cur == (uint16_t *)0 || cur_len == (size_t *)0 || *cur_len == 0u)
        return;
    n = *cur_len - 1u;                    /* the trailing '/' */
    while (n > 0u && cur[n - 1u] != (uint16_t)'/')
        n--;
    cur[n] = 0;
    *cur_len = n;
}
