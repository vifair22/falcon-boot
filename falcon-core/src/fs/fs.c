/* The filesystem registry of DESIGN Section 9: the vtable's wrappers, the
 * lookups over the table consumers pick from, and the pieces the adapters
 * share. The adapters themselves, one per driver, are fs_<driver>.c, and the
 * table is fs_reg_all.c or fs_reg_ext4.c: a build links the table it wants
 * and with it only the drivers that table names, which is how the
 * stationary build (DESIGN Section 13) carries ext4 and nothing else.
 *
 * The adapters are deliberately thin and deliberately not clever. Each one
 * casts the caller's opaque blob to its driver's context, rebuilds the
 * driver's own dirent from the unified one when a call needs it, and maps the
 * status back. No driver source changed to make this work, which is the
 * property that lets the driver test groups keep pinning driver behaviour
 * unchanged: if an adapter is wrong, the fs group catches it, and the driver
 * groups stay the statement about the driver.
 *
 * Why the dirent is rebuilt rather than carried whole: the driver dirents are
 * mostly name (528 and 264 bytes, of which 512 and 256 are the name), and the
 * unified dirent already carries the name in the one canonical form. So the
 * private part is the handful of bytes that actually locate the object -
 * exFAT's first cluster and chain flag, ext4's inode and file type - and the
 * unified dirent stays roughly the size of the driver dirent it replaces
 * instead of the sum of both.
 */

#include <fs/fs.h>
#include <fs/fs_adapter.h>

#include <lib/string.h>

/* Each adapter asserts, beside its driver, that the blobs hold every
 * context it keeps there: a driver that grows past FS_VOL_SIZE and its
 * siblings breaks the build rather than overrunning a caller's object at
 * run time. And each asserts that its extent type and fs_extent are the same
 * two fields in the same order, which is what lets the wrapper hand the
 * driver the caller's array directly instead of copying a potentially large
 * run table twice. */

/* ---- name helpers ---- */

static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

size_t fs_name_to_bytes(const uint16_t *name, uint8_t *out, size_t cap)
{
    size_t i = 0;

    if (out == (uint8_t *)0 || cap == 0)
        return 0;
    if (name != (const uint16_t *)0) {
        while (name[i] != 0 && i + 1 < cap) {
            out[i] = (name[i] <= 0xFFu) ? (uint8_t)name[i] : (uint8_t)'?';
            i++;
        }
    }
    out[i] = 0;
    return i;
}

size_t fs_name_from_bytes(const uint8_t *name, size_t len, uint16_t *out,
                          size_t cap)
{
    size_t i = 0;

    if (out == (uint16_t *)0 || cap == 0)
        return 0;
    if (name != (const uint8_t *)0) {
        while (i < len && i + 1 < cap) {
            out[i] = name[i];
            i++;
        }
    }
    out[i] = 0;
    return i;
}

int fs_name_eq_ascii(const uint16_t *name, const char *ascii)
{
    size_t i = 0;

    if (name == (const uint16_t *)0 || ascii == (const char *)0)
        return 0;
    while (name[i] != 0) {
        if (ascii[i] == '\0')
            return 0;
        if (ascii_lower((int)name[i]) != ascii_lower((int)(unsigned char)ascii[i]))
            return 0;
        i++;
    }
    return ascii[i] == '\0';
}

int fs_name_ends_ascii(const uint16_t *name, const char *suffix)
{
    size_t nl = 0;
    size_t sl = 0;
    size_t i;

    if (name == (const uint16_t *)0 || suffix == (const char *)0)
        return 0;
    while (name[nl] != 0)
        nl++;
    while (suffix[sl] != '\0')
        sl++;
    if (sl == 0 || nl < sl)
        return 0;
    for (i = 0; i < sl; i++) {
        if (ascii_lower((int)name[nl - sl + i]) !=
            ascii_lower((int)(unsigned char)suffix[i]))
            return 0;
    }
    return 1;
}

