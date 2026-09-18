#include <common/proc.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <common/log.h>

#define PROC_MAX_ARGV 64u

/* execvp's prototype is `char *const argv[]` for historical reasons; it does
 * not modify the strings, and POSIX says so explicitly. Laundering the
 * qualifier through a union rather than a cast keeps -Wcast-qual meaningful
 * everywhere else in this tree, where a dropped const really would be a bug. */
static int exec_const(const char *const *argv)
{
    union {
        const char *const *in;
        char *const       *out;
    } u;

    u.in = argv;
    return execvp(argv[0], u.out);
}

static void log_argv(const char *const *argv)
{
    char line[1024];
    size_t used = 0;
    size_t i;

    if (cli_log_level_get() > CLI_LOG_DEBUG)
        return;

    line[0] = '\0';
    for (i = 0; argv[i] != NULL && used + 1 < sizeof line; i++) {
        int n = snprintf(line + used, sizeof line - used, "%s%s",
                         (i > 0) ? " " : "", argv[i]);
        if (n < 0)
            break;
        used += (size_t)n;
        if (used >= sizeof line)
            break;
    }
    log_debug("exec: %s", line);
}

/* Everything the child does between fork and exec, in one place and marked
 * noreturn, because it is: every path either execs or _exit()s. Keeping it
 * out of proc_run is not cosmetic. The descriptors this dups onto 0 and 1 are
 * meant to survive into the exec'd program, so a reader (human or static
 * analyser) tracking "is this fd closed on every path" has to see that these
 * paths do not return to the parent's bookkeeping at all. */
static void child(const proc_spec *spec, int in_pipe[2], int out_pipe[2])
    __attribute__((noreturn));

/* gcc -fanalyzer reports each dup2 below as an fd leak, and it is wrong in a
 * way it cannot be argued out of: the whole point of dup2 onto descriptor 0
 * or 1 is that the new descriptor OUTLIVES this code, first past the exec
 * that replaces the process image and then for as long as the exec'd program
 * runs. Closing it is what would be the bug. The analyzer has no model for
 * exec, so it sees a descriptor opened and never closed.
 *
 * Scoped to this function and no wider, so a genuine leak anywhere else in
 * the file is still reported. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"

static void child(const proc_spec *spec, int in_pipe[2], int out_pipe[2])
{
    int devnull = -1;

    if (spec->in != NULL) {
        (void)close(in_pipe[1]);
        if (dup2(in_pipe[0], STDIN_FILENO) < 0)
            _exit(127);
        (void)close(in_pipe[0]);
    } else {
        devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)close(devnull);
        }
    }

    if (spec->out != NULL) {
        (void)close(out_pipe[0]);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0)
            _exit(127);
        (void)close(out_pipe[1]);
    } else if (spec->quiet_stdout) {
        devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            (void)dup2(devnull, STDOUT_FILENO);
            (void)close(devnull);
        }
    }

    (void)exec_const(spec->argv);
    _exit(127);
}

#pragma GCC diagnostic pop

cli_status proc_run(const proc_spec *spec)
{
    int in_pipe[2] = { -1, -1 };
    int out_pipe[2] = { -1, -1 };
    pid_t pid;
    int wstatus = 0;
    cli_status rc = CLI_OK;

    if (spec == NULL || spec->argv == NULL || spec->argv[0] == NULL)
        return set_error(CLI_ERR_INTERNAL, "proc_run: empty argv");

    log_argv(spec->argv);

    if (spec->in != NULL && pipe(in_pipe) != 0)
        return set_error_errno(CLI_ERR_EXEC, "pipe for %s stdin",
                               spec->argv[0]);
    if (spec->out != NULL && pipe(out_pipe) != 0) {
        if (spec->in != NULL) {
            (void)close(in_pipe[0]);
            (void)close(in_pipe[1]);
        }
        return set_error_errno(CLI_ERR_EXEC, "pipe for %s stdout",
                               spec->argv[0]);
    }

    /* Each pipe is closed on the path that created it and by the condition
     * that created it, rather than by a shared "close whatever is still
     * open" epilogue. Sweeping up at the end reads as tidier and is not: it
     * makes the fd's lifetime depend on a sentinel value nobody can see from
     * the place the fd was opened, and neither a reader nor a static
     * analyser can then tell an owned descriptor from a stale integer. */
    pid = fork();
    if (pid < 0) {
        rc = set_error_errno(CLI_ERR_EXEC, "fork for %s", spec->argv[0]);
        if (spec->in != NULL) {
            (void)close(in_pipe[0]);
            (void)close(in_pipe[1]);
        }
        if (spec->out != NULL) {
            (void)close(out_pipe[0]);
            (void)close(out_pipe[1]);
        }
        return rc;
    }

    if (pid == 0)
        child(spec, in_pipe, out_pipe);

    /* Parent. */
    if (spec->in != NULL) {
        const char *p = (const char *)spec->in;
        size_t left = spec->in_len;

        (void)close(in_pipe[0]);
        in_pipe[0] = -1;
        while (left > 0) {
            ssize_t n = write(in_pipe[1], p, left);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                rc = set_error_errno(CLI_ERR_EXEC, "writing stdin of %s",
                                     spec->argv[0]);
                break;
            }
            p += (size_t)n;
            left -= (size_t)n;
        }
        (void)close(in_pipe[1]);
        in_pipe[1] = -1;
    }

    if (spec->out != NULL) {
        size_t used = 0;
        int overflow = 0;

        (void)close(out_pipe[1]);
        out_pipe[1] = -1;
        for (;;) {
            char scratch[4096];
            ssize_t n = read(out_pipe[0], scratch, sizeof scratch);

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                rc = set_error_errno(CLI_ERR_EXEC, "reading stdout of %s",
                                     spec->argv[0]);
                break;
            }
            if (n == 0)
                break;
            if (used + (size_t)n < spec->out_cap) {
                memcpy(spec->out + used, scratch, (size_t)n);
                used += (size_t)n;
            } else {
                overflow = 1;
            }
        }
        (void)close(out_pipe[0]);
        out_pipe[0] = -1;
        if (spec->out_cap > 0)
            spec->out[used] = '\0';
        if (spec->out_len != NULL)
            *spec->out_len = used;
        if (overflow && rc == CLI_OK)
            rc = set_error(CLI_ERR_RANGE, "%s produced more than %zu bytes "
                           "of output", spec->argv[0], spec->out_cap - 1);
    }

    /* Both pipes are fully closed by here: the parent blocks closed them as
     * it finished with each end, and the child owns its own copies. */
    while (waitpid(pid, &wstatus, 0) < 0) {
        if (errno != EINTR)
            return set_error_errno(CLI_ERR_EXEC, "waiting for %s",
                                   spec->argv[0]);
    }

    if (WIFSIGNALED(wstatus))
        rc = set_error(CLI_ERR_EXEC, "%s was killed by signal %d",
                       spec->argv[0], WTERMSIG(wstatus));
    else if (!WIFEXITED(wstatus))
        rc = set_error(CLI_ERR_EXEC, "%s ended abnormally", spec->argv[0]);
    else if (WEXITSTATUS(wstatus) == 127)
        rc = set_error(CLI_ERR_EXEC, "%s could not be run (not installed?)",
                       spec->argv[0]);
    else if (WEXITSTATUS(wstatus) != 0)
        rc = set_error(CLI_ERR_EXEC, "%s exited %d", spec->argv[0],
                       WEXITSTATUS(wstatus));

    return rc;
}

