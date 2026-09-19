/* The hooked-boot cmdline. See falcon-core/src/media/hookcmd.h. */

#include <media/hookcmd.h>

#include <media/cpio.h>
#include <lib/string.h>

/* One nudge: when the cmdline carries `when` as a whole word, `add` is
 * appended unless an argument with add's key is already there. */
typedef struct {
    const char *when;
    const char *add;
} nudge;

static const nudge g_nudges[] = {
    /* Debian live-boot (Debian, Kali, Clonezilla, Tails): its device scan
     * excludes dm-* by name, and live-media= names a device it mounts as
     * is. Measured on debian-live 13.7 (falcon-hook/tools/run-hook-rig.sh). */
    { "boot=live", "live-media=" HOOKCMD_DEVICE },
};

static size_t slen(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

/* The length of an argument's key: up to its '=', or the whole word. */
static size_t key_len(const char *w, size_t n)
{
    size_t k = 0;

    while (k < n && w[k] != '=')
        k++;
    return k;
}

/* Iterates the words of s: *pos is advanced past the next word, whose start
 * and length are returned. 0 at the end. */
static int next_word(const char *s, size_t *pos, const char **w, size_t *wl)
{
    size_t i = *pos;

    while (s[i] == ' ' || s[i] == '\t')
        i++;
    if (s[i] == '\0') {
        *pos = i;
        return 0;
    }
    *w = s + i;
    while (s[i] != '\0' && s[i] != ' ' && s[i] != '\t')
        i++;
    *wl = (size_t)(s + i - *w);
    *pos = i;
    return 1;
}

static int has_word(const char *s, const char *word)
{
    size_t pos = 0, wl, n = slen(word);
    const char *w;

    while (next_word(s, &pos, &w, &wl)) {
        if (wl == n && memcmp(w, word, n) == 0)
            return 1;
    }
    return 0;
}

static int has_key(const char *s, const char *arg)
{
    size_t pos = 0, wl, k = key_len(arg, slen(arg));
    const char *w;

    while (next_word(s, &pos, &w, &wl)) {
        if (key_len(w, wl) == k && memcmp(w, arg, k) == 0)
            return 1;
    }
    return 0;
}

/* Appends a word, space-separated, keeping out NUL-terminated. */
static int put(char *out, size_t cap, size_t *o, const char *w, size_t n)
{
    if (*o + (*o ? 1u : 0u) + n + 1u > cap)
        return HOOKCMD_ERR_NOSPACE;
    if (*o)
        out[(*o)++] = ' ';
    memcpy(out + *o, w, n);
    *o += n;
    out[*o] = '\0';
    return HOOKCMD_OK;
}

/* The additions: the nudges the entry calls for, then the hook. */
static int put_additions(const char *base, char *out, size_t cap, size_t *o)
{
    size_t i;
    int rc;

    for (i = 0; i < sizeof g_nudges / sizeof g_nudges[0]; i++) {
        if (has_word(base, g_nudges[i].when) &&
            !has_key(base, g_nudges[i].add)) {
            rc = put(out, cap, o, g_nudges[i].add, slen(g_nudges[i].add));
            if (rc != HOOKCMD_OK)
                return rc;
        }
    }
    return put(out, cap, o, "rdinit=" HOOKCMD_HOOK_PATH,
               sizeof("rdinit=" HOOKCMD_HOOK_PATH) - 1u);
}

int hookcmd_build(const char *base, char *out, size_t cap)
{
    static const char rdinit[] = "rdinit=";
    static const char finit[] = "falcon.init=";
    const size_t rl = sizeof rdinit - 1u, fl = sizeof finit - 1u;
    size_t pos = 0, o = 0, wl;
    const char *w;
    int rc, added = 0;

    if (base == (const char *)0 || out == (char *)0 || cap == 0)
        return HOOKCMD_ERR_BAD_ARG;
    out[0] = '\0';

    /* The entry's own words, an rdinit= renamed to falcon.init= so the hook
     * can hand over to what the distro asked for. The additions go before a
     * "--": the kernel passes everything after one to init, where rdinit=
     * would mean nothing (Mint ends its cmdline with "quiet splash --"). */
    while (next_word(base, &pos, &w, &wl)) {
        if (!added && wl == 2 && w[0] == '-' && w[1] == '-') {
            rc = put_additions(base, out, cap, &o);
            if (rc != HOOKCMD_OK)
                return rc;
            added = 1;
        } else if (!added && wl > rl && memcmp(w, rdinit, rl) == 0) {
            if (o + (o ? 1u : 0u) + fl + (wl - rl) + 1u > cap)
                return HOOKCMD_ERR_NOSPACE;
            rc = put(out, cap, &o, finit, fl);
            if (rc != HOOKCMD_OK)
                return rc;
            memcpy(out + o, w + rl, wl - rl);
            o += wl - rl;
            out[o] = '\0';
            continue;
        }
        rc = put(out, cap, &o, w, wl);
        if (rc != HOOKCMD_OK)
            return rc;
    }
    return added ? HOOKCMD_OK : put_additions(base, out, cap, &o);
}

/* Directory, then the executable: the kernel's unpacker creates only what
 * an archive names. */
static void archive_entries(cpio_entry *e, const void *bin, size_t bin_len)
{
    e[0].path = "falcon";
    e[0].data = (const void *)0;
    e[0].len = 0;
    e[0].mode = CPIO_MODE_DIR;
    e[1].path = "falcon/hook";
    e[1].data = bin;
    e[1].len = bin_len;
    e[1].mode = 0100755u;
}

size_t hookcmd_archive_size(size_t bin_len)
{
    /* cpio_size only measures, but it refuses data NULL with a length. */
    static const unsigned char any = 0;
    cpio_entry e[2];
    size_t n = 0;

    archive_entries(e, &any, bin_len);
    if (cpio_size(e, 2, &n) != CPIO_OK)
        return 0;
    return n;
}

int hookcmd_archive(const void *bin, size_t bin_len, void *buf, size_t cap,
                    size_t *len)
{
    cpio_entry e[2];
    int rc;

    if (bin == (const void *)0 || bin_len == 0 || buf == (void *)0 ||
        len == (size_t *)0)
        return HOOKCMD_ERR_BAD_ARG;
    archive_entries(e, bin, bin_len);
    rc = cpio_write(e, 2, buf, cap, len);
    if (rc == CPIO_ERR_NOSPACE)
        return HOOKCMD_ERR_NOSPACE;
    return rc == CPIO_OK ? HOOKCMD_OK : HOOKCMD_ERR_BAD_ARG;
}
