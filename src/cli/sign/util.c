/* falcon-boot-sign: error buffer, logging, subprocess exec and file I/O.
 *
 * Nothing here is specific to blessing; it is the host-tooling floor the
 * repo's CLI conventions ask for (thread-local error buffer set by the
 * deepest frame, log levels split across the two streams, explicit cleanup
 * before every error return). When a second host tool arrives this is what
 * moves to src/cli/common.
 */

#include <cli/sign/sign.h>

#include <crypto/sha256.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------ errors */

#define ERR_CAP 512u

static _Thread_local char g_err[ERR_CAP];
static _Thread_local int  g_err_code;

void sgn_set_error(int code, const char *fmt, ...)
{
    va_list ap;

    g_err_code = code;
    va_start(ap, fmt);
    (void)vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}

void sgn_set_error_errno(int code, const char *fmt, ...)
{
    int saved = errno;
    va_list ap;
    int n;

    g_err_code = code;
    va_start(ap, fmt);
    n = vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof(g_err))
        (void)snprintf(g_err + n, sizeof(g_err) - (size_t)n, ": %s",
                       strerror(saved));
}

const char *sgn_error(void)
{
    return g_err[0] != '\0' ? g_err : "no error recorded";
}

int sgn_error_code(void)
{
    return g_err_code;
}

/* ------------------------------------------------------------ logging */

static sgn_level g_min_level = SGN_LOG_INFO;

void sgn_log_set_level(sgn_level min)
{
    g_min_level = min;
}

void sgn_log(sgn_level lvl, const char *fmt, ...)
{
    static const char *const names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    va_list ap;

    if (lvl < g_min_level)
        return;
    (void)fprintf(stderr, "%-5s ", names[lvl]);
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);
}

/* ------------------------------------------------------------ exec */

/* Common half of the runners: fork, point the child's stdout at out_fd and
 * its stdin at in_fd (either may be -1 to leave that descriptor alone), exec,
 * and wait.
 *
 * execvp rather than a shell: the whole point is that no argument is ever
 * parsed by anything, so a path with a space, a quote or a semicolon in it
 * is just a path. */
static int run_common_io(char *const argv[], int out_fd, int in_fd)
{
    pid_t pid;
    int status = 0;

    if (argv == NULL || argv[0] == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "empty argv");
        return SGN_ERR_INTERNAL;
    }

    sgn_log(SGN_LOG_DEBUG, "exec: %s ...", argv[0]);
    pid = fork();
    if (pid < 0) {
        sgn_set_error_errno(SGN_ERR_EXEC, "fork for '%s'", argv[0]);
        return SGN_ERR_EXEC;
    }
    if (pid == 0) {
        if (out_fd >= 0) {
            if (dup2(out_fd, STDOUT_FILENO) < 0)
                _exit(127);
            if (out_fd != STDOUT_FILENO)
                (void)close(out_fd);
        }
        if (in_fd >= 0) {
            if (dup2(in_fd, STDIN_FILENO) < 0)
                _exit(127);
            if (in_fd != STDIN_FILENO)
                (void)close(in_fd);
        }
        execvp(argv[0], argv);
        (void)fprintf(stderr, "falcon-boot-sign: exec %s: %s\n", argv[0],
                      strerror(errno));
        _exit(127);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            sgn_set_error_errno(SGN_ERR_EXEC, "waitpid for '%s'", argv[0]);
            return SGN_ERR_EXEC;
        }
    }
    if (WIFSIGNALED(status)) {
        sgn_set_error(SGN_ERR_EXEC, "'%s' killed by signal %d", argv[0],
                      WTERMSIG(status));
        return SGN_ERR_EXEC;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        sgn_set_error(SGN_ERR_EXEC, "'%s' exited %d", argv[0],
                      WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return SGN_ERR_EXEC;
    }
    return SGN_OK;
}

int sgn_run(char *const argv[])
{
    int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    int rc;

    if (devnull < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "open /dev/null");
        return SGN_ERR_IO;
    }
    rc = run_common_io(argv, devnull, -1);
    (void)close(devnull);
    return rc;
}