static cli_status collect(const char *prog, va_list ap, const char **argv,
                          size_t cap)
{
    size_t n = 0;

    argv[n++] = prog;
    for (;;) {
        const char *a = va_arg(ap, const char *);

        if (a == NULL)
            break;
        if (n + 1 >= cap)
            return set_error(CLI_ERR_INTERNAL,
                             "%s: more than %zu arguments", prog, cap - 2);
        argv[n++] = a;
    }
    argv[n] = NULL;
    return CLI_OK;
}

cli_status proc_runl(const char *prog, ...)
{
    const char *argv[PROC_MAX_ARGV];
    proc_spec spec;
    va_list ap;
    cli_status rc;

    va_start(ap, prog);
    rc = collect(prog, ap, argv, PROC_MAX_ARGV);
    va_end(ap);
    if (rc != CLI_OK)
        return rc;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    return proc_run(&spec);
}

cli_status proc_runl_quiet(const char *prog, ...)
{
    const char *argv[PROC_MAX_ARGV];
    proc_spec spec;
    va_list ap;
    cli_status rc;

    va_start(ap, prog);
    rc = collect(prog, ap, argv, PROC_MAX_ARGV);
    va_end(ap);
    if (rc != CLI_OK)
        return rc;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    /* --verbose is the way to see what a tool had to say; by default the
     * chatter of six formatters would bury our own progress lines. */
    spec.quiet_stdout = (cli_log_level_get() > CLI_LOG_DEBUG);
    return proc_run(&spec);
}

cli_status proc_input_l(const void *in, size_t in_len, const char *prog, ...)
{
    const char *argv[PROC_MAX_ARGV];
    proc_spec spec;
    va_list ap;
    cli_status rc;

    va_start(ap, prog);
    rc = collect(prog, ap, argv, PROC_MAX_ARGV);
    va_end(ap);
    if (rc != CLI_OK)
        return rc;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.in = in;
    spec.in_len = in_len;
    spec.quiet_stdout = (cli_log_level_get() > CLI_LOG_DEBUG);
    return proc_run(&spec);
}

cli_status proc_capture_l(char *out, size_t cap, size_t *len,
                          const char *prog, ...)
{
    const char *argv[PROC_MAX_ARGV];
    proc_spec spec;
    va_list ap;
    cli_status rc;

    va_start(ap, prog);
    rc = collect(prog, ap, argv, PROC_MAX_ARGV);
    va_end(ap);
    if (rc != CLI_OK)
        return rc;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.out = out;
    spec.out_cap = cap;
    spec.out_len = len;
    return proc_run(&spec);
}

int proc_exists(const char *prog)
{
    const char *path = getenv("PATH");
    const char *p;

    if (prog == NULL || prog[0] == '\0')
        return 0;
    if (strchr(prog, '/') != NULL)
        return access(prog, X_OK) == 0;
    if (path == NULL)
        path = "/usr/bin:/bin:/usr/sbin:/sbin";

    for (p = path; *p != '\0';) {
        const char *end = strchr(p, ':');
        size_t len = (end != NULL) ? (size_t)(end - p) : strlen(p);
        char candidate[4096];

        if (len > 0 && len + strlen(prog) + 2 < sizeof candidate) {
            (void)snprintf(candidate, sizeof candidate, "%.*s/%s",
                           (int)len, p, prog);
            if (access(candidate, X_OK) == 0)
                return 1;
        }
        if (end == NULL)
            break;
        p = end + 1;
    }
    return 0;
}
