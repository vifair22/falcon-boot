/* falcon-boot appliance: the consoles, and reading from them. */

#include "appliance.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <termios.h>
#include <unistd.h>

/* ---- console ----
 *
 * THE TRAP, because it is surprising enough to have cost a hardware boot:
 * Linux writes kernel output to EVERY console= on the cmdline, but only the
 * LAST one becomes /dev/console, and /dev/console is the only console a
 * process can READ from. Name a serial port the machine does not really have
 * and you get a console that prints (through the kernel's other console, on
 * screen) yet answers every read with EOF. Measured on real hardware
 * 2026-09-13: the appliance ran, printed its whole survey to the laptop
 * screen, reached "press Enter" and rebooted instantly, because the /dev
 * /console behind it was a serial port with nothing on the other end.
 *
 * So this program does not trust /dev/console to be the one the user is at.
 * It opens every console it can (/dev/console, the screen, the rig's serial
 * port), writes to all of them, and reads from whichever produces a byte
 * first. The three worlds this has to serve - a laptop with a screen and a
 * keyboard, the headless rig typing into a serial FIFO, a machine whose only
 * console is a serial line - then all work off the same image, and the
 * cmdline's console= ORDER stops deciding whether the appliance is usable.
 *
 * Deduplication is by device number, not by name: /dev/console is a device of
 * its own (5:1) that redirects to one of the others, so names and st_rdev
 * both say "different device" about two paths to the same terminal. TIOCGDEV
 * asks the kernel which tty is really behind a descriptor, which is the one
 * answer that distinguishes them; st_rdev is the fallback when it fails.
 */

#define CONSOLE_MAX 4

typedef struct {
    int   fd;
    dev_t dev;       /* the terminal really behind fd, for deduplication */
    int   readable;  /* cleared once this console has stopped giving input */
} console_dev;

static console_dev g_con[CONSOLE_MAX];
static size_t      g_con_n;

static void write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);

        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            /* A console that has stopped listening is not a reason to stop
             * talking to the others. */
            return;
        }
        off += (size_t)n;
    }
}

/* Formatted whole and written once per console, so a line arrives as one
 * write: two consoles interleaving inside a line would be readable on
 * neither. */
static void con_write(const char *buf, size_t len)
{
    size_t i;

    /* Nothing opened: fd 1 is whatever the kernel handed PID 1, which is
     * still better than dropping the message. */
    if (g_con_n == 0u) {
        write_all(1, buf, len);
        return;
    }
    for (i = 0; i < g_con_n; i++)
        write_all(g_con[i].fd, buf, len);
}

void say(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    con_write(buf, ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1u);
}

/* Which terminal is really behind this descriptor. */
static dev_t con_devno(int fd)
{
    struct stat st;
    unsigned int real = 0;

    if (ioctl(fd, TIOCGDEV, &real) == 0)
        return (dev_t)makedev((real >> 8) & 0xfffu, (real & 0xffu) |
                              ((real >> 12) & 0xfff00u));
    if (fstat(fd, &st) == 0)
        return st.st_rdev;
    return (dev_t)0;
}

/* Opens one candidate and keeps it unless it is a terminal already held.
 * O_NOCTTY throughout: PID 1 is a session leader, and a controlling terminal
 * would let a Ctrl-C at any prompt kill init, which the kernel answers with a
 * panic. Nothing here needs job control. */
static void console_add(const char *path)
{
    int fd;
    dev_t dev;
    size_t i;

    if (g_con_n == CONSOLE_MAX)
        return;
    fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        return;
    dev = con_devno(fd);
    for (i = 0; i < g_con_n; i++) {
        if (g_con[i].dev == dev) {
            (void)close(fd);
            return;
        }
    }
    g_con[g_con_n].fd = fd;
    g_con[g_con_n].dev = dev;
    g_con[g_con_n].readable = 1;
    g_con_n++;
}

/* The kernel gives an initramfs no console of its own, so /dev has to exist
 * before the first print or every message goes to a closed descriptor and the
 * run looks like a hang (the same rule falcon-efi/tools/uki-init/init.c
 * records).
 *
 * The candidates are /dev/console (whatever the cmdline made it), the screen,
 * and the serial port the cmdline names for the rig. A candidate that cannot
 * be opened is simply not in the set. Descriptors 0/1/2 stay on /dev/console
 * because that is what the children inherit; their own output is not routed
 * through this program. */