int fs_name_cmp(const uint16_t *a, const uint16_t *b)
{
    size_t i = 0;

    while (a[i] != 0 && b[i] != 0) {
        int ca = ascii_lower((int)a[i]);
        int cb = ascii_lower((int)b[i]);

        if (ca != cb)
            return (ca < cb) ? -1 : 1;
        i++;
    }
    if (a[i] == b[i])
        return 0;
    return (a[i] == 0) ? -1 : 1;
}

/* ---- status mapping ----
 *
 * One row per driver status. The drivers' own enums are never renumbered:
 * their tests pin those values and they are the right vocabulary inside the
 * driver. The tables are what makes the two vocabularies one at the seam. */

int fs_map_status(const fs_status_row *rows, size_t n, int rc)
{
    size_t i;

    if (rc >= 0)
        return rc; /* OK, and dir_next's "1 = entry produced", pass through */
    for (i = 0; i < n; i++) {
        if (rows[i].driver == rc)
            return rows[i].unified;
    }
    /* A driver that grew a status without a row here: report the
     * conservative answer rather than leak an unmapped number. */
    return FS_ERR_CORRUPT;
}

/* ---- the registry ----
 *
 * The table is fs_registry (fs_reg_all.c or fs_reg_ext4.c); probe order is
 * its order. */

size_t fs_driver_count(void)
{
    return fs_registry_n;
}

const fs_ops *fs_driver_at(size_t i)
{
    return (i < fs_registry_n) ? fs_registry[i] : (const fs_ops *)0;
}

const fs_ops *fs_by_name(const char *name)
{
    size_t i;
    size_t j;

    if (name == (const char *)0)
        return (const fs_ops *)0;
    for (i = 0; i < fs_registry_n; i++) {
        const char *n = fs_registry[i]->name;

        for (j = 0; n[j] != '\0' && name[j] != '\0' && n[j] == name[j]; j++)
            ;
        if (n[j] == '\0' && name[j] == '\0')
            return fs_registry[i];
    }
    return (const fs_ops *)0;
}

const fs_ops *fs_identify(fs_read_fn read, void *ctx, fs_hint hint)
{
    unsigned pass;
    size_t i;

    if (read == (fs_read_fn)0)
        return (const fs_ops *)0;

    /* Pass 0 is the role's own driver, pass 1 everything else in table
     * order. The fallback is not a nicety: an exFAT volume sitting on a
     * partition typed as the shelf is a mistake worth reporting honestly at
     * the next layer up, not a volume to declare unreadable here. */
    for (pass = 0; pass < 2u; pass++) {
        for (i = 0; i < fs_registry_n; i++) {
            const fs_ops *o = fs_registry[i];
            int preferred = (hint != FS_HINT_NONE && o->role == hint);

            if ((pass == 0u) != (preferred != 0))
                continue;
            if (o->probe(read, ctx) == FS_OK)
                return o;
        }
    }
    return (const fs_ops *)0;
}

/* ---- the wrappers ---- */