int sgn_run_stdin(char *const argv[], const char *path)
{
    int devnull;
    int in_fd;
    int rc;

    in_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (in_fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "open '%s' for read", path);
        return SGN_ERR_IO;
    }
    devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "open /dev/null");
        (void)close(in_fd);
        return SGN_ERR_IO;
    }
    rc = run_common_io(argv, devnull, in_fd);
    (void)close(devnull);
    (void)close(in_fd);
    return rc;
}

int sgn_run_to_file(char *const argv[], const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int rc;

    if (fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "open '%s' for write", path);
        return SGN_ERR_IO;
    }
    rc = run_common_io(argv, fd, -1);
    if (close(fd) < 0 && rc == SGN_OK) {
        sgn_set_error_errno(SGN_ERR_IO, "close '%s'", path);
        rc = SGN_ERR_IO;
    }
    if (rc != SGN_OK)
        (void)unlink(path);
    return rc;
}

int sgn_run_capture(char *const argv[], char **out, size_t *len, size_t max)
{
    int fds[2];
    pid_t pid;
    char *buf = NULL;
    size_t cap = 65536u;
    size_t used = 0;
    int status = 0;
    int rc = SGN_OK;

    if (out == NULL || len == NULL || argv == NULL || argv[0] == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "sgn_run_capture: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    *out = NULL;
    *len = 0;

    if (pipe(fds) < 0) {
        sgn_set_error_errno(SGN_ERR_EXEC, "pipe for '%s'", argv[0]);
        return SGN_ERR_EXEC;
    }
    sgn_log(SGN_LOG_DEBUG, "exec (capture): %s ...", argv[0]);
    pid = fork();
    if (pid < 0) {
        sgn_set_error_errno(SGN_ERR_EXEC, "fork for '%s'", argv[0]);
        (void)close(fds[0]);
        (void)close(fds[1]);
        return SGN_ERR_EXEC;
    }
    if (pid == 0) {
        (void)close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        (void)close(fds[1]);
        execvp(argv[0], argv);
        (void)fprintf(stderr, "falcon-boot-sign: exec %s: %s\n", argv[0],
                      strerror(errno));
        _exit(127);
    }
    (void)close(fds[1]);

    buf = malloc(cap);
    if (buf == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory capturing '%s'", argv[0]);
        rc = SGN_ERR_IO;
    }
    /* Drain the pipe to the end even after an error: a child left writing
     * into a full pipe would never exit and waitpid would never return. */
    while (rc == SGN_OK || buf == NULL) {
        ssize_t n;
        char sink[65536];
        char *dst = (buf != NULL) ? buf + used : sink;
        size_t room = (buf != NULL) ? cap - used : sizeof(sink);

        n = read(fds[0], dst, room);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (rc == SGN_OK) {
                sgn_set_error_errno(SGN_ERR_EXEC, "read from '%s'", argv[0]);
                rc = SGN_ERR_EXEC;
            }
            break;
        }
        if (n == 0)
            break;
        if (buf == NULL)
            continue;
        used += (size_t)n;
        if (used > max) {
            sgn_set_error(SGN_ERR_NOSPACE,
                          "'%s' produced more than %zu bytes of output",
                          argv[0], max);
            rc = SGN_ERR_NOSPACE;
            free(buf);
            buf = NULL;
            continue;
        }
        if (used + 1u >= cap) {
            char *bigger;

            cap *= 2u;
            bigger = realloc(buf, cap);
            if (bigger == NULL) {
                sgn_set_error(SGN_ERR_IO, "out of memory capturing '%s'",
                              argv[0]);
                rc = SGN_ERR_IO;
                free(buf);
                buf = NULL;
                continue;
            }
            buf = bigger;
        }
    }
    (void)close(fds[0]);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            sgn_set_error_errno(SGN_ERR_EXEC, "waitpid for '%s'", argv[0]);
            free(buf);
            return SGN_ERR_EXEC;
        }
    }
    if (rc != SGN_OK) {
        free(buf);
        return rc;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        sgn_set_error(SGN_ERR_EXEC, "'%s' exited %d", argv[0],
                      WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        free(buf);
        return SGN_ERR_EXEC;
    }

    buf[used] = '\0';
    *out = buf;
    *len = used;
    return SGN_OK;
}

