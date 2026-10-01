#include <mkstick/pass.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <common/log.h>
#include <common/util.h>

/* Not memset: a compiler is entitled to delete a memset whose result is never
 * read, which is exactly the shape of every wipe. volatile stores are not. */
static void wipe(char *p, size_t n)
{
    volatile char *v = (volatile char *)p;

    while (n-- > 0)
        *v++ = 0;
}

void pass_wipe(mks_pass *p)
{
    wipe(p->buf, sizeof p->buf);
    p->len = 0;
}

static cli_status validate(const mks_pass *p)
{
    size_t i;

    if (p->len == 0)
        return set_error(CLI_ERR_INVALID, "the passphrase is empty");

    for (i = 0; i < p->len; i++) {
        unsigned char c = (unsigned char)p->buf[i];

        if (c < 0x20u || c >= 0x7fu)
            return set_error(CLI_ERR_INVALID,
                             "the passphrase must be printable ASCII; byte "
                             "0x%02x would not unlock p3 from the boot menu",
                             c);
    }
    if (p->len < 8u)
        log_warn("the passphrase is %zu characters; it protects p4 (root and "
                 "home) as well as p3", p->len);
    return CLI_OK;
}

static cli_status from_file(mks_pass *p, const char *file)
{
    void *buf = NULL;
    size_t len = 0;
    size_t i;
    cli_status rc;

    rc = util_read_file(file, &buf, &len, MKS_PASS_MAX);
    if (rc != CLI_OK)
        return rc;

    /* First line only: a passphrase file written by `echo` carries a trailing
     * newline that is not part of the passphrase, and cryptsetup's own
     * --key-file has the same rule. */
    for (i = 0; i < len; i++) {
        char c = ((const char *)buf)[i];

        if (c == '\n' || c == '\r')
            break;
    }
    if (i >= sizeof p->buf) {
        wipe(buf, len);
        return set_error(CLI_ERR_RANGE, "%s holds more than %u characters",
                         file, (unsigned)MKS_PASS_MAX - 1u);
    }
    memcpy(p->buf, buf, i);
    p->buf[i] = '\0';
    p->len = i;

    wipe(buf, len);
    return validate(p);
}

/* Reads one line from the terminal with echo off.
 *
 * read(2) rather than stdio, deliberately. stdio would buffer the typed
 * passphrase inside a FILE this code cannot reach, so the secret would sit in
 * a second copy nothing wipes; a one-byte-at-a-time read keeps the only copy
 * the one the caller owns and can erase. A passphrase prompt is not a hot
 * loop, so the syscall per character costs nothing that matters. */
static cli_status read_line(int fd, const char *prompt, char *out,
                            size_t cap, size_t *len)
{
    struct termios saved;
    struct termios quiet;
    int have_termios;
    size_t got = 0;
    int hit_eof = 0;
    int read_errno = 0;

    (void)fprintf(stderr, "%s", prompt);
    (void)fflush(stderr);

    have_termios = (tcgetattr(fd, &saved) == 0);
    if (have_termios) {
        quiet = saved;
        quiet.c_lflag &= (tcflag_t)~(ECHO);
        quiet.c_lflag |= ECHONL;
        if (tcsetattr(fd, TCSAFLUSH, &quiet) != 0)
            have_termios = 0;
    }
    if (!have_termios)
        log_warn("could not disable terminal echo; the passphrase will be "
                 "visible as you type");

    /* The terminal is closed or redirected more often than anyone expects
     * (a CI job, a `< /dev/null`), and a prompt loop that treats end-of-input
     * as an empty line would silently create a stick with no passphrase. So
     * EOF and a read error are both tracked and both refused. */
    for (;;) {
        char c;
        ssize_t n = read(fd, &c, 1);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            read_errno = errno;
            break;
        }
        if (n == 0) {
            hit_eof = 1;
            break;
        }
        if (c == '\n' || c == '\r')
            break;
        if (got + 1u >= cap) {
            got = cap;            /* signals "too long" to the caller */
            break;
        }
        out[got++] = c;
    }

    if (have_termios)
        (void)tcsetattr(fd, TCSAFLUSH, &saved);
    if (!have_termios)
        (void)fputc('\n', stderr);

    if (read_errno != 0) {
        errno = read_errno;
        return set_error_errno(CLI_ERR_IO, "reading the passphrase");
    }
    if (hit_eof)
        return set_error(CLI_ERR_IO,
                         "end of input at the passphrase prompt; use "
                         "--passphrase-file when there is no terminal");
    if (got >= cap)
        return set_error(CLI_ERR_RANGE, "passphrase longer than %u characters",
                         (unsigned)cap - 1u);
    out[got] = '\0';
    *len = got;
    return CLI_OK;
}

static cli_status from_terminal(mks_pass *p)
{
    mks_pass again;
    cli_status rc;
    int tty;

    /* /dev/tty, not stdin: the passphrase must come from the controlling
     * terminal even when stdin is a pipe, and it must not be readable out of
     * a redirected stdin by accident. */
    tty = open("/dev/tty", O_RDONLY | O_CLOEXEC);
    if (tty < 0)
        return set_error_errno(CLI_ERR_IO,
                               "opening /dev/tty to prompt for the "
                               "passphrase (use --passphrase-file when there "
                               "is no terminal)");

    memset(&again, 0, sizeof again);
    rc = read_line(tty, "passphrase for p3 and p4: ", p->buf, sizeof p->buf,
                   &p->len);
    if (rc == CLI_OK)
        rc = read_line(tty, "repeat: ", again.buf, sizeof again.buf,
                       &again.len);
    (void)close(tty);

    if (rc == CLI_OK) {
        if (again.len != p->len ||
            memcmp(again.buf, p->buf, p->len) != 0)
            rc = set_error(CLI_ERR_INVALID, "the two passphrases differ");
    }
    pass_wipe(&again);

    if (rc != CLI_OK) {
        pass_wipe(p);
        return rc;
    }
    return validate(p);
}

cli_status pass_unlock(mks_pass *p, const char *file, const char *prompt)
{
    cli_status rc;
    int tty;

    memset(p, 0, sizeof *p);
    if (file != NULL)
        return from_file(p, file);
    tty = open("/dev/tty", O_RDONLY | O_CLOEXEC);
    if (tty < 0)
        return set_error_errno(CLI_ERR_IO,
                               "opening /dev/tty to prompt for the "
                               "passphrase (use --passphrase-file when there "
                               "is no terminal)");
    rc = read_line(tty, prompt, p->buf, sizeof p->buf, &p->len);
    (void)close(tty);
    if (rc != CLI_OK) {
        pass_wipe(p);
        return rc;
    }
    return validate(p);
}

cli_status pass_acquire(mks_pass *p, const char *file)
{
    memset(p, 0, sizeof *p);
    if (file != NULL)
        return from_file(p, file);
    return from_terminal(p);
}