int fs_mount(fs_vol *v, const fs_ops *ops, fs_read_fn read, void *ctx)
{
    int rc;

    if (v == (fs_vol *)0 || ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    v->ops = (const fs_ops *)0;
    rc = ops->mount(v, read, ctx);
    if (rc == FS_OK)
        v->ops = ops;
    return rc;
}

int fs_mount_journal(fs_vol *v, const fs_ops *ops, fs_read_fn read, void *ctx,
                     const fs_jmem *mem, fs_jinfo *info)
{
    int rc;

    if (info == (fs_jinfo *)0)
        return FS_ERR_BAD_ARG;
    info->flags = 0;
    info->need = 0;
    if (v == (fs_vol *)0 || ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    if (ops->mount_journal == 0)
        return fs_mount(v, ops, read, ctx);
    v->ops = (const fs_ops *)0;
    rc = ops->mount_journal(v, read, ctx, mem, info);
    if (rc == FS_OK)
        v->ops = ops;
    return rc;
}

int fs_dir_open_root(fs_vol *v, fs_dir *d)
{
    if (v == (fs_vol *)0 || d == (fs_dir *)0 || v->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    d->ops = v->ops;
    return v->ops->dir_open_root(v, d);
}

int fs_dir_open(fs_vol *v, fs_dir *d, const fs_dirent *e)
{
    if (v == (fs_vol *)0 || d == (fs_dir *)0 || e == (const fs_dirent *)0 ||
        v->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    d->ops = v->ops;
    return v->ops->dir_open(v, d, e);
}

int fs_dir_next(fs_dir *d, fs_dirent *out)
{
    if (d == (fs_dir *)0 || out == (fs_dirent *)0 ||
        d->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    return d->ops->dir_next(d, out);
}

int fs_file_open(fs_vol *v, fs_file *f, const fs_dirent *e)
{
    if (v == (fs_vol *)0 || f == (fs_file *)0 || e == (const fs_dirent *)0 ||
        v->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    f->ops = v->ops;
    return v->ops->file_open(v, f, e);
}

int fs_file_read(fs_file *f, uint64_t off, uint64_t len, void *buf)
{
    if (f == (fs_file *)0 || f->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    return f->ops->file_read(f, off, len, buf);
}

int fs_file_read_cb(void *file, uint64_t off, uint64_t len, void *buf)
{
    return fs_file_read((fs_file *)file, off, len, buf) == FS_OK ? 0 : -1;
}

uint64_t fs_file_size(const fs_file *f)
{
    if (f == (const fs_file *)0 || f->ops == (const fs_ops *)0)
        return 0;
    return f->ops->file_size(f);
}

int fs_has_extents(const fs_vol *v)
{
    if (v == (const fs_vol *)0 || v->ops == (const fs_ops *)0)
        return 0;
    return v->ops->extents != (int (*)(fs_vol *, const fs_dirent *,
                                       fs_extent *, size_t, size_t *))0;
}

int fs_extents(fs_vol *v, const fs_dirent *e, fs_extent *out, size_t cap,
               size_t *count)
{
    if (v == (fs_vol *)0 || e == (const fs_dirent *)0 ||
        count == (size_t *)0 || v->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    if (!fs_has_extents(v)) {
        *count = 0;
        return FS_ERR_NO_CAPABILITY;
    }
    return v->ops->extents(v, e, out, cap, count);
}

int fs_lookup(fs_vol *v, const char *path, fs_dir *scratch, fs_dirent *out)
{
    const char *p = path;
    int have = 0;
    int rc;

    if (v == (fs_vol *)0 || path == (const char *)0 ||
        scratch == (fs_dir *)0 || out == (fs_dirent *)0 ||
        v->ops == (const fs_ops *)0)
        return FS_ERR_BAD_ARG;
    rc = fs_dir_open_root(v, scratch);
    if (rc != FS_OK)
        return rc;
    for (;;) {
        char comp[FS_NAME_MAX + 1];
        size_t n = 0;
        fs_dirent e;
        int found = 0;

        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        while (p[n] != '\0' && p[n] != '/') {
            if (n >= FS_NAME_MAX)
                return FS_ERR_BAD_ARG;
            comp[n] = p[n];
            n++;
        }
        comp[n] = '\0';
        p += n;
        if (have) {
            if (!out->is_dir)
                return FS_ERR_NOT_FOUND;
            rc = fs_dir_open(v, scratch, out);
            if (rc != FS_OK)
                return rc;
        }
        while ((rc = fs_dir_next(scratch, &e)) == 1) {
            if (fs_name_eq_ascii(e.name, comp)) {
                found = 1;
                break;
            }
        }
        if (rc < 0)
            return rc;
        if (!found)
            return FS_ERR_NOT_FOUND;
        memcpy(out, &e, sizeof e);
        have = 1;
    }
    return have ? FS_OK : FS_ERR_BAD_ARG;
}

const char *fs_vol_name(const fs_vol *v)
{
    if (v == (const fs_vol *)0 || v->ops == (const fs_ops *)0)
        return "none";
    return v->ops->name;
}

const char *fs_vol_label(const fs_vol *v)
{
    const char *l;

    if (v == (const fs_vol *)0 || v->ops == (const fs_ops *)0 ||
        v->ops->label == (const char *(*)(const fs_vol *))0)
        return "";
    l = v->ops->label(v);
    return l != (const char *)0 ? l : "";
}