int sgn_have_program(const char *name)
{
    const char *path = getenv("PATH");
    const char *p;

    if (name == NULL)
        return 0;
    if (strchr(name, '/') != NULL)
        return access(name, X_OK) == 0;
    if (path == NULL)
        path = "/usr/bin:/bin";
    for (p = path; *p != '\0';) {
        char probe[PATH_MAX];
        const char *end = strchr(p, ':');
        size_t n = (end != NULL) ? (size_t)(end - p) : strlen(p);
        size_t name_len = strlen(name);

        /* dir + '/' + name + NUL must fit: the direct bound on n keeps the
         * index provably inside the buffer even for a hostile PATH, and the
         * subtraction cannot underflow once n is bounded. */
        if (n > 0 && n < sizeof(probe) - 1u &&
            name_len + 2u <= sizeof(probe) - n) {
            memcpy(probe, p, n);
            probe[n] = '/';
            memcpy(probe + n + 1u, name, name_len + 1u);
            if (access(probe, X_OK) == 0)
                return 1;
        }
        if (end == NULL)
            break;
        p = end + 1;
    }
    return 0;
}

/* ------------------------------------------------------------ files */

int sgn_file_exists(const char *path)
{
    struct stat st;

    return path != NULL && stat(path, &st) == 0;
}

int sgn_file_size(const char *path, uint64_t *out)
{
    struct stat st;

    if (path == NULL || out == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "sgn_file_size: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    if (stat(path, &st) != 0) {
        sgn_set_error_errno(SGN_ERR_IO, "stat '%s'", path);
        return SGN_ERR_IO;
    }
    if (!S_ISREG(st.st_mode)) {
        sgn_set_error(SGN_ERR_IO, "'%s' is not a regular file", path);
        return SGN_ERR_IO;
    }
    *out = (uint64_t)st.st_size;
    return SGN_OK;
}

int sgn_pad_file(const char *path, uint64_t len)
{
    uint64_t have = 0;
    int rc;
    int fd;

    rc = sgn_file_size(path, &have);
    if (rc != SGN_OK)
        return rc;
    if (have == len)
        return SGN_OK;
    if (have > len) {
        sgn_set_error(SGN_ERR_INTERNAL,
                      "refusing to truncate '%s' from %llu to %llu bytes",
                      path, (unsigned long long)have,
                      (unsigned long long)len);
        return SGN_ERR_INTERNAL;
    }

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "open '%s' to pad", path);
        return SGN_ERR_IO;
    }
    /* ftruncate past the end, which POSIX defines as reading back zeroes.
     * That leaves a sparse tail on a filesystem that supports one, so the
     * padding costs no writes here; the loader serves the range and reads the
     * zeroes exactly as if they had been written. */
    if (ftruncate(fd, (off_t)len) < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "pad '%s' to %llu bytes", path,
                            (unsigned long long)len);
        (void)close(fd);
        return SGN_ERR_IO;
    }
    if (fsync(fd) < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "fsync '%s'", path);
        (void)close(fd);
        return SGN_ERR_IO;
    }
    if (close(fd) < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "close '%s'", path);
        return SGN_ERR_IO;
    }
    return SGN_OK;
}

/* Bounded on purpose. Every tree this removes is one the tool built two
 * function calls ago and is at most two levels deep; a depth limit turns a
 * symlink loop or a path this code did not create into a refusal rather than
 * an unbounded walk, and the caller is never asking it to clean up anything
 * it did not make. */
#define RM_MAX_DEPTH 4

