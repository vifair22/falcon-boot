/* falcon-boot-sign: reading an ISO without mounting it.
 *
 * The bless tool has to fingerprint an ISO's layout (DESIGN Section 6.2) and
 * pull files out of it. Loop-mounting would need root, which a user blessing
 * an ISO on their own laptop should not need, and would put a mount of
 * untrusted media into a tool whose whole job is to be the careful step.
 *
 * So the ISO is read in process by falcon-core's own ISO9660 and UDF drivers
 * (DESIGN Section 6.4), through the filesystem registry: the same code the
 * loader reads an ISO with at boot, which makes "what the sign tool found"
 * and "what the loader will find" one answer rather than two. Before this the
 * tool listed the image with bsdtar for ISO9660 and 7-Zip for UDF and unioned
 * the two listings; both helpers are gone from the bless path.
 *
 * One name space, the registry's choice. A Windows install image is a UDF
 * bridge disc whose ISO9660 side is a stub with one README, and the registry
 * probes UDF before ISO9660 for exactly that reason; a Linux image has no UDF
 * and is read through Rock Ridge.
 *
 * The tree is walked once at open and indexed: every entry's path (relative
 * to the root, no leading slash), its size and the dirent the driver needs to
 * open it again. Lookups are then string matches over the index, which is
 * what falcon-core/src/media/family.c's exists() and the kernel table want.
 *
 * Path convention: family.h specifies absolute, '/'-separated, ASCII, no
 * trailing slash, matched case-insensitively. Rock Ridge names keep the
 * author's case and oscdimg's UDF names are lower case, so every comparison
 * here folds both sides.
 */

#include <sign/sign.h>
#include <sign/isoprobe.h>

#include <fs/fs.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Content fingerprints read the head of a small text file (family.c asks for
 * 1 KB of a loader entry). Nothing legitimate needs more. */
#define MEMBER_MAX (4u << 20)

/* How deep the index walk descends. Deep-directory relocation lets Rock Ridge
 * go past ISO9660's eight levels; no medium in the corpus goes past twelve. */
#define WALK_DEPTH_MAX 32u

/* How many entries an image may hold before the index gives up. The largest
 * medium in the corpus holds about 1400. */
#define ENTRIES_MAX 200000u

typedef struct {
    char     *name;     /* heap, normalised: no leading or trailing '/' */
    uint64_t  size;
    uint8_t   is_dir;
    fs_dirent ent;      /* for reopening; unused in a view with no volume */
} iso_entry;

struct iso_view {
    int         fd;                     /* -1 for a view with no image */
    fs_vol     *vol;                    /* NULL for a view with no image */
    iso_entry  *entries;
    size_t      n_entries;
    size_t      cap;
};

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static int ci_eq(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (lower_ascii(a[i]) != lower_ascii(b[i]))
            return 0;
    }
    return 1;
}

static const char *strip_leading(const char *p)
{
    for (;;) {
        if (*p == '/')
            p++;
        else if (p[0] == '.' && p[1] == '/')
            p += 2;
        else
            return p;
    }
}

/* ------------------------------------------------------------ the index */

static int entries_push(iso_view *v, const char *name, uint64_t size,
                        uint8_t is_dir, const fs_dirent *ent)
{
    iso_entry *e;

    if (v->n_entries >= ENTRIES_MAX) {
        sgn_set_error(SGN_ERR_FORMAT, "the ISO holds more than %u entries",
                      ENTRIES_MAX);
        return SGN_ERR_FORMAT;
    }
    if (v->n_entries == v->cap) {
        size_t cap = (v->cap == 0) ? 256u : v->cap * 2u;
        iso_entry *bigger = realloc(v->entries, cap * sizeof(*bigger));

        if (bigger == NULL) {
            sgn_set_error(SGN_ERR_IO, "out of memory indexing the ISO");
            return SGN_ERR_IO;
        }
        v->entries = bigger;
        v->cap = cap;
    }
    e = &v->entries[v->n_entries];
    memset(e, 0, sizeof(*e));
    e->name = strdup(name);
    if (e->name == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory indexing the ISO");
        return SGN_ERR_IO;
    }
    e->size = size;
    e->is_dir = is_dir;
    if (ent != NULL)
        e->ent = *ent;
    v->n_entries++;
    return SGN_OK;
}

