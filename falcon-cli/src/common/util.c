#include <common/util.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

cli_status util_parse_size(const char *text, uint64_t *out)
{
    uint64_t value = 0;
    const char *p;
    unsigned shift = 0;

    if (text == NULL || text[0] == '\0')
        return set_error(CLI_ERR_USAGE, "empty size");

    for (p = text; *p >= '0' && *p <= '9'; p++) {
        uint64_t digit = (uint64_t)(*p - '0');

        if (value > (UINT64_MAX - digit) / 10u)
            return set_error(CLI_ERR_RANGE, "size '%s' overflows", text);
        value = value * 10u + digit;
    }
    if (p == text)
        return set_error(CLI_ERR_USAGE, "size '%s' does not start with a "
                         "number", text);

    switch (*p) {
    case '\0': shift = 0;  break;
    case 'k': case 'K': shift = 10; p++; break;
    case 'm': case 'M': shift = 20; p++; break;
    case 'g': case 'G': shift = 30; p++; break;
    case 't': case 'T': shift = 40; p++; break;
    default:
        return set_error(CLI_ERR_USAGE, "size '%s' has an unknown unit "
                         "(use K, M, G or T)", text);
    }
    /* Accept the IEC spellings too: 64MiB and 64M are the same number. */
    if (*p == 'i' || *p == 'I')
        p++;
    if (*p == 'b' || *p == 'B')
        p++;
    if (*p != '\0')
        return set_error(CLI_ERR_USAGE, "trailing junk in size '%s'", text);

    if (shift != 0 && value > (UINT64_MAX >> shift))
        return set_error(CLI_ERR_RANGE, "size '%s' overflows", text);

    *out = value << shift;
    return CLI_OK;
}

const char *util_format_size(uint64_t bytes, char *buf, size_t cap)
{
    static const char *const unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    size_t i = 0;
    double v = (double)bytes;

    while (v >= 1024.0 && i + 1 < sizeof unit / sizeof unit[0]) {
        v /= 1024.0;
        i++;
    }
    if (i == 0)
        (void)snprintf(buf, cap, "%llu B", (unsigned long long)bytes);
    else
        (void)snprintf(buf, cap, "%.1f %s", v, unit[i]);
    return buf;
}

cli_status util_copy_file(const char *src, const char *dst, mode_t mode)
{
    int in = -1;
    int out = -1;
    cli_status rc = CLI_OK;

    in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s", src);

    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (out < 0) {
        rc = set_error_errno(CLI_ERR_IO, "creating %s", dst);
        goto done;
    }

    for (;;) {
        char buf[65536];
        ssize_t n = read(in, buf, sizeof buf);
        const char *p = buf;
        size_t left;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = set_error_errno(CLI_ERR_IO, "reading %s", src);
            goto done;
        }
        if (n == 0)
            break;
        left = (size_t)n;
        while (left > 0) {
            ssize_t w = write(out, p, left);

            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = set_error_errno(CLI_ERR_IO, "writing %s", dst);
                goto done;
            }
            p += (size_t)w;
            left -= (size_t)w;
        }
    }

    /* The mode is set explicitly rather than left to open()'s umask dance:
     * the private key's 0600 is a security requirement (DESIGN Section 7.5),
     * not a default. */
    if (fchmod(out, mode) != 0) {
        rc = set_error_errno(CLI_ERR_IO, "chmod %s", dst);
        goto done;
    }
    if (fsync(out) != 0 && errno != EINVAL) {
        rc = set_error_errno(CLI_ERR_IO, "syncing %s", dst);
        goto done;
    }

done:
    if (in >= 0)
        (void)close(in);
    if (out >= 0)
        (void)close(out);
    return rc;
}