static int rm_rf_depth(const char *path, int depth)
{
    DIR *d;
    struct dirent *ent;
    struct stat st;
    int rc = SGN_OK;

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT)
            return SGN_OK;
        sgn_set_error_errno(SGN_ERR_IO, "lstat '%s'", path);
        return SGN_ERR_IO;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) != 0 && errno != ENOENT) {
            sgn_set_error_errno(SGN_ERR_IO, "unlink '%s'", path);
            return SGN_ERR_IO;
        }
        return SGN_OK;
    }
    if (depth >= RM_MAX_DEPTH) {
        sgn_set_error(SGN_ERR_IO,
                      "'%s' is deeper than this cleanup will descend", path);
        return SGN_ERR_IO;
    }

    d = opendir(path);
    if (d == NULL) {
        sgn_set_error_errno(SGN_ERR_IO, "opendir '%s'", path);
        return SGN_ERR_IO;
    }
    while ((ent = readdir(d)) != NULL) {
        char child[PATH_MAX];

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, ent->d_name) >=
            (int)sizeof(child)) {
            sgn_set_error(SGN_ERR_NOSPACE, "path too long under '%s'", path);
            rc = SGN_ERR_NOSPACE;
            break;
        }
        rc = rm_rf_depth(child, depth + 1);
        if (rc != SGN_OK)
            break;
    }
    (void)closedir(d);
    if (rc != SGN_OK)
        return rc;

    if (rmdir(path) != 0 && errno != ENOENT) {
        sgn_set_error_errno(SGN_ERR_IO, "rmdir '%s'", path);
        return SGN_ERR_IO;
    }
    return SGN_OK;
}

int sgn_rm_rf(const char *path)
{
    if (path == NULL || path[0] == '\0' || strcmp(path, "/") == 0) {
        sgn_set_error(SGN_ERR_INTERNAL, "sgn_rm_rf: refusing '%s'",
                      (path != NULL) ? path : "(null)");
        return SGN_ERR_INTERNAL;
    }
    return rm_rf_depth(path, 0);
}

/* ------------------------------------------------------------ paths */

const char *sgn_base_name(const char *p)
{
    const char *slash;

    if (p == NULL)
        return "";
    slash = strrchr(p, '/');
    return (slash != NULL) ? slash + 1 : p;
}

int sgn_field_copy(char *dst, size_t cap, const char *src, const char *what)
{
    size_t n;

    if (dst == NULL || src == NULL || cap == 0) {
        sgn_set_error(SGN_ERR_INTERNAL, "sgn_field_copy: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    n = strlen(src);
    if (n + 1u > cap) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "%s is %zu bytes, which does not fit the manifest's "
                      "%zu-byte field: shorten it or use --name",
                      what, n, cap - 1u);
        return SGN_ERR_NOSPACE;
    }
    memcpy(dst, src, n + 1u);
    return SGN_OK;
}

int sgn_derive_iso_path(const char *iso, const char *stick,
                        const char *override_path, char *out, size_t cap)
{
    char iso_real[PATH_MAX];
    char stick_real[PATH_MAX];
    size_t slen;

    if (iso == NULL || stick == NULL || out == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "sgn_derive_iso_path: bad arguments");
        return SGN_ERR_INTERNAL;
    }

    if (override_path != NULL) {
        const char *p = override_path;

        while (*p == '/')
            p++;
        if (snprintf(out, cap, "/%s", p) >= (int)cap) {
            sgn_set_error(SGN_ERR_NOSPACE, "--iso-path is too long");
            return SGN_ERR_NOSPACE;
        }
        return SGN_OK;
    }

    if (realpath(iso, iso_real) == NULL) {
        sgn_set_error_errno(SGN_ERR_IO, "realpath '%s'", iso);
        return SGN_ERR_IO;
    }
    if (realpath(stick, stick_real) == NULL) {
        sgn_set_error_errno(SGN_ERR_IO, "realpath '%s'", stick);
        return SGN_ERR_IO;
    }
    slen = strlen(stick_real);
    if (slen > 0 && stick_real[slen - 1u] == '/')
        slen--;

    if (strncmp(iso_real, stick_real, slen) == 0 && iso_real[slen] == '/') {
        if (snprintf(out, cap, "%s", iso_real + slen) >= (int)cap) {
            sgn_set_error(SGN_ERR_NOSPACE, "the ISO's path on p2 is too long");
            return SGN_ERR_NOSPACE;
        }
        return SGN_OK;
    }

    sgn_log(SGN_LOG_WARN,
            "the ISO is not under --stick, so its path on p2 is a guess: "
            "assuming /%s. Copy it there, or pass --iso-path.",
            sgn_base_name(iso));
    if (snprintf(out, cap, "/%s", sgn_base_name(iso)) >= (int)cap) {
        sgn_set_error(SGN_ERR_NOSPACE, "the ISO's name is too long");
        return SGN_ERR_NOSPACE;
    }
    return SGN_OK;
}