void console_open(void)
{
    int fd;

    (void)mkdir("/dev", 0755);
    (void)mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

    console_add("/dev/console");
    console_add("/dev/tty0");
    console_add("/dev/ttyS1");

    fd = open("/dev/console", O_RDWR | O_NOCTTY);
    if (fd < 0)
        return;
    (void)dup2(fd, 0);
    (void)dup2(fd, 1);
    (void)dup2(fd, 2);
    if (fd > 2)
        (void)close(fd);
    setvbuf(stdout, NULL, _IONBF, 0);
}

/* One byte from whichever console produces one first. Returns 1 with the byte
 * in *c, or 0 once every console has stopped giving input: a console at EOF is
 * retired from the readable set rather than polled again, or a dead one would
 * spin this loop forever. It keeps its place in the output set, because not
 * being typed into says nothing about being read. */
static int con_getc(char *c)
{
    struct pollfd pfd[CONSOLE_MAX];
    size_t idx[CONSOLE_MAX];
    nfds_t n = 0;
    size_t alive;
    size_t i;

    if (g_con_n == 0u)
        return (read(0, c, 1) == 1) ? 1 : 0;

    for (i = 0; i < g_con_n; i++) {
        if (!g_con[i].readable)
            continue;
        idx[n] = i;
        pfd[n].fd = g_con[i].fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        n++;
    }
    if (n == 0u)
        return 0;

    alive = (size_t)n;
    while (alive > 0u) {
        if (poll(pfd, n, -1) < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        for (i = 0; i < (size_t)n; i++) {
            ssize_t rd;

            if (pfd[i].fd < 0 || pfd[i].revents == 0)
                continue;
            rd = read(pfd[i].fd, c, 1);
            if (rd == 1)
                return 1;
            if (rd < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            g_con[idx[i]].readable = 0;
            pfd[i].fd = -1;      /* poll() ignores a negative descriptor */
            alive--;
        }
    }
    return 0;
}

/* Echo off on every console at once, so a passphrase typed on one of them is
 * not printed on another. Each console's old state is saved separately: they
 * are different terminals and only the ones that were changed are restored.
 * Both loops also stop at CONSOLE_MAX, the size of the caller's arrays;
 * console_add never lets g_con_n pass it, and saying so here is what lets the
 * analyzer (and a reader) see the indexing is in bounds. */
static void con_echo_off(struct termios *saved, int *saved_ok)
{
    size_t i;

    for (i = 0; i < g_con_n && i < CONSOLE_MAX; i++) {
        struct termios quiet;

        saved_ok[i] = (tcgetattr(g_con[i].fd, &saved[i]) == 0);
        if (!saved_ok[i])
            continue;
        quiet = saved[i];
        quiet.c_lflag &= (tcflag_t)~(tcflag_t)ECHO;
        (void)tcsetattr(g_con[i].fd, TCSAFLUSH, &quiet);
    }
}

static void con_echo_restore(const struct termios *saved, const int *saved_ok)
{
    size_t i;

    for (i = 0; i < g_con_n && i < CONSOLE_MAX; i++)
        if (saved_ok[i])
            (void)tcsetattr(g_con[i].fd, TCSAFLUSH, &saved[i]);
}

/* ---- the passphrase ---- */

/* Reads a line from the console with the terminal's echo off, exactly as
 * every other passphrase prompt on this stick does. Returns the length, or
 * -1 on EOF/error. The caller owns wiping the buffer. */
ssize_t read_secret(char *buf, size_t cap)
{
    struct termios saved[CONSOLE_MAX];
    int saved_ok[CONSOLE_MAX] = { 0 };
    size_t len = 0;

    con_echo_off(saved, saved_ok);

    for (;;) {
        char c;

        if (!con_getc(&c)) {
            len = 0;
            break;
        }
        if (c == '\n' || c == '\r')
            break;
        if (len + 1u < cap)
            buf[len++] = c;
    }

    con_echo_restore(saved, saved_ok);
    say("\n");

    buf[len] = '\0';
    return (ssize_t)len;
}

/* Reads a small file whole (the loader-delivered credential). Returns the
 * byte count, 0 for empty, -1 when absent or unreadable; never an error the
 * caller must act on, since the interactive prompt is the fallback. */

ssize_t read_line(char *buf, size_t cap)
{
    size_t len = 0;

    for (;;) {
        char c;

        if (!con_getc(&c))
            return -1;
        if (c == '\n' || c == '\r')
            break;
        if (len + 1u < cap)
            buf[len++] = c;
    }
    buf[len] = '\0';
    return (ssize_t)len;
}
