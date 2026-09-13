/* airies-boot: the fixture UKI's /init (docs/DESIGN.md Sections 5.1 and 10).
 *
 * The smallest program that can answer the one question step 6 of the build
 * order asks: did the passphrase the loader typed into a synthetic filesystem
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
 * src/pos/pos.c.
 *
 * THIS IS RIG SCAFFOLDING. The passphrase it knows is the well-known test one
 * from tools/mk-test-stick.sh, and printing credential bytes is the whole
 * point of it. Nothing here belongs anywhere near a real initramfs.
 *
 * Built static by tools/mk-test-uki.sh: it is PID 1 in an initramfs with no
 * libraries in it.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

/* Must match AIRIES_TEST_PASS in tools/mk-test-stick.sh. */
#define EXPECT_PASS "airies-test"

/* The name the whole chain turns on; see src/pos/pos.c for why it is this and
 * not something readable. */
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
static void open_console(void)
{
    int fd;

    (void)mkdir("/dev", 0755);
    (void)mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

    fd = open("/dev/console", O_RDWR);
    if (fd < 0)
        return;
    (void)dup2(fd, 0);
    (void)dup2(fd, 1);
    (void)dup2(fd, 2);
    if (fd > 2)
        (void)close(fd);
    setvbuf(stdout, NULL, _IONBF, 0);
}

/* Prints the first `max` bytes of a credential with the unprintables escaped.
 * The fixture passphrase is not a secret, and seeing the exact bytes is how a
 * stray newline or a NUL terminator gets caught rather than guessed at. */
static void print_bytes(const unsigned char *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (p[i] >= 0x20 && p[i] < 0x7f)
            putchar((int)p[i]);
        else
            printf("\\x%02x", p[i]);
    }
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

    printf("\nAIRIES-UKI-INIT\n");

    d = opendir(CRED_DIR);
    if (!d) {
        printf("AIRIES-CRED-DIR-MISSING %s\n", CRED_DIR);
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
            printf("AIRIES-CRED-FILE %s %lld bytes\n", e->d_name,
                   (long long)st.st_size);

            n = read_file(path, buf, sizeof buf);
            if (n < 0) {
                printf("AIRIES-CRED-UNREADABLE %s\n", e->d_name);
                continue;
            }
            printf("AIRIES-CRED-HEAD %s [", e->d_name);
            print_bytes(buf, (size_t)n < 64 ? (size_t)n : 64);
            printf("]\n");

            if (strcmp(e->d_name, CRED_NAME) == 0 &&
                (size_t)n == strlen(EXPECT_PASS) &&
                memcmp(buf, EXPECT_PASS, (size_t)n) == 0)
                matched = 1;
        }
        (void)closedir(d);
        printf("AIRIES-CRED-COUNT %d\n", found);
    }

    if (matched)
        printf("AIRIES-CRED-OK\n");
    else
        printf("AIRIES-CRED-FAIL\n");

    printf("AIRIES-UKI-DONE\n");
    (void)fflush(stdout);
    sync();
    /* Powering off rather than halting so the rig's QEMU exits on its own and
     * a headless run does not need a timeout to end. */
    (void)reboot(RB_POWER_OFF);
    for (;;)
        pause();
}
