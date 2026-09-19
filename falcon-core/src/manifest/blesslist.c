/* The loader-to-appliance bless list. See falcon-core/src/manifest/
 * blesslist.h. */

#include <manifest/blesslist.h>
#include <manifest/manifest.h>

#include <lib/string.h>

static size_t slen(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Does [s, s+n) equal word, ASCII case folded? */
static int same(const char *s, size_t n, const char *word)
{
    size_t i;

    if (slen(word) != n)
        return 0;
    for (i = 0; i < n; i++) {
        if (lower(s[i]) != word[i])
            return 0;
    }
    return 1;
}

int blesslist_path_ok(const char *path)
{
    size_t n, i, start = 0;

    if (path == (const char *)0)
        return 0;
    n = slen(path);
    if (n == 0 || n >= MANIFEST_PATH_MAX || path[0] == '/' ||
        path[n - 1u] == '/')
        return 0;
    for (i = 0; i < n; i++) {
        char c = path[i];

        if (c < 0x20 || c > 0x7e || c == '#' || c == '\\')
            return 0;
    }
    /* Components: none empty, none "." or "..", the first not ours. */
    for (i = 0; i <= n; i++) {
        size_t cl;

        if (i < n && path[i] != '/')
            continue;
        cl = i - start;
        if (cl == 0 || same(path + start, cl, ".") ||
            same(path + start, cl, ".."))
            return 0;
        if (start == 0 && i < n && same(path, cl, "falcon"))
            return 0;
        start = i + 1u;
    }
    return n > 4u && (same(path + n - 4u, 4, ".iso") ||
                      same(path + n - 4u, 4, ".img") ||
                      same(path + n - 4u, 4, ".efi"));
}

int blesslist_add(char *buf, size_t cap, size_t *len, const char *path)
{
    size_t n;

    if (buf == (char *)0 || len == (size_t *)0 || path == (const char *)0)
        return BLESSLIST_ERR_BAD_ARG;
    if (!blesslist_path_ok(path))
        return BLESSLIST_ERR_PATH;
    n = slen(path);
    if (*len > cap || cap - *len < n + 1u)
        return BLESSLIST_ERR_NOSPACE;
    memcpy(buf + *len, path, n);
    buf[*len + n] = '\n';
    *len += n + 1u;
    return BLESSLIST_OK;
}

int blesslist_next(const char *buf, size_t len, size_t *pos, char *out,
                   size_t cap)
{
    size_t start, n;

    if (buf == (const char *)0 || pos == (size_t *)0 ||
        out == (char *)0 || cap == 0)
        return BLESSLIST_ERR_BAD_ARG;
    out[0] = '\0';
    if (*pos >= len)
        return 0;
    start = *pos;
    while (*pos < len && buf[*pos] != '\n')
        (*pos)++;
    n = *pos - start;
    if (*pos < len)
        (*pos)++;
    if (n >= cap)
        return BLESSLIST_ERR_PATH;
    memcpy(out, buf + start, n);
    out[n] = '\0';
    return blesslist_path_ok(out) ? 1 : BLESSLIST_ERR_PATH;
}