cli_status util_read_file(const char *path, void **buf, size_t *len,
                          size_t max)
{
    int fd;
    struct stat st;
    unsigned char *mem;
    size_t got = 0;
    cli_status rc = CLI_OK;

    *buf = NULL;
    *len = 0;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return set_error_errno(CLI_ERR_IO, "opening %s", path);
    if (fstat(fd, &st) != 0) {
        rc = set_error_errno(CLI_ERR_IO, "stat %s", path);
        goto done;
    }
    if (!S_ISREG(st.st_mode)) {
        rc = set_error(CLI_ERR_INVALID, "%s is not a regular file", path);
        goto done;
    }
    if ((uint64_t)st.st_size > (uint64_t)max) {
        rc = set_error(CLI_ERR_RANGE, "%s is %llu bytes, cap is %zu",
                       path, (unsigned long long)st.st_size, max);
        goto done;
    }

    mem = malloc((size_t)st.st_size + 1u);
    if (mem == NULL) {
        rc = set_error(CLI_ERR_NOMEM, "allocating %llu bytes for %s",
                       (unsigned long long)st.st_size + 1u, path);
        goto done;
    }

    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, mem + got, (size_t)st.st_size - got);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = set_error_errno(CLI_ERR_IO, "reading %s", path);
            free(mem);
            goto done;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    mem[got] = '\0';
    *buf = mem;
    *len = got;

done:
    (void)close(fd);
    return rc;
}

cli_status util_write_file(const char *path, const void *buf, size_t len,
                           mode_t mode)
{
    int fd;
    const char *p = (const char *)buf;
    size_t left = len;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return set_error_errno(CLI_ERR_IO, "creating %s", path);

    while (left > 0) {
        ssize_t n = write(fd, p, left);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            return set_error_errno(CLI_ERR_IO, "writing %s", path);
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    if (fchmod(fd, mode) != 0) {
        (void)close(fd);
        return set_error_errno(CLI_ERR_IO, "chmod %s", path);
    }
    if (close(fd) != 0)
        return set_error_errno(CLI_ERR_IO, "closing %s", path);
    return CLI_OK;
}

cli_status util_mkdir_p(const char *path, mode_t mode)
{
    char work[4096];
    size_t i;

    if (strlen(path) >= sizeof work)
        return set_error(CLI_ERR_RANGE, "path too long: %s", path);
    (void)snprintf(work, sizeof work, "%s", path);

    for (i = 1; work[i] != '\0'; i++) {
        if (work[i] != '/')
            continue;
        work[i] = '\0';
        if (mkdir(work, mode) != 0 && errno != EEXIST)
            return set_error_errno(CLI_ERR_IO, "mkdir %s", work);
        work[i] = '/';
    }
    if (mkdir(work, mode) != 0 && errno != EEXIST)
        return set_error_errno(CLI_ERR_IO, "mkdir %s", work);
    return CLI_OK;
}

cli_status util_shred(const char *path)
{
    int fd;
    struct stat st;
    off_t done = 0;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT)
            return CLI_OK;
        return set_error_errno(CLI_ERR_IO, "opening %s to wipe it", path);
    }
    if (fstat(fd, &st) != 0) {
        (void)close(fd);
        return set_error_errno(CLI_ERR_IO, "stat %s", path);
    }

    while (done < st.st_size) {
        char zero[4096];
        size_t want = sizeof zero;
        ssize_t n;

        memset(zero, 0, sizeof zero);
        if ((off_t)want > st.st_size - done)
            want = (size_t)(st.st_size - done);
        n = write(fd, zero, want);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            return set_error_errno(CLI_ERR_IO, "wiping %s", path);
        }
        done += n;
    }
    (void)fsync(fd);
    (void)close(fd);
    if (unlink(path) != 0)
        return set_error_errno(CLI_ERR_IO, "unlinking %s", path);
    return CLI_OK;
}

cli_status util_join(char *dst, size_t cap, const char *a, const char *b)
{
    size_t alen = strlen(a);
    int n;

    while (alen > 1u && a[alen - 1u] == '/')
        alen--;
    while (*b == '/')
        b++;

    n = snprintf(dst, cap, "%.*s/%s", (int)alen, a, b);
    if (n < 0 || (size_t)n >= cap)
        return set_error(CLI_ERR_RANGE, "path too long: %s/%s", a, b);
    return CLI_OK;
}

const char *util_basename(const char *path)
{
    const char *slash = strrchr(path, '/');

    return (slash != NULL) ? slash + 1 : path;
}

int util_is_file(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int util_is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int util_is_block(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISBLK(st.st_mode);
}
