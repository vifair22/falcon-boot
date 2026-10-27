/* falcon-boot mkstick: the drives interactive `create` offers. drives.h has
 * the rules. */

#include <mkstick/drives.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <common/util.h>

/* Everything stacked on one disk: the disk, its partitions and their
 * holders, by kernel name. */
#define DRV_MAX_NAMES 64u

typedef struct {
    char   name[DRV_MAX_NAMES][40];
    size_t n;
} name_set;

void drv_env_default(drv_env *env)
{
    env->sys = "/sys";
    env->dev = "/dev";
    env->mounts = "/proc/mounts";
    env->swaps = "/proc/swaps";
}

static int is_skipped(const char *name)
{
    static const char *const skip_prefix[] = {
        "loop", "ram", "zram", "sr", "fd", "dm-", "md", "nbd"
    };
    size_t i;

    if (name[0] == '.')
        return 1;
    for (i = 0; i < sizeof skip_prefix / sizeof skip_prefix[0]; i++) {
        if (strncmp(name, skip_prefix[i], strlen(skip_prefix[i])) == 0)
            return 1;
    }
    return 0;
}

/* The first line of a sysfs attribute, trailing blanks dropped; empty when
 * it cannot be read. */
static void read_attr(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "re");
    size_t len;

    out[0] = '\0';
    if (f == NULL)
        return;
    if (fgets(out, (int)cap, f) == NULL)
        out[0] = '\0';
    (void)fclose(f);
    len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == ' '))
        out[--len] = '\0';
}

static void read_attr_at(const char *dir, const char *leaf, char *out,
                         size_t cap)
{
    char path[MKS_PATH_MAX];

    out[0] = '\0';
    if (snprintf(path, sizeof path, "%s/%s", dir, leaf) < (int)sizeof path)
        read_attr(path, out, cap);
}

/* <dir>/size, in 512-byte sectors whatever the disk's own sector size. */
static uint64_t read_size(const char *dir)
{
    char path[MKS_PATH_MAX];
    char text[32];

    if (snprintf(path, sizeof path, "%s/size", dir) >= (int)sizeof path)
        return 0;
    read_attr(path, text, sizeof text);
    return (uint64_t)strtoull(text, NULL, 10) * 512u;
}

static int set_has(const name_set *s, const char *name)
{
    size_t i;

    for (i = 0; i < s->n; i++) {
        if (strcmp(s->name[i], name) == 0)
            return 1;
    }
    return 0;
}

static void set_add(name_set *s, const char *name)
{
    if (s->n < DRV_MAX_NAMES && !set_has(s, name) &&
        strlen(name) < sizeof s->name[0])
        (void)snprintf(s->name[s->n++], sizeof s->name[0], "%s", name);
}

/* What holds <dir> open (its holders/), and what holds that, into s. The
 * first holder found is kept in first for the reason shown. */
static void add_holders(const drv_env *env, const char *dir, name_set *s,
                        char *first, size_t cap, unsigned depth)
{
    char path[MKS_PATH_MAX];
    struct dirent *e;
    DIR *d;

    if (depth > 4u ||
        snprintf(path, sizeof path, "%s/holders", dir) >= (int)sizeof path)
        return;
    d = opendir(path);
    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        char next[MKS_PATH_MAX];

        if (e->d_name[0] == '.' || set_has(s, e->d_name))
            continue;
        set_add(s, e->d_name);
        if (first[0] == '\0' && strlen(e->d_name) < cap)
            memcpy(first, e->d_name, strlen(e->d_name) + 1u);
        if (snprintf(next, sizeof next, "%s/block/%s", env->sys,
                     e->d_name) < (int)sizeof next)
            add_holders(env, next, s, first, cap, depth + 1u);
    }
    (void)closedir(d);
}

/* The kernel name a mount or swap source is: /dev/sdb1 is sdb1, and
 * /dev/mapper/vg-root is the dm-N it links to. */
static void source_name(const char *src, char *out, size_t cap)
{
    char real[MKS_PATH_MAX];

    if (realpath(src, real) != NULL)
        (void)snprintf(out, cap, "%s", util_basename(real));
    else
        (void)snprintf(out, cap, "%s", util_basename(src));
}

static void check_mounts(const drv_env *env, const name_set *s,
                         drv_entry *e)
{
    char line[8192];
    FILE *f = fopen(env->mounts, "re");

    if (f == NULL)
        return;
    while (fgets(line, sizeof line, f) != NULL) {
        char src[MKS_PATH_MAX];
        char dst[MKS_PATH_MAX];
        char name[64];

        if (sscanf(line, "%4095s %4095s", src, dst) != 2 ||
            strncmp(src, "/dev/", 5) != 0)
            continue;
        source_name(src, name, sizeof name);
        if (!set_has(s, name))
            continue;
        if (strcmp(dst, "/") == 0)
            e->system = 1;
        if (e->busy[0] == '\0')
            (void)snprintf(e->busy, sizeof e->busy, "mounted at %.140s",
                           dst);
    }
    (void)fclose(f);
}

static void check_swaps(const drv_env *env, const name_set *s, drv_entry *e)
{
    char line[1024];
    FILE *f = fopen(env->swaps, "re");

    if (f == NULL)
        return;
    while (fgets(line, sizeof line, f) != NULL) {
        char src[MKS_PATH_MAX];
        char name[64];

        if (sscanf(line, "%4095s", src) != 1 ||
            strncmp(src, "/dev/", 5) != 0)
            continue;
        source_name(src, name, sizeof name);
        if (set_has(s, name) && e->busy[0] == '\0')
            (void)snprintf(e->busy, sizeof e->busy, "used as swap");
    }
    (void)fclose(f);
}

