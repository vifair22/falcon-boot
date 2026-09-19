/* falcon-boot appliance: running the helper programs. */

#include "appliance.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* The environment every child gets. Spelled out rather than inherited,
 * because PID 1 in an initramfs inherits nothing and `sign` looks its helpers
 * up on PATH. TMPDIR is the tmpfs mounted below: the extracted kernel and the
 * unsigned wrap are intermediate artifacts of a signing operation and they
 * have no business landing on any partition of the stick. */
static char *const g_child_env[] = {
    (char *)"PATH=/usr/bin:/usr/sbin:/bin:/sbin",
    (char *)"TMPDIR=/tmp",
    (char *)"HOME=/root",
    (char *)"LANG=C",
    (char *)"LC_ALL=C",
    /* No __pycache__ is shipped and writing one would only consume RAM. */
    (char *)"PYTHONDONTWRITEBYTECODE=1",
    NULL
};

/* ---- running children ---- */

/* Forwards a child's output to every console, one whole line at a time and
 * with a prefix so it sits under the appliance's own lines. Whole lines
 * because con_write's contract is one write per line; a line too long for the
 * buffer is emitted in pieces rather than waited on. */
static void echo_child(int fd, const char *prefix)
{
    char buf[512];
    size_t len = 0;

    for (;;) {
        ssize_t n = read(fd, buf + len, sizeof(buf) - 1u - len);
        size_t start = 0;
        size_t i;

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        len += (size_t)n;
        for (i = 0; i < len; i++) {
            if (buf[i] != '\n')
                continue;
            buf[i] = '\0';
            say("%s%s\n", prefix, buf + start);
            start = i + 1u;
        }
        if (start > 0u) {
            memmove(buf, buf + start, len - start);
            len -= start;
        } else if (len + 1u >= sizeof(buf)) {
            buf[len] = '\0';
            say("%s%s\n", prefix, buf);
            len = 0;
        }
    }
    if (len > 0u) {
        buf[len] = '\0';
        say("%s%s\n", prefix, buf);
    }
}

/* fork/exec with our own environment and, optionally, `in` on stdin. No
 * shell anywhere: every argument is passed as a vector, so nothing a file
 * name contains can become syntax. Returns the exit status, or -1.
 *
 * `echo` non-NULL routes the child's own output through say() with that
 * prefix, instead of letting it inherit /dev/console. Only the payload path
 * asks for it: what that child prints is the statement that this bless writes
 * to the user's file and the report of what it wrote, and those have to reach
 * the console the user is actually at. /dev/console is only ever one of them
 * (see the console section above), and on a serial-only machine it is not the
 * one the appliance's own prompts came out of.
 *
 * `in` and `echo` together would need both pipes pumped at once to be safe
 * against a child that fills the output pipe before reading its input; no
 * caller asks for both, and the input pipe is drained first below. */
int run(char *const argv[], const char *in, size_t in_len, const char *echo)
{
    int pipefd[2] = { -1, -1 };
    int outfd[2] = { -1, -1 };
    pid_t pid;
    int status = 0;

    if (in != NULL && pipe(pipefd) != 0) {
        say("ERROR: pipe: %s\n", strerror(errno));
        return -1;
    }
    if (echo != NULL && pipe(outfd) != 0) {
        say("ERROR: pipe: %s\n", strerror(errno));
        if (in != NULL) {
            (void)close(pipefd[0]);
            (void)close(pipefd[1]);
        }
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        say("ERROR: fork: %s\n", strerror(errno));
        if (in != NULL) {
            (void)close(pipefd[0]);
            (void)close(pipefd[1]);
        }
        if (echo != NULL) {
            (void)close(outfd[0]);
            (void)close(outfd[1]);
        }
        return -1;
    }
    if (pid == 0) {
        if (in != NULL) {
            (void)close(pipefd[1]);
            (void)dup2(pipefd[0], 0);
            (void)close(pipefd[0]);
        }
        if (echo != NULL) {
            /* stderr as well as stdout: the tool's refusals are the half of
             * its output a user most needs to see. */
            (void)close(outfd[0]);
            (void)dup2(outfd[1], 1);
            (void)dup2(outfd[1], 2);
            (void)close(outfd[1]);
        }
        (void)execve(argv[0], argv, g_child_env);
        /* execve only returns on failure, and the parent has no other way to
         * learn which program was missing. */
        (void)fprintf(stderr, "ERROR: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    if (in != NULL) {
        size_t off = 0;

        (void)close(pipefd[0]);
        while (off < in_len) {
            ssize_t n = write(pipefd[1], in + off, in_len - off);

            if (n <= 0)
                break;
            off += (size_t)n;
        }
        (void)close(pipefd[1]);
    }
    if (echo != NULL) {
        (void)close(outfd[1]);
        echo_child(outfd[0], echo);
        (void)close(outfd[0]);
    }
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}