/* ------------------------------------------------------------ families */

typedef struct {
    family_id   id;
    const char *slug;
} family_slug;

/* These live here rather than in src/iso/family.h because nothing in the
 * loader needs them until the blessed boot path reads the manifest, at which
 * point this table is what that path should be taught from. */
static const family_slug g_slugs[] = {
    { FAMILY_UNKNOWN,       "unknown" },
    { FAMILY_DEBIAN_CASPER, "casper" },
    { FAMILY_DEBIAN_LIVE,   "debian-live" },
    { FAMILY_FEDORA_DRACUT, "fedora" },
    { FAMILY_ARCH,          "arch" },
    { FAMILY_SYSTEMRESCUE,  "systemrescue" },
    { FAMILY_MEMTEST,       "memtest" },
    { FAMILY_WINDOWS,       "windows" },
};

const char *sgn_family_slug(family_id id)
{
    size_t i;

    for (i = 0; i < sizeof(g_slugs) / sizeof(g_slugs[0]); i++) {
        if (g_slugs[i].id == id)
            return g_slugs[i].slug;
    }
    return "unknown";
}

int sgn_sha256_file(const char *path, uint8_t digest[32], uint64_t *size)
{
    /* One megabyte: large enough that a multi-gigabyte ISO is a few thousand
     * reads, small enough to stay off the stack (it is static, and the tool
     * is single-threaded by construction). */
    static uint8_t chunk[1u << 20];
    sha256_ctx ctx;
    uint64_t total = 0;
    int fd;

    if (path == NULL || digest == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "sgn_sha256_file: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "open '%s'", path);
        return SGN_ERR_IO;
    }
    sha256_init(&ctx);
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));

        if (n < 0) {
            if (errno == EINTR)
                continue;
            sgn_set_error_errno(SGN_ERR_IO, "read '%s'", path);
            (void)close(fd);
            return SGN_ERR_IO;
        }
        if (n == 0)
            break;
        sha256_update(&ctx, chunk, (size_t)n);
        total += (uint64_t)n;
    }
    (void)close(fd);
    sha256_final(&ctx, digest);
    if (size != NULL)
        *size = total;
    return SGN_OK;
}

void sgn_hex32(const uint8_t digest[32], char *out)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < 32u; i++) {
        out[i * 2u]      = hex[digest[i] >> 4];
        out[i * 2u + 1u] = hex[digest[i] & 0x0Fu];
    }
    out[64] = '\0';
}