/* The partitions sysfs lists under the disk: <dir>/<name>N with a
 * `partition` attribute. */
static void read_parts(const char *dir, drv_entry *e, name_set *s)
{
    struct dirent *de;
    DIR *d = opendir(dir);

    if (d == NULL)
        return;
    while ((de = readdir(d)) != NULL) {
        char pdir[MKS_PATH_MAX];
        char attr[MKS_PATH_MAX];

        if (strncmp(de->d_name, e->name, strlen(e->name)) != 0 ||
            strlen(de->d_name) >= sizeof e->part[0])
            continue;
        if (snprintf(pdir, sizeof pdir, "%s/%s", dir, de->d_name) >=
                (int)sizeof pdir ||
            snprintf(attr, sizeof attr, "%s/partition", pdir) >=
                (int)sizeof attr ||
            !util_is_file(attr))
            continue;
        set_add(s, de->d_name);
        if (e->n_parts < DRV_MAX_PARTS) {
            /* Its length was checked against part[] above. */
            memcpy(e->part[e->n_parts], de->d_name,
                   strlen(de->d_name) + 1u);
            e->part_size[e->n_parts] = read_size(pdir);
            e->n_parts++;
        }
    }
    (void)closedir(d);
}

/* Partitions in the kernel's order: sdb2 before sdb10. */
static void sort_parts(drv_entry *e)
{
    unsigned i;
    unsigned j;
    size_t base = strlen(e->name);

    for (i = 1; i < e->n_parts; i++) {
        for (j = i; j > 0; j--) {
            const char *a = e->part[j - 1u] + base;
            const char *b = e->part[j] + base;
            char tn[40];
            uint64_t ts;

            while (*a != '\0' && (*a < '0' || *a > '9'))
                a++;
            while (*b != '\0' && (*b < '0' || *b > '9'))
                b++;
            if (strtoul(a, NULL, 10) <= strtoul(b, NULL, 10))
                break;
            memcpy(tn, e->part[j], sizeof tn);
            memcpy(e->part[j], e->part[j - 1u], sizeof tn);
            memcpy(e->part[j - 1u], tn, sizeof tn);
            ts = e->part_size[j];
            e->part_size[j] = e->part_size[j - 1u];
            e->part_size[j - 1u] = ts;
        }
    }
}

static void read_drive(const drv_env *env, const char *name, drv_entry *e)
{
    char dir[MKS_PATH_MAX];
    char real[MKS_PATH_MAX];
    char vendor[24];
    char model[96];
    char holder[40] = "";
    name_set s;
    unsigned i;

    memset(e, 0, sizeof *e);
    memset(&s, 0, sizeof s);
    (void)snprintf(e->name, sizeof e->name, "%s", name);
    (void)snprintf(e->path, sizeof e->path, "%s/%s", env->dev, name);
    (void)snprintf(dir, sizeof dir, "%s/block/%s", env->sys, name);
    e->size = read_size(dir);

    read_attr_at(dir, "removable", vendor, sizeof vendor);
    e->removable = strcmp(vendor, "1") == 0;
    /* <sys>/block/sdb links into the device tree, through the USB host
     * controller for a USB disk. */
    e->usb = realpath(dir, real) != NULL && strstr(real, "/usb") != NULL;

    read_attr_at(dir, "device/vendor", vendor, sizeof vendor);
    read_attr_at(dir, "device/model", model, sizeof model);
    if (vendor[0] != '\0' && model[0] != '\0')
        (void)snprintf(e->model, sizeof e->model, "%s %s", vendor, model);
    else
        (void)snprintf(e->model, sizeof e->model, "%s",
                       model[0] != '\0' ? model : vendor);

    set_add(&s, name);
    read_parts(dir, e, &s);
    sort_parts(e);
    add_holders(env, dir, &s, holder, sizeof holder, 0);
    for (i = 0; i < e->n_parts; i++) {
        char pdir[MKS_PATH_MAX];

        if (snprintf(pdir, sizeof pdir, "%s/%s", dir, e->part[i]) <
                (int)sizeof pdir)
            add_holders(env, pdir, &s, holder, sizeof holder, 0);
    }

    check_mounts(env, &s, e);
    check_swaps(env, &s, e);
    if (e->busy[0] == '\0' && holder[0] != '\0')
        (void)snprintf(e->busy, sizeof e->busy, "held by %s", holder);
    if (e->system)
        (void)snprintf(e->busy, sizeof e->busy,
                       "this machine's system disk");
}

static int by_kind_then_name(const void *pa, const void *pb)
{
    const drv_entry *a = pa;
    const drv_entry *b = pb;
    int ra = a->removable || a->usb;
    int rb = b->removable || b->usb;

    if (ra != rb)
        return rb - ra;
    return strcmp(a->name, b->name);
}

cli_status drv_list(const drv_env *env, drv_entry *out, size_t cap,
                    size_t *n)
{
    char path[MKS_PATH_MAX];
    struct dirent *e;
    DIR *d;

    *n = 0;
    (void)snprintf(path, sizeof path, "%s/block", env->sys);
    d = opendir(path);
    if (d == NULL)
        return set_error_errno(CLI_ERR_IO, "listing %s", path);
    while ((e = readdir(d)) != NULL && *n < cap) {
        if (is_skipped(e->d_name) || strlen(e->d_name) >= sizeof out->name)
            continue;
        read_drive(env, e->d_name, &out[*n]);
        /* An empty card reader slot is a disk of size 0. */
        if (out[*n].size != 0)
            (*n)++;
    }
    (void)closedir(d);
    qsort(out, *n, sizeof *out, by_kind_then_name);
    return CLI_OK;
}
