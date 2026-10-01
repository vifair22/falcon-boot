/* falcon-boot: the fixture UKI's /init (docs/DESIGN.md Sections 5.1 and 10).
 *
 * The smallest program that can answer one question: did the passphrase the
 * loader typed into a synthetic filesystem
 * arrive in the initramfs as a systemd credential? sd-stub packs the
 * `.cred` files it finds in the `<uki>.efi.extra.d` directory next to the
 * image into a cpio and merges that into the initrd, so by the time PID 1
 * runs the files are simply
 * sitting in /.extra/credentials/. This prints what is there and compares the
 * one we care about against a baked-in expected value.
 *
 * It is NOT systemd and does not pretend to be. A real initramfs would have
 * systemd's PID 1 import that directory (dropping the .cred suffix) and
 * systemd-cryptsetup consume `cryptsetup.passphrase` from it; what this
 * fixture proves is the link before that, which is the only link this repo
 * writes any code for. The naming that the later links require is recorded in
 * falcon-efi/src/pos/pos.c.
 *
 * THIS IS RIG SCAFFOLDING. The passphrase it knows is the well-known test one
 * from falcon-efi/tools/mk-test-stick.sh, and printing credential bytes is the
 * whole point of it. Nothing here belongs anywhere near a real initramfs.
 *
 * Built static by falcon-efi/tools/mk-test-uki.sh: it is PID 1 in an initramfs
 * with no libraries in it.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

/* Must match FALCON_TEST_PASS in falcon-efi/tools/mk-test-stick.sh. */
#define EXPECT_PASS "falcon-test"

/* The name the whole chain turns on; see falcon-efi/src/pos/pos.c for why it is
 * this and not something readable. */
#define CRED_NAME "cryptsetup.passphrase.cred"

#define CRED_DIR "/.extra/credentials"

/* Enough for the fixture's credential many times over. A credential that does
 * not fit is reported as a mismatch, which is the answer we want from a
 * passphrase that is not the one we sent. */
#define READ_MAX 4096

/* The kernel gives an initramfs no console of its own: CONFIG_DEVTMPFS_MOUNT
 * explicitly does not apply here, so /dev is empty until we mount it. Without
 * this every print below goes to a closed descriptor and the run looks like a
 * hang. */
/* Everything the fixture proves is asserted by grepping the rig's serial
 * log, while a human doing UAT watches the QEMU window. The cmdline puts
 * tty0 last, so /dev/console (fd 1 below) is the screen; g_serial carries
 * the same bytes to ttyS0 so the headless assertions keep working. */
static int g_serial = -1;

static void open_console(void)
{
    int fd;

    (void)mkdir("/dev", 0755);
    (void)mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

    fd = open("/dev/console", O_RDWR);
    if (fd >= 0) {
        (void)dup2(fd, 0);
        (void)dup2(fd, 1);
        (void)dup2(fd, 2);
        if (fd > 2)
            (void)close(fd);
        setvbuf(stdout, NULL, _IONBF, 0);
    }
    g_serial = open("/dev/ttyS0", O_WRONLY | O_NOCTTY);
}

/* printf to both the console and the serial log; every line the fixture
 * emits goes through here so the two audiences cannot diverge. */
static void outf(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof line)
        n = (int)(sizeof line) - 1;
    /* glibc marks write() warn_unused_result; a fixture print that fails
     * has nowhere to report to, so the results are consumed explicitly. */
    if (write(1, line, (size_t)n) < 0) { /* nothing to do */ }
    if (g_serial >= 0 && write(g_serial, line, (size_t)n) < 0) {
        /* nothing to do */
    }
}

/* Prints the first `max` bytes of a credential with the unprintables escaped.
 * The fixture passphrase is not a secret, and seeing the exact bytes is how a
 * stray newline or a NUL terminator gets caught rather than guessed at. */
static void format_bytes(const unsigned char *p, size_t n, char *out,
                         size_t cap)
{
    size_t i;
    size_t w = 0;

    for (i = 0; i < n && w + 5 < cap; i++) {
        if (p[i] >= 0x20 && p[i] < 0x7f) {
            out[w++] = (char)p[i];
        } else {
            w += (size_t)snprintf(out + w, cap - w, "\\x%02x", p[i]);
        }
    }
    out[w] = '\0';
}

static ssize_t read_file(const char *path, unsigned char *buf, size_t cap)
{
    ssize_t total = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -1;
    for (;;) {
        ssize_t n = read(fd, buf + total, cap - (size_t)total);

        if (n < 0) {
            (void)close(fd);
            return -1;
        }
        if (n == 0)
            break;
        total += n;
        if ((size_t)total >= cap)
            break;
    }
    (void)close(fd);
    return total;
}

int main(void)
{
    unsigned char buf[READ_MAX];
    char path[512];
    DIR *d;
    struct dirent *e;
    int found = 0;
    int matched = 0;

    open_console();

    outf("\nFALCON-UKI-INIT\n");

    d = opendir(CRED_DIR);
    if (!d) {
        outf("FALCON-CRED-DIR-MISSING %s\n", CRED_DIR);
    } else {
        while ((e = readdir(d)) != NULL) {
            struct stat st;
            ssize_t n;

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            found++;
            snprintf(path, sizeof path, "%s/%s", CRED_DIR, e->d_name);
            if (stat(path, &st) != 0)
                st.st_size = -1;
            outf("FALCON-CRED-FILE %s %lld bytes\n", e->d_name,
                 (long long)st.st_size);

            n = read_file(path, buf, sizeof buf);
            if (n < 0) {
                outf("FALCON-CRED-UNREADABLE %s\n", e->d_name);
                continue;
            }
            {
                char shown[512];

                format_bytes(buf, (size_t)n < 64 ? (size_t)n : 64,
                             shown, sizeof shown);
                outf("FALCON-CRED-HEAD %s [%s]\n", e->d_name, shown);
            }

            if (strcmp(e->d_name, CRED_NAME) == 0 &&
                (size_t)n == strlen(EXPECT_PASS) &&
                memcmp(buf, EXPECT_PASS, (size_t)n) == 0)
                matched = 1;
        }
        (void)closedir(d);
        outf("FALCON-CRED-COUNT %d\n", found);
    }

    if (matched)
        outf("FALCON-CRED-OK\n");
    else
        outf("FALCON-CRED-FAIL\n");

    outf("FALCON-UKI-DONE\n");
    /* A human watching the QEMU window needs a moment to read the result;
     * a headless run just costs six extra seconds. Announced so the pause
     * reads as deliberate rather than a hang. */
    outf("powering off in 6 seconds.\n");
    (void)fflush(stdout);
    sleep(6);
    sync();
    /* Powering off rather than halting so the rig's QEMU exits on its own and
     * a headless run does not need a timeout to end. */
    (void)reboot(RB_POWER_OFF);
    for (;;)
        pause();
}