int sgn_read_file(const char *path, char **out, size_t *len, size_t max)
{
    FILE *f;
    char *buf;
    size_t cap = 4096u;
    size_t used = 0;

    if (out == NULL || len == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "sgn_read_file: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    *out = NULL;
    *len = 0;
    f = fopen(path, "rb");
    if (f == NULL) {
        sgn_set_error_errno(SGN_ERR_IO, "open '%s'", path);
        return SGN_ERR_IO;
    }
    buf = malloc(cap);
    if (buf == NULL) {
        (void)fclose(f);
        sgn_set_error(SGN_ERR_IO, "out of memory reading '%s'", path);
        return SGN_ERR_IO;
    }
    for (;;) {
        size_t n = fread(buf + used, 1, cap - used - 1u, f);

        used += n;
        /* Stop the moment the stream reports EOF or error rather than
         * calling fread again on a stream in either state; the error check
         * after the loop turns the ferror case into a failure. */
        if (n == 0 || feof(f) || ferror(f))
            break;
        if (used > max) {
            free(buf);
            (void)fclose(f);
            sgn_set_error(SGN_ERR_NOSPACE, "'%s' is larger than %zu bytes",
                          path, max);
            return SGN_ERR_NOSPACE;
        }
        if (used + 1u >= cap) {
            char *bigger;

            cap *= 2u;
            bigger = realloc(buf, cap);
            if (bigger == NULL) {
                free(buf);
                (void)fclose(f);
                sgn_set_error(SGN_ERR_IO, "out of memory reading '%s'", path);
                return SGN_ERR_IO;
            }
            buf = bigger;
        }
    }
    if (ferror(f)) {
        free(buf);
        (void)fclose(f);
        sgn_set_error(SGN_ERR_IO, "read '%s' failed", path);
        return SGN_ERR_IO;
    }
    (void)fclose(f);
    buf[used] = '\0';
    *out = buf;
    *len = used;
    return SGN_OK;
}

int sgn_write_file_atomic(const char *path, const void *buf, size_t len)
{
    char tmp[PATH_MAX];
    int fd;
    const uint8_t *p = buf;
    size_t done = 0;

    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        sgn_set_error(SGN_ERR_NOSPACE, "path too long: '%s'", path);
        return SGN_ERR_NOSPACE;
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "open '%s' for write", tmp);
        return SGN_ERR_IO;
    }
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            sgn_set_error_errno(SGN_ERR_IO, "write '%s'", tmp);
            (void)close(fd);
            (void)unlink(tmp);
            return SGN_ERR_IO;
        }
        done += (size_t)n;
    }
    /* fsync before the rename: p2 is a removable stick that gets yanked, and
     * a manifest that survives the rename but not the data is the one state
     * the loader cannot tell from a truncated file. */
    if (fsync(fd) < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "fsync '%s'", tmp);
        (void)close(fd);
        (void)unlink(tmp);
        return SGN_ERR_IO;
    }
    if (close(fd) < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "close '%s'", tmp);
        (void)unlink(tmp);
        return SGN_ERR_IO;
    }
    if (rename(tmp, path) < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "rename '%s' to '%s'", tmp, path);
        (void)unlink(tmp);
        return SGN_ERR_IO;
    }
    return SGN_OK;
}

int sgn_mkdir_p(const char *path)
{
    char work[PATH_MAX];
    size_t i;
    size_t n;

    if (path == NULL || path[0] == '\0') {
        sgn_set_error(SGN_ERR_INTERNAL, "sgn_mkdir_p: empty path");
        return SGN_ERR_INTERNAL;
    }
    n = strlen(path);
    if (n + 1u > sizeof(work)) {
        sgn_set_error(SGN_ERR_NOSPACE, "path too long: '%s'", path);
        return SGN_ERR_NOSPACE;
    }
    memcpy(work, path, n + 1u);
    for (i = 1; i <= n; i++) {
        if (work[i] != '/' && work[i] != '\0')
            continue;
        work[i] = '\0';
        if (mkdir(work, 0755) < 0 && errno != EEXIST) {
            sgn_set_error_errno(SGN_ERR_IO, "mkdir '%s'", work);
            return SGN_ERR_IO;
        }
        if (i < n)
            work[i] = '/';
    }
    return SGN_OK;
}

/* ---- filesystem UUID of the device behind a path, without libblkid ---- */

/* Longest mount point in /proc/self/mountinfo that prefixes path, and the
 * device node mounted there. mountinfo field layout, per
 * Documentation/filesystems/proc.rst: fields 5 is the mount point, and the
 * source device follows the " - <fstype> " separator. */