/* Depth-first over the mounted volume. The path buffer is extended in place
 * and restored on the way out; each level's iterator is on the heap because
 * it carries a driver's block cache. */
static int index_dir(iso_view *v, fs_dir *d, char *path, size_t cap,
                     unsigned depth)
{
    fs_dirent e;
    int rc;

    while ((rc = fs_dir_next(d, &e)) == 1) {
        char name[FS_NAME_MAX * 3u + 1u];
        size_t pl = strlen(path);
        size_t nl;
        int w;

        nl = fs_name_to_bytes(e.name, (uint8_t *)name, sizeof(name));
        if (nl == 0 || memchr(name, '/', nl) != NULL)
            continue;   /* no name a path can carry */
        w = snprintf(path + pl, cap - pl, "%s%s", pl ? "/" : "", name);
        if (w < 0 || (size_t)w >= cap - pl) {
            path[pl] = '\0';
            sgn_set_error(SGN_ERR_FORMAT, "a path in the ISO is too long");
            return SGN_ERR_FORMAT;
        }
        rc = entries_push(v, path, e.size, e.is_dir, &e);
        if (rc != SGN_OK)
            return rc;
        if (e.is_dir && depth + 1u < WALK_DEPTH_MAX) {
            fs_dir *sub = malloc(sizeof(*sub));

            if (sub == NULL) {
                sgn_set_error(SGN_ERR_IO, "out of memory indexing the ISO");
                return SGN_ERR_IO;
            }
            rc = fs_dir_open(v->vol, sub, &e);
            if (rc == FS_OK) {
                rc = index_dir(v, sub, path, cap, depth + 1u);
            } else {
                sgn_set_error(SGN_ERR_FORMAT, "the ISO's directory '%s' "
                              "cannot be read (status %d)", path, rc);
                rc = SGN_ERR_FORMAT;
            }
            free(sub);
            if (rc != SGN_OK)
                return rc;
        }
        path[pl] = '\0';
    }
    if (rc < 0) {
        sgn_set_error(SGN_ERR_FORMAT, "the ISO's directory tree is corrupt "
                      "(%s status %d)", fs_vol_name(v->vol), rc);
        return SGN_ERR_FORMAT;
    }
    return SGN_OK;
}

static int fd_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    int fd = *(const int *)ctx;
    uint8_t *p = buf;

    while (len > 0) {
        ssize_t n = pread(fd, p, (size_t)len, (off_t)off);

        if (n <= 0)
            return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

static iso_view *view_alloc(void)
{
    iso_view *v = calloc(1, sizeof(*v));

    if (v == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory");
        return NULL;
    }
    v->fd = -1;
    return v;
}

/* Mounts through the registry and indexes the whole tree into v. The volume
 * keeps ctx for every later read, so ctx must live as long as v does. */
static int view_index(iso_view *v, fs_read_fn read, void *ctx,
                      const char *what)
{
    const fs_ops *ops;
    fs_dir *root;
    char *path;
    int rc;

    v->vol = malloc(sizeof(*v->vol));
    root = malloc(sizeof(*root));
    path = malloc(PATH_MAX);
    if (v->vol == NULL || root == NULL || path == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory");
        rc = SGN_ERR_IO;
        goto out;
    }

    ops = fs_identify(read, ctx, FS_HINT_NONE);
    if (ops == NULL || (ops != &fs_ops_udf && ops != &fs_ops_iso9660)) {
        sgn_set_error(SGN_ERR_FORMAT, "'%s' is not an ISO9660 or UDF image",
                      what);
        rc = SGN_ERR_FORMAT;
        goto out;
    }
    rc = fs_mount(v->vol, ops, read, ctx);
    if (rc != FS_OK) {
        sgn_set_error(SGN_ERR_FORMAT, "'%s': the %s volume cannot be read "
                      "(status %d)%s", what, ops->name, rc,
                      rc == FS_ERR_UNSUPPORTED
                          ? "; it uses a feature this reader does not have"
                          : "");
        rc = SGN_ERR_FORMAT;
        goto out;
    }
    if (fs_dir_open_root(v->vol, root) != FS_OK) {
        sgn_set_error(SGN_ERR_FORMAT, "'%s': the root directory cannot be "
                      "read", what);
        rc = SGN_ERR_FORMAT;
        goto out;
    }
    path[0] = '\0';
    rc = index_dir(v, root, path, PATH_MAX, 0);
    if (rc == SGN_OK && v->n_entries == 0) {
        sgn_set_error(SGN_ERR_FORMAT, "'%s' contains no files", what);
        rc = SGN_ERR_FORMAT;
    }
    if (rc == SGN_OK)
        sgn_log(SGN_LOG_DEBUG, "indexed %zu entries in %s (%s)",
                v->n_entries, what, ops->name);
out:
    free(root);
    free(path);
    return rc;
}

int iso_view_open_cb(fs_read_fn read, void *ctx, const char *what,
                     iso_view **out)
{
    iso_view *v;
    int rc;

    if (read == NULL || out == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "iso_view_open: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    *out = NULL;
    v = view_alloc();
    if (v == NULL)
        return SGN_ERR_IO;
    rc = view_index(v, read, ctx, what != NULL ? what : "the image");
    if (rc != SGN_OK) {
        iso_view_close(v);
        return rc;
    }
    *out = v;
    return SGN_OK;
}

int iso_view_open(const char *path, iso_view **out)
{
    iso_view *v;
    int rc;

    if (path == NULL || out == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "iso_view_open: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    *out = NULL;
    v = view_alloc();
    if (v == NULL)
        return SGN_ERR_IO;
    v->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (v->fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "opening ISO '%s'", path);
        iso_view_close(v);
        return SGN_ERR_IO;
    }
    /* The descriptor lives in the view, and its address is the context the
     * mounted volume reads through for as long as the view is open. */
    rc = view_index(v, fd_read, &v->fd, path);
    if (rc != SGN_OK) {
        iso_view_close(v);
        return rc;
    }
    *out = v;
    return SGN_OK;
}

int iso_view_from_paths(const char *const *paths, const uint64_t *sizes,
                        iso_view **out)
{
    iso_view *v;
    size_t i;

    if (paths == NULL || out == NULL)
        return SGN_ERR_INTERNAL;
    *out = NULL;
    v = view_alloc();
    if (v == NULL)
        return SGN_ERR_IO;
    for (i = 0; paths[i] != NULL; i++) {
        const char *p = strip_leading(paths[i]);
        size_t n = strlen(p);
        int rc;

        if (n == 0)
            continue;
        rc = entries_push(v, p, sizes != NULL ? sizes[i] : 0,
                          (uint8_t)(p[n - 1u] == '/'), NULL);
        if (rc != SGN_OK) {
            iso_view_close(v);
            return rc;
        }
        if (p[n - 1u] == '/')
            v->entries[v->n_entries - 1u].name[n - 1u] = '\0';
    }
    *out = v;
    return SGN_OK;
}

void iso_view_close(iso_view *v)
{
    size_t i;

    if (v == NULL)
        return;
    for (i = 0; i < v->n_entries; i++)
        free(v->entries[i].name);
    free(v->entries);
    free(v->vol);
    if (v->fd >= 0)
        close(v->fd);
    free(v);
}

const char *iso_view_reader(const iso_view *v)
{
    return (v != NULL && v->vol != NULL) ? fs_vol_name(v->vol) : "none";
}

size_t iso_view_entries(const iso_view *v)
{
    return (v != NULL) ? v->n_entries : 0;
}

/* ------------------------------------------------------------ lookup */

/* Does entry name query, or something inside it? The second half keeps a
 * query for a directory working in a view built from file paths alone. */
static int entry_matches(const char *entry, const char *query, size_t qlen)
{
    size_t elen = strlen(entry);

    if (elen < qlen)
        return 0;
    if (!ci_eq(entry, query, qlen))
        return 0;
    return elen == qlen || entry[qlen] == '/';
}

static const iso_entry *find_entry(const iso_view *v, const char *path)
{
    const char *q;
    size_t qlen;
    size_t i;
    int wild;

    if (v == NULL || path == NULL)
        return NULL;
    q = strip_leading(path);
    qlen = strlen(q);
    wild = (qlen > 0 && q[qlen - 1u] == '*');
    if (wild)
        qlen--;
    if (qlen == 0)
        return NULL;

    for (i = 0; i < v->n_entries; i++) {
        const char *e = v->entries[i].name;

        if (wild) {
            if (strlen(e) < qlen || !ci_eq(e, q, qlen))
                continue;
        } else if (!entry_matches(e, q, qlen)) {
            continue;
        }
        return &v->entries[i];
    }
    return NULL;
}

int iso_view_find(iso_view *v, const char *path, const char **match)
{
    const iso_entry *e = find_entry(v, path);

    if (match != NULL)
        *match = (e != NULL) ? e->name : NULL;
    return e != NULL;
}

/* The entry a member pointer came from. Members handed out by iso_view_find
 * are entry names, so identity answers exactly; a pointer from anywhere else
 * finds nothing. */
static const iso_entry *entry_of(const iso_view *v, const char *member)
{
    size_t i;

    if (v == NULL || member == NULL)
        return NULL;
    for (i = 0; i < v->n_entries; i++) {
        if (v->entries[i].name == member)
            return &v->entries[i];
    }
    return NULL;
}

int iso_view_size(const iso_view *v, const char *member, uint64_t *out)
{
    const iso_entry *e = entry_of(v, member);

    if (out != NULL)
        *out = 0;
    if (e == NULL || out == NULL || e->is_dir)
        return 0;
    *out = e->size;
    return 1;
}

/* Opens a member for reading. SGN_ERR_FORMAT for a directory, a symlink or a
 * file the driver refuses (a zisofs-compressed one); the caller's message
 * names the member. */
static int member_open(iso_view *v, const char *member, fs_file **out)
{
    const iso_entry *e = entry_of(v, member);
    fs_file *f;
    int rc;

    *out = NULL;
    if (e == NULL || v->vol == NULL || e->is_dir) {
        sgn_set_error(SGN_ERR_FORMAT, "'%s' is not a file in the ISO",
                      member != NULL ? member : "(null)");
        return SGN_ERR_FORMAT;
    }
    f = malloc(sizeof(*f));
    if (f == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory");
        return SGN_ERR_IO;
    }
    rc = fs_file_open(v->vol, f, &e->ent);
    if (rc != FS_OK) {
        free(f);
        sgn_set_error(SGN_ERR_FORMAT, "'%s' cannot be read from the ISO "
                      "(status %d)%s", member, rc,
                      rc == FS_ERR_SYMLINK ? ": it is a symlink" :
                      rc == FS_ERR_UNSUPPORTED ? ": it is stored compressed"
                                               : "");
        return SGN_ERR_FORMAT;
    }
    *out = f;
    return SGN_OK;
}

/* ------------------------------------------------------------ probe */

static int probe_exists(void *ctx, const char *path)
{
    return iso_view_find((iso_view *)ctx, path, NULL);
}

static int probe_read(void *ctx, const char *path, uint64_t off, void *buf,
                      size_t len, size_t *got)
{
    iso_view *v = ctx;
    const char *member = NULL;
    fs_file *f = NULL;
    uint64_t size, take;

    if (got != NULL)
        *got = 0;
    /* A view with no image behind it answers layout questions and never
     * content ones: an unreadable fingerprint is one that does not match. */
    if (v == NULL || v->vol == NULL)
        return 1;
    if (!iso_view_find(v, path, &member) || member == NULL)
        return 1;
    if (member_open(v, member, &f) != SGN_OK)
        return 1;
    size = fs_file_size(f);
    if (size > MEMBER_MAX)
        size = MEMBER_MAX;
    if (off >= size) {
        free(f);
        return 0;   /* past end of file: zero bytes, not an error */
    }
    take = size - off;
    if (take > len)
        take = len;
    if (fs_file_read(f, off, take, buf) != FS_OK) {
        free(f);
        return 1;
    }
    free(f);
    if (got != NULL)
        *got = (size_t)take;
    return 0;
}

void iso_view_family_probe(iso_view *v, family_probe *fp)
{
    if (fp == NULL)
        return;
    fp->exists = probe_exists;
    fp->read = probe_read;
    fp->ctx = v;
}

/* ------------------------------------------------------------ extract */

int iso_view_extract(iso_view *v, const char *member, const char *dst)
{
    static uint8_t buf[1u << 20];
    fs_file *f = NULL;
    uint64_t size, off = 0;
    FILE *out;
    int rc;

    if (v == NULL || member == NULL || dst == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "iso_view_extract: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    rc = member_open(v, member, &f);
    if (rc != SGN_OK)
        return rc;
    out = fopen(dst, "wb");
    if (out == NULL) {
        free(f);
        sgn_set_error_errno(SGN_ERR_IO, "creating %s", dst);
        return SGN_ERR_IO;
    }
    size = fs_file_size(f);
    while (off < size) {
        uint64_t n = size - off;

        if (n > sizeof(buf))
            n = sizeof(buf);
        rc = fs_file_read(f, off, n, buf);
        if (rc != FS_OK) {
            (void)fclose(out);
            (void)unlink(dst);
            free(f);
            sgn_set_error(SGN_ERR_FORMAT, "reading '%s' from the ISO failed "
                          "at byte %llu (status %d)", member,
                          (unsigned long long)off, rc);
            return SGN_ERR_FORMAT;
        }
        if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
            (void)fclose(out);
            (void)unlink(dst);
            free(f);
            sgn_set_error_errno(SGN_ERR_IO, "writing %s", dst);
            return SGN_ERR_IO;
        }
        off += n;
    }
    free(f);
    if (fclose(out) != 0) {
        (void)unlink(dst);
        sgn_set_error_errno(SGN_ERR_IO, "writing %s", dst);
        return SGN_ERR_IO;
    }
    return SGN_OK;
}

/* ------------------------------------------------------ kernel paths */

/* Where each family keeps the kernel the bless wrap is built around.
 *
 * This table lives in the tool and not in falcon-core/src/media/family.c, on
 * purpose. The registry is the loader's data: it answers "what cmdline does
 * this family need", which the loader consumes at boot. Which file inside the
 * ISO holds the kernel is a question only the signing step ever asks, and
 * answering it in the registry would put a host-tooling concern into an EFI
 * module for no one's benefit.
 *
 * Candidates are tried in order; a trailing '*' matches any suffix, which
 * is what Debian live images need since their kernel carries the version. */
typedef struct {
    family_id          id;
    const char *const *paths;
} kernel_row;

static const char *const k_casper[] = {
    "/casper/vmlinuz", "/casper/vmlinuz.efi", "/casper/vmlinuz*", NULL
};
static const char *const k_debian_live[] = {
    "/live/vmlinuz", "/live/vmlinuz*", NULL
};
static const char *const k_fedora[] = {
    "/images/pxeboot/vmlinuz", "/isolinux/vmlinuz", NULL
};
static const char *const k_arch[] = {
    "/arch/boot/x86_64/vmlinuz-linux", NULL
};
static const char *const k_systemrescue[] = {
    "/sysresccd/boot/x86_64/vmlinuz", NULL
};
static const char *const k_gentoo[] = {
    "/boot/gentoo", NULL
};

/* Initramfs names that do not start with "initr". A family absent here keeps
 * its initramfs beside the kernel under that prefix. */
static const char *const i_gentoo[] = {
    "/boot/gentoo.igz", NULL
};

static const kernel_row g_initrds[] = {
    { FAMILY_GENTOO, i_gentoo },
};

static const kernel_row g_kernels[] = {
    { FAMILY_DEBIAN_CASPER, k_casper },
    { FAMILY_DEBIAN_LIVE,   k_debian_live },
    { FAMILY_FEDORA_DRACUT, k_fedora },
    { FAMILY_ARCH,          k_arch },
    { FAMILY_SYSTEMRESCUE,  k_systemrescue },
    { FAMILY_GENTOO,        k_gentoo },
    /* FAMILY_MEMTEST is a bare EFI binary with no kernel to wrap, and it
     * needs no blessing: it boots with no cmdline and no backing media.
     * FAMILY_WINDOWS boots no Linux kernel at all. Both are absent rather
     * than empty so the "no entry" message can say why. */
};

const char *iso_kernel_path(iso_view *v, family_id id)
{
    size_t i;

    if (v == NULL)
        return NULL;
    for (i = 0; i < sizeof(g_kernels) / sizeof(g_kernels[0]); i++) {
        size_t j;

        if (g_kernels[i].id != id)
            continue;
        for (j = 0; g_kernels[i].paths[j] != NULL; j++) {
            const char *match = NULL;

            if (iso_view_find(v, g_kernels[i].paths[j], &match))
                return match;
        }
        sgn_set_error(SGN_ERR_FORMAT,
                      "no kernel found in the ISO for family '%s' (looked for "
                      "%s and %zu alternative(s))",
                      family_by_id(id)->name, g_kernels[i].paths[0], j - 1u);
        return NULL;
    }
    sgn_set_error(SGN_ERR_FORMAT,
                  "family '%s' has no kernel to wrap: it does not boot a "
                  "Linux kernel from the ISO, so blessing does not apply",
                  family_by_id(id)->name);
    return NULL;
}

const char *iso_initrd_path(iso_view *v, family_id id, const char *kernel)
{
    char guess[PATH_MAX];
    const char *match = NULL;
    const char *slash;
    size_t dirlen;
    size_t i;

    if (v == NULL || kernel == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "iso_initrd_path: bad arguments");
        return NULL;
    }
    for (i = 0; i < sizeof(g_initrds) / sizeof(g_initrds[0]); i++) {
        size_t j;

        if (g_initrds[i].id != id)
            continue;
        for (j = 0; g_initrds[i].paths[j] != NULL; j++) {
            if (iso_view_find(v, g_initrds[i].paths[j], &match))
                return match;
        }
    }

    /* The kernel member is ISO-relative with no leading slash. */
    slash = strrchr(kernel, '/');
    dirlen = (slash != NULL) ? (size_t)(slash - kernel) : 0u;
    if (snprintf(guess, sizeof(guess), "/%.*s/initr*", (int)dirlen, kernel) >=
        (int)sizeof(guess)) {
        sgn_set_error(SGN_ERR_NOSPACE, "initramfs path too long");
        return NULL;
    }
    if (iso_view_find(v, guess, &match))
        return match;
    sgn_set_error(SGN_ERR_FORMAT,
                  "no initramfs found next to the kernel in the ISO (looked "
                  "for %s); --no-initrd builds the wrap without one", guess);
    return NULL;
}

/* ------------------------------------------------------------ label */

int iso_volume_label(const char *path, char *out, size_t cap)
{
    fs_vol *vol;
    const char *label;
    size_t n, i;
    int fd, rc;

    if (path == NULL || out == NULL || cap == 0u) {
        sgn_set_error(SGN_ERR_INTERNAL, "iso_volume_label: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "opening %s", path);
        return SGN_ERR_IO;
    }
    vol = malloc(sizeof(*vol));
    if (vol == NULL) {
        close(fd);
        sgn_set_error(SGN_ERR_IO, "out of memory");
        return SGN_ERR_IO;
    }
    /* The ISO9660 driver by name, not the registry's pick: the label a live
     * initramfs looks for (dracut's CDLABEL=, udev's by-label) is the
     * Primary Volume Descriptor's, even on a disc that also carries UDF. */
    rc = fs_mount(vol, &fs_ops_iso9660, fd_read, &fd);
    if (rc != FS_OK) {
        free(vol);
        close(fd);
        sgn_set_error(SGN_ERR_FORMAT, "%s has no ISO9660 volume descriptor",
                      path);
        return SGN_ERR_FORMAT;
    }
    label = fs_vol_label(vol);
    n = strlen(label);
    rc = SGN_OK;
    if (n == 0u) {
        sgn_set_error(SGN_ERR_FORMAT, "%s has an empty volume label", path);
        rc = SGN_ERR_FORMAT;
    }
    for (i = 0; rc == SGN_OK && i < n; i++) {
        unsigned char c = (unsigned char)label[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            sgn_set_error(SGN_ERR_FORMAT,
                          "%s: volume label has a character a kernel cmdline "
                          "cannot carry unescaped (0x%02x); pass --cmdline",
                          path, c);
            rc = SGN_ERR_FORMAT;
        }
    }
    if (rc == SGN_OK && n >= cap) {
        sgn_set_error(SGN_ERR_NOSPACE, "volume label too long");
        rc = SGN_ERR_NOSPACE;
    }
    if (rc == SGN_OK) {
        memcpy(out, label, n);
        out[n] = '\0';
    }
    free(vol);
    close(fd);
    return rc;
}