static int mount_source_for(const char *path, char *dev, size_t dev_cap)
{
    FILE *f = fopen("/proc/self/mountinfo", "r");
    char line[4096];
    size_t best = 0;
    int found = 0;

    if (f == NULL) {
        sgn_set_error_errno(SGN_ERR_IO, "open /proc/self/mountinfo");
        return SGN_ERR_IO;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *fields[8];
        char *sep;
        char *save = NULL;
        char *tok;
        size_t nf = 0;
        size_t mplen;

        sep = strstr(line, " - ");
        if (sep == NULL)
            continue;
        *sep = '\0';
        for (tok = strtok_r(line, " ", &save);
             tok != NULL && nf < 8u;
             tok = strtok_r(NULL, " ", &save))
            fields[nf++] = tok;
        if (nf < 5u)
            continue;

        mplen = strlen(fields[4]);
        if (strncmp(path, fields[4], mplen) != 0)
            continue;
        /* "/mnt" must not match "/mntfoo"; the root mount always matches. */
        if (mplen > 1u && path[mplen] != '\0' && path[mplen] != '/')
            continue;
        if (mplen < best)
            continue;

        save = NULL;
        (void)strtok_r(sep + 3, " ", &save);   /* fstype, skipped */
        tok = strtok_r(NULL, " ", &save);      /* mount source */
        if (tok == NULL)
            continue;
        {
            size_t tl = strlen(tok) + 1u;

            if (tl > dev_cap)
                continue;
            memcpy(dev, tok, tl);
        }
        best = mplen;
        found = 1;
    }
    (void)fclose(f);
    if (!found) {
        sgn_set_error(SGN_ERR_IO, "no mount point found for '%s'", path);
        return SGN_ERR_IO;
    }
    return SGN_OK;
}

int sgn_fs_uuid_for_path(const char *path, char *out, size_t cap)
{
    char real[PATH_MAX];
    char dev[PATH_MAX];
    char dev_real[PATH_MAX];
    DIR *d;
    struct dirent *ent;
    int rc;

    if (path == NULL || out == NULL || cap == 0) {
        sgn_set_error(SGN_ERR_INTERNAL, "sgn_fs_uuid_for_path: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    if (realpath(path, real) == NULL) {
        sgn_set_error_errno(SGN_ERR_IO, "realpath '%s'", path);
        return SGN_ERR_IO;
    }
    rc = mount_source_for(real, dev, sizeof(dev));
    if (rc != SGN_OK)
        return rc;
    if (realpath(dev, dev_real) == NULL) {
        sgn_set_error_errno(SGN_ERR_IO, "realpath '%s'", dev);
        return SGN_ERR_IO;
    }

    d = opendir("/dev/disk/by-uuid");
    if (d == NULL) {
        sgn_set_error_errno(SGN_ERR_IO, "opendir /dev/disk/by-uuid");
        return SGN_ERR_IO;
    }
    while ((ent = readdir(d)) != NULL) {
        char link[PATH_MAX];
        char target[PATH_MAX];

        if (ent->d_name[0] == '.')
            continue;
        if (snprintf(link, sizeof(link), "/dev/disk/by-uuid/%s",
                     ent->d_name) >= (int)sizeof(link))
            continue;
        if (realpath(link, target) == NULL)
            continue;
        if (strcmp(target, dev_real) != 0)
            continue;
        {
            size_t nl = strlen(ent->d_name) + 1u;

            if (nl > cap) {
                (void)closedir(d);
                sgn_set_error(SGN_ERR_NOSPACE,
                              "UUID does not fit the buffer");
                return SGN_ERR_NOSPACE;
            }
            memcpy(out, ent->d_name, nl);
        }
        (void)closedir(d);
        return SGN_OK;
    }
    (void)closedir(d);
    sgn_set_error(SGN_ERR_IO, "no filesystem UUID for '%s' (device %s)",
                  path, dev_real);
    return SGN_ERR_IO;
}
