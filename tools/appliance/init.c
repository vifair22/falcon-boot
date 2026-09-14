/* falcon-boot: the on-stick signing appliance's PID 1 (DESIGN Sections 7.5
 * and 6.3.1, build order step 8).
 *
 * The appliance is a signed UKI that boots under full Secure Boot, surveys the
 * ISO library on p2 and does two different things with the two halves of it,
 * both through the SAME `falcon-boot-sign` implementation the host route runs
 * (here the static link, /usr/bin/falcon-boot-sign-static). Then it reboots.
 * That is the whole program; there is no shell in this initramfs and nothing
 * else to run.
 *
 *   Windows media is PREPARED (DESIGN Sections 6.3 and 6.3.1). The whole
 *   batch runs in one boot, because the reboot is the expensive part and
 *   batching is what makes dropping three images cost one of them. The batch
 *   is still released by one mark-review-confirm: DESIGN Section 7.7 makes
 *   patching a bless operation and sends it through the same gate as signing.
 *   Preparation signs nothing and holds no key.
 *
 *   Everything else is BLESSED (DESIGN Section 7.5). p3 is unlocked, the MOK
 *   private key that lives there is read, and the user marks the unsigned
 *   ISOs one by one and then confirms the marked set as a whole.
 *
 *   A bare EFI payload (a .efi, or a .img whose payload is one) is blessed
 *   too, and it is the one bless that WRITES TO THE USER'S OWN FILE: the
 *   payload boots as itself, so there is no wrap to put the signature in
 *   instead (DESIGN Section 7.2). The review therefore names every file that
 *   will be written to, inside a .img as well as outside one, in the sign
 *   tool's own words, before the yes that releases it.
 *
 * Each half asks its own gate, immediately before the work that gate releases,
 * so a mixed run proposes two operations in the order they will happen and the
 * user can accept either without accepting the other. One combined review
 * would put a minutes-long preparation batch between the signing "yes" and the
 * signing, which is the opposite of Section 7.7's "at the moment of
 * confirmation".
 *
 * The two halves are ordered prepare-then-bless, and that ordering is load
 * bearing rather than arbitrary: p3 is opened only if the bless half has
 * something to do. An auto-prep boot (the loader launched this image because
 * the user asked for a Windows ISO to be prepared) therefore decrypts no key
 * and asks for no passphrase at all, and preparation works unchanged on a
 * --p3-plain stick, which is what src/cli/sign/prepare.h promises.
 *
 * The appliance receives no instruction from the loader and looks for none.
 * `FalconNextBoot` is the loader's variable and its whole lifecycle is the
 * loader's (DESIGN Section 6.3.1: one component sets it and clears it);
 * nothing here reads it, writes it or is steered by it. What this program
 * does is decided entirely by what is on p2 when it looks.
 *
 * Why a C init rather than busybox: the appliance is the only place besides an
 * external host where the private key is ever decrypted, so the userland
 * around that moment should be small enough to read in one sitting. busybox
 * would bring several hundred applets to run four programs.
 *
 * What this program must never do, and the reasons, so nobody "improves" it:
 *
 *   - It never signs anything itself and never touches the key bytes. It
 *     hands `sign` two PATHS under /mnt/p3 and `sign` hands them to sbsign.
 *     The key is read from the LUKS volume by the process that needs it and
 *     is never copied anywhere, least of all onto p2 (DESIGN Section 7.5).
 *   - It never unlocks p3 for the preparation half. Preparation needs no
 *     secret, so giving it one would widen the window the key is decrypted in
 *     for nothing, and the smallness of that window is the reason the
 *     appliance exists rather than a full OS (DESIGN Section 7.5).
 *   - The passphrase never appears in argv (it goes to cryptsetup on a pipe),
 *     never reaches a file, and is wiped from this process's memory as soon
 *     as the unlock returns. Core dumps are disabled at entry for the same
 *     reason.
 *   - p3 is unlocked and mounted READ-ONLY. Nothing in the signing flow writes
 *     to the shelf: the UKI wrap and the manifest record both land on p2. A
 *     read-only mapping means a bug in anything downstream cannot damage the
 *     one copy of the key.
 *   - Nothing is signed OR patched that the user did not mark by hand and then
 *     confirm. The per-image mark plus the typed "yes" over the reviewed list
 *     together ARE the "I vouch for these" act the design rests on: they are
 *     what stops an ISO someone dropped on the stick from being blessed
 *     because it happened to be sitting there. Neither list is ever
 *     pre-marked, and each confirmation prints every path it is about to act
 *     on, so nothing can be swept in unseen (DESIGN Section 7.5,
 *     select-then-sign; Section 7.7, the same gate for patched media).
 *   - There is no network in this initramfs (no interface is brought up, and
 *     nothing here would use one), so the decrypted-key window cannot be
 *     reached from outside the machine.
 *
 * The partitions are identified out of the GPT through src/part/part.c, the
 * same parser AND the same matcher (part_match_role) the loader uses, so the
 * appliance and the loader cannot disagree about which partition is which. The manifest is read
 * through src/manifest/manifest.c, the same parser the loader will use, so
 * "already blessed" means here exactly what it will mean at the menu. Each
 * ISO is classified through src/cli/sign/isoprobe.c and src/family/family.c,
 * which is the reader and the table `falcon-boot-sign` itself uses, so the
 * survey cannot put an image in a queue the tool will then refuse it from.
 * The set of file names the survey looks at is the LOADER's set (src/efi
 * /main.c: *.iso, *.img, *.efi), because an appliance that disagreed with the
 * menu about what counts as media would be unable to bless something the user
 * can see and boot.
 *
 * Built static by tools/appliance/mk-appliance.sh: it is PID 1 in an
 * initramfs whose dynamic loader exists for the other binaries, not for this
 * one.
 */

/* _GNU_SOURCE comes from the host flag set in the Makefile (HOST_CFLAGS),
 * which mk-appliance.sh copies verbatim. */

#include <appliance/route.h>

#include <cli/sign/payload.h>
#include <cli/sign/prepcore.h>
#include <cli/sign/sign.h>
#include <family/family.h>
#include <manifest/manifest.h>
#include <part/part.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* ---- fixed layout of this initramfs and of the stick ---- */

#define P2_MNT   "/mnt/p2"
#define P3_MNT   "/mnt/p3"
#define MAPPER   "falcon-p3"
#define MAPPER_NODE "/dev/mapper/" MAPPER

/* mkstick writes both of these to the root of the LUKS volume
 * (src/cli/mkstick/mkstick.h, MKS_P3_KEY_NAME / MKS_P3_CERT_NAME). */
#define KEY_PATH  P3_MNT "/falcon-mok.key"
#define CERT_PATH P3_MNT "/falcon-mok.crt"

#define SIGN_TOOL "/usr/bin/falcon-boot-sign-static"
#define STUB_PATH "/usr/lib/systemd/boot/efi/linuxx64.efi.stub"

/* What the preparation half needs from the image, checked by name so an
 * appliance built without one of them says so once instead of failing per
 * ISO with a message about the ISO. WIM_TOOL rewrites boot.wim; Z_TOOL is
 * what reads the UDF namespace, and Windows media is UDF-only, so without it
 * every Windows ISO on p2 would classify as an unknown Linux image and land
 * in the wrong queue. WINJUMP_PATH is the file that replaces winpeshl.exe
 * inside the patched image; it sits beside the sign tool because that is the
 * first place src/cli/sign/prepare.c looks (find_winjump). */
#define WIM_TOOL     "wimlib-imagex"
#define Z_TOOL       "7z"
#define WINJUMP_PATH "/usr/bin/winjump.exe"

#define MANIFEST_PATH P2_MNT "/falcon/blessed/manifest"

/* Reserved directory of DESIGN Section 6.2; never a bless candidate. */
#define RESERVED_DIR "falcon"

#define PASS_MAX     512u
#define UNLOCK_TRIES 3

/* The loader-delivered credential (DESIGN Section 5.1): sd-stub packs the
 * companion file into the initramfs under this name; systemd would strip
 * the .cred suffix on import, but this init reads the file itself. */
#define CRED_FILE "cryptsetup.passphrase.cred"

#define MAX_CANDIDATES 64u
#define MAX_RECORDS    256u
#define MANIFEST_MAX   (1u << 20)

/* p2 is a drag-and-drop library, so the tree is the user's, not ours. Eight
 * levels is deeper than any ISO collection and stops a symlink-free but
 * pathological tree from recursing this program to death. */
#define WALK_MAX_DEPTH 8

/* Where the build script stages the kernel modules this flow needs. Arch's
 * kernel builds ext4 in but ships device-mapper, exFAT, the loop driver and
 * FAT as modules, so the appliance carries them rather than assuming: without
 * dm-crypt there is no unlock, without exfat there is no p2, and without loop
 * and vfat there is no way into the filesystem inside a .img payload, which
 * is where src/cli/sign/payload.c has to write.
 *
 * The files are named NN-<module>.ko and loaded in name order, because that
 * order is a dependency order the build script computed from the kernel's own
 * modules.dep. Resolving dependencies here would mean parsing modules.dep in
 * PID 1 to learn something that is fixed the moment the image is built. */
#define MODULE_DIR "/modules"

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

static void say(const char *fmt, ...)
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
 * run looks like a hang (the same rule tools/uki-init/init.c records).
 *
 * The candidates are /dev/console (whatever the cmdline made it), the screen,
 * and the serial port the cmdline names for the rig. A candidate that cannot
 * be opened is simply not in the set. Descriptors 0/1/2 stay on /dev/console
 * because that is what the children inherit; their own output is not routed
 * through this program. */
static void console_open(void)
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
 * are different terminals and only the ones that were changed are restored. */
static void con_echo_off(struct termios *saved, int *saved_ok)
{
    size_t i;

    for (i = 0; i < g_con_n; i++) {
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

    for (i = 0; i < g_con_n; i++)
        if (saved_ok[i])
            (void)tcsetattr(g_con[i].fd, TCSAFLUSH, &saved[i]);
}

static void early_mounts(void)
{
    (void)mkdir("/proc", 0555);
    (void)mkdir("/sys", 0555);
    (void)mkdir("/tmp", 01777);
    (void)mkdir("/run", 0755);
    (void)mkdir("/mnt", 0755);
    (void)mkdir(P2_MNT, 0755);
    (void)mkdir(P3_MNT, 0755);

    (void)mount("proc", "/proc", "proc", 0, NULL);
    (void)mount("sysfs", "/sys", "sysfs", 0, NULL);
    /* The work area for kernel extraction and the unsigned wrap. RAM only:
     * nothing a signing run produces before the signature exists should be
     * able to outlive the run on persistent storage. */
    (void)mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777");
    /* cryptsetup keeps its device locks here and warns loudly without it. */
    (void)mkdir("/run/cryptsetup", 0700);
}

static void load_modules(void)
{
    struct dirent **ents = NULL;
    int n = scandir(MODULE_DIR, &ents, NULL, alphasort);
    int i;

    if (n < 0) {
        say("note: no %s directory in this initramfs\n", MODULE_DIR);
        return;
    }
    for (i = 0; i < n; i++) {
        char path[PATH_MAX];
        size_t len = strlen(ents[i]->d_name);
        int fd;

        if (len < 4u || strcmp(ents[i]->d_name + len - 3u, ".ko") != 0) {
            free(ents[i]);
            continue;
        }
        (void)snprintf(path, sizeof(path), "%s/%s", MODULE_DIR,
                       ents[i]->d_name);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            say("note: %s could not be opened (%s)\n", path, strerror(errno));
            free(ents[i]);
            continue;
        }
        if (syscall(SYS_finit_module, fd, "", 0) != 0 && errno != EEXIST) {
            /* Not fatal on its own: a kernel with the facility built in
             * refuses the module and the flow works anyway. The failure that
             * matters shows up as a failed unlock or a failed mount, with a
             * message that says which. */
            say("note: %s did not load (%s)\n", ents[i]->d_name,
                strerror(errno));
        }
        (void)close(fd);
        free(ents[i]);
    }
    free(ents);
}

/* ---- partition discovery ---- */

/* Device-node paths. Sized for a kernel block-device name (DISK_NAME_LEN is
 * 32, but sysfs entries are bounded only by NAME_MAX) plus "/dev/" and a
 * partition suffix, so no name a disk can carry gets silently truncated into
 * the path of some other device. */
#define NODE_MAX 288u

typedef struct {
    char p2[NODE_MAX];    /* device node of the exFAT ISO library */
    char p3[NODE_MAX];    /* device node of the LUKS2 UKI shelf */
} stick_parts;

static int disk_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    int fd = *(const int *)ctx;
    ssize_t n = pread(fd, buf, (size_t)len, (off_t)off);

    return (n == (ssize_t)len) ? 0 : -1;
}

/* /dev/sda + 3 -> /dev/sda3, /dev/nvme0n1 + 3 -> /dev/nvme0n1p3. The rule is
 * the kernel's: a trailing digit in the disk name takes a 'p' separator. */
static void part_node(char *out, size_t cap, const char *disk, uint32_t number)
{
    size_t n = strlen(disk);
    const char *sep = (n > 0 && disk[n - 1u] >= '0' && disk[n - 1u] <= '9')
                          ? "p" : "";

    (void)snprintf(out, cap, "/dev/%s%s%u", disk, sep, number);
}

/* Block devices that cannot be the stick: no loopback, no ramdisk, no
 * device-mapper node (that is our own mapping once p3 is open), no optical
 * drive. Everything else gets a GPT read. */
static int skip_disk(const char *name)
{
    static const char *const pre[] = { "loop", "ram", "dm-", "md", "sr", "zram" };
    size_t i;

    for (i = 0; i < sizeof(pre) / sizeof(pre[0]); i++) {
        if (strncmp(name, pre[i], strlen(pre[i])) == 0)
            return 1;
    }
    return 0;
}

/* Reads the first `len` bytes of a device. Used by the fallback probe and by
 * the exFAT volume-serial read; both want a boot sector and nothing else. */
static int read_head(const char *node, void *buf, size_t len)
{
    int fd = open(node, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, buf, len);
    (void)close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

static int looks_like_luks(const char *node)
{
    uint8_t hdr[8];
    static const uint8_t magic[6] = { 'L', 'U', 'K', 'S', 0xba, 0xbe };

    if (read_head(node, hdr, sizeof(hdr)) != 0)
        return 0;
    return memcmp(hdr, magic, sizeof(magic)) == 0;
}

static int looks_like_exfat(const char *node)
{
    uint8_t vbr[16];

    if (read_head(node, vbr, sizeof(vbr)) != 0)
        return 0;
    return memcmp(vbr + 3, "EXFAT   ", 8) == 0;
}

/* Identification proper: DESIGN Section 3's rule (p3 by its falcon type GUID,
 * p2 by Microsoft basic data plus the name FALCON-ISO), applied by the
 * loader's own matcher over a GPT read by the loader's own parser. Returns
 * nonzero when both partitions were found on one disk; a stick always has
 * both on one disk, and accepting a p2 from one device and a p3 from another
 * would be a way to get a key from one stick to bless media on somebody
 * else's. */
static int find_by_ident(const char *disk, stick_parts *out)
{
    char node[NODE_MAX];
    gpt_part parts[128];
    gpt_table t;
    int fd;
    int rc;
    uint32_t i;
    int have2 = 0;
    int have3 = 0;

    (void)snprintf(node, sizeof(node), "/dev/%s", disk);
    fd = open(node, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;

    t.parts = parts;
    t.cap = (uint32_t)(sizeof(parts) / sizeof(parts[0]));
    t.count = 0;
    t.truncated = 0;

    rc = gpt_parse(&t, disk_read, &fd, 512);
    (void)close(fd);
    if (rc != GPT_OK)
        return 0;

    for (i = 0; i < t.count; i++) {
        if (gpt_part_is_role(&parts[i], PART_ROLE_ISO)) {
            part_node(out->p2, sizeof(out->p2), disk, parts[i].number);
            have2 = 1;
        } else if (gpt_part_is_role(&parts[i], PART_ROLE_SHELF)) {
            part_node(out->p3, sizeof(out->p3), disk, parts[i].number);
            have3 = 1;
        }
    }
    return have2 && have3;
}

/* Fallback for a stick that predates this identification scheme (and for the
 * rig's older fixtures): take the first exFAT partition as p2 and the first LUKS
 * partition as p3, on a disk that carries both. Content probing rather than
 * naming, and deliberately narrower than the loader's own fallback: this
 * program is about to ask for a passphrase, and the honest failure is "no
 * falcon stick found", not a prompt aimed at some unrelated volume. */
static int find_by_content(const char *disk, stick_parts *out)
{
    char dir[PATH_MAX];
    DIR *d;
    struct dirent *e;
    int have2 = 0;
    int have3 = 0;

    (void)snprintf(dir, sizeof(dir), "/sys/block/%s", disk);
    d = opendir(dir);
    if (d == NULL)
        return 0;

    while ((e = readdir(d)) != NULL) {
        char node[NODE_MAX];

        if (e->d_name[0] == '.')
            continue;
        if (strncmp(e->d_name, disk, strlen(disk)) != 0)
            continue;   /* only this disk's partition directories */
        (void)snprintf(node, sizeof(node), "/dev/%s", e->d_name);
        if (!have2 && looks_like_exfat(node)) {
            (void)snprintf(out->p2, sizeof(out->p2), "%s", node);
            have2 = 1;
        } else if (!have3 && looks_like_luks(node)) {
            (void)snprintf(out->p3, sizeof(out->p3), "%s", node);
            have3 = 1;
        }
    }
    (void)closedir(d);
    return have2 && have3;
}

/* Names every disk the kernel currently has, so a failure to find the stick
 * says what WAS there. Without this the message is the same whether no bus
 * driver loaded, the stick is genuinely absent, or its partitions are not
 * ours, and those need different answers from the user. */
static void say_block_devices(void)
{
    DIR *d = opendir("/sys/block");
    struct dirent *e;
    int n = 0;

    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || skip_disk(e->d_name))
            continue;
        say("%s %s", n == 0 ? "       disks seen:" : ",", e->d_name);
        n++;
    }
    (void)closedir(d);
    say(n == 0 ? "       disks seen: none at all\n" : "\n");
}

static int find_stick_once(stick_parts *out)
{
    DIR *d = opendir("/sys/block");
    struct dirent *e;
    int found = 0;

    memset(out, 0, sizeof(*out));
    if (d == NULL) {
        say("ERROR: /sys/block is not readable; is sysfs mounted?\n");
        return 0;
    }
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || skip_disk(e->d_name))
            continue;
        if (find_by_ident(e->d_name, out)) {
            say("stick: %s (falcon GPT identification)\n", e->d_name);
            found = 1;
        }
    }
    if (!found) {
        rewinddir(d);
        while (!found && (e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.' || skip_disk(e->d_name))
                continue;
            if (find_by_content(e->d_name, out)) {
                say("stick: %s (untyped GPT; exFAT and LUKS volumes probed)\n",
                    e->d_name);
                found = 1;
            }
        }
    }
    (void)closedir(d);
    return found;
}

/* The stick does not exist yet when this program starts. finit_module returns
 * as soon as the module is in the kernel, not when the bus it drives has
 * finished enumerating, and USB mass storage takes a second or more to probe,
 * attach a SCSI device and publish /sys/block/sdX. There is no udev in here to
 * wait on, so waiting is this loop.
 *
 * The rig never showed it: an emulated SATA disk is present before init runs,
 * so a single pass always succeeded there and the race was invisible until the
 * appliance met a real USB stick (2026-09-13) and reported no stick found.
 *
 * Bounded by wall clock rather than attempt count, for the same reason the
 * same guard in src/winjump is: one pass costs whatever its slowest device
 * costs, so a count expires in unpredictable real time. A stick that is
 * genuinely absent should end in a legible message, not a hang, which is why
 * this gives up at all.
 */
#define STICK_WAIT_MS 30000
#define STICK_POLL_MS 250

static int find_stick(stick_parts *out)
{
    unsigned waited = 0;
    int said = 0;

    for (;;) {
        if (find_stick_once(out))
            return 1;
        if (waited >= STICK_WAIT_MS)
            return 0;
        if (!said) {
            say("waiting for the stick to enumerate...\n");
            said = 1;
        }
        (void)usleep(STICK_POLL_MS * 1000u);
        waited += STICK_POLL_MS;
    }
}

/* p2's volume serial, in the form blkid reports it, because that is what the
 * family cmdline templates mean by %DEV% and what the booted initramfs will
 * resolve. Read from the exFAT VBR rather than asked of blkid: there is no
 * udev in here to maintain /dev/disk/by-uuid, which is where the host route's
 * lookup gets it from (src/cli/sign/util.c). */
static int exfat_uuid(const char *node, char *out, size_t cap)
{
    uint8_t vbr[512];
    uint32_t serial;

    if (read_head(node, vbr, sizeof(vbr)) != 0)
        return -1;
    if (memcmp(vbr + 3, "EXFAT   ", 8) != 0)
        return -1;
    serial = (uint32_t)vbr[100] | ((uint32_t)vbr[101] << 8) |
             ((uint32_t)vbr[102] << 16) | ((uint32_t)vbr[103] << 24);
    if (snprintf(out, cap, "%04X-%04X", (unsigned)(serial >> 16),
                 (unsigned)(serial & 0xFFFFu)) >= (int)cap)
        return -1;
    return 0;
}

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
 * `beat_s` non-zero makes the wait say, every that many seconds, that the
 * child is still running. Only the preparation batch asks for it, and only
 * because hashing a multi-gigabyte ISO off USB is minutes during which the
 * tool has nothing to print and the machine is indistinguishable from a hung
 * one. The line is indented to sit under that batch's per-image lines, which
 * is the one place it appears.
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
static int run(char *const argv[], const char *in, size_t in_len,
               unsigned beat_s, const char *echo)
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
    if (beat_s == 0u) {
        if (waitpid(pid, &status, 0) < 0)
            return -1;
    } else {
        unsigned elapsed = 0;

        for (;;) {
            pid_t done = waitpid(pid, &status, WNOHANG);

            if (done == pid)
                break;
            if (done < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            (void)usleep(1000u * 1000u);
            elapsed++;
            if (elapsed % beat_s == 0u)
                say("      ... still working (%um%02us so far)\n",
                    elapsed / 60u, elapsed % 60u);
        }
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

/* ---- the passphrase ---- */

/* Reads a line from the console with the terminal's echo off, exactly as
 * every other passphrase prompt on this stick does. Returns the length, or
 * -1 on EOF/error. The caller owns wiping the buffer. */
static ssize_t read_secret(char *buf, size_t cap)
{
    struct termios saved[CONSOLE_MAX];
    int saved_ok[CONSOLE_MAX];
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
static ssize_t read_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    size_t len = 0;

    if (fd < 0)
        return -1;
    for (;;) {
        ssize_t n = read(fd, buf + len, cap - 1u - len);

        if (n < 0) {
            (void)close(fd);
            return -1;
        }
        if (n == 0)
            break;
        len += (size_t)n;
        if (len + 1u >= cap)
            break;
    }
    (void)close(fd);
    buf[len] = '\0';
    return (ssize_t)len;
}

/* The confirmation prompt. Echoed on purpose: the user is meant to see the
 * word they are typing, because typing it is the whole ceremony. The echo is
 * the terminal's own, so it appears on the console being typed at and not on
 * the others; only what this program prints goes everywhere. */
static ssize_t read_line(char *buf, size_t cap)
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

/* ---- the ISO survey ---- */

typedef struct {
    char              rel[MANIFEST_PATH_MAX]; /* path relative to the p2 root */
    uint64_t          size;
    manifest_bless    state;                /* NONE or STALE; FRESH is done */
    family_id         fam;
    /* NONE for an ISO. A payload is signed in place, which decides what the
     * review has to disclose and which arguments the tool is given, so the
     * shape travels with the candidate rather than being re-derived from the
     * name at each of those points. */
    sgn_payload_shape shape;
    int               selected;             /* marked by hand; never preset */
} candidate;

static manifest_record g_records[MAX_RECORDS];
static size_t          g_record_n;
/* The bless queue. Everything downstream of the selection ceremony works on
 * this one, which is why it keeps the plain name it has always had. */
static candidate       g_cand[MAX_CANDIDATES];
static size_t          g_cand_n;
/* The prepare queue: no marks, no ceremony, no key (DESIGN Section 6.3.1). */
static candidate       g_prep[MAX_CANDIDATES];
static size_t          g_prep_n;
static size_t          g_done_prepared;
static size_t          g_done_blessed;
static size_t          g_unreadable;
static char            g_manifest[MANIFEST_MAX];

static void load_manifest(void)
{
    int fd = open(MANIFEST_PATH, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    size_t count = 0;

    g_record_n = 0;
    if (fd < 0) {
        /* DESIGN Section 8.1: an absent manifest is "nothing is blessed",
         * never an error. */
        say("manifest: none yet on this stick\n");
        return;
    }
    n = read(fd, g_manifest, sizeof(g_manifest));
    (void)close(fd);
    if (n <= 0) {
        say("manifest: empty\n");
        return;
    }
    (void)manifest_parse(g_manifest, (size_t)n, g_records, MAX_RECORDS,
                         &count);
    g_record_n = (count > MAX_RECORDS) ? MAX_RECORDS : count;
    say("manifest: %zu blessed record(s)\n", g_record_n);
}

static int ends_with_iso(const char *name)
{
    size_t n = strlen(name);

    return n > 4u && strcasecmp(name + n - 4u, ".iso") == 0;
}

/* Appends to one of the two queues. `what` names the queue in the overflow
 * message, because "more than 64 pending images" is a different problem
 * depending on which half ran out. */
static void add_to(candidate *q, size_t *n, const char *what, const char *rel,
                   uint64_t size, manifest_bless st, family_id fam,
                   sgn_payload_shape shape)
{
    if (*n >= MAX_CANDIDATES) {
        say("note: more than %u images %s; the rest are not listed\n",
            (unsigned)MAX_CANDIDATES, what);
        return;
    }
    if (snprintf(q[*n].rel, sizeof(q[*n].rel), "%s", rel) >=
            (int)sizeof(q[*n].rel)) {
        say("note: skipping '%s': its path is too long for a manifest record\n",
            rel);
        return;
    }
    q[*n].size = size;
    q[*n].state = st;
    q[*n].fam = fam;
    q[*n].shape = shape;
    q[*n].selected = 0;
    (*n)++;
}

/* The family of one ISO, read the way `falcon-boot-sign` reads it: bsdtar
 * over the ISO9660 namespace and 7-Zip over the UDF one, unioned into a
 * listing, then src/family/family.c fingerprinted over that. Same reader and
 * same table as the tool this program is about to hand the file to, so the
 * survey cannot route an image into a queue the tool will then refuse it
 * from. No mount and no loop device: src/cli/sign/isoprobe.c says in as many
 * words why a tool that reads untrusted media should not be mounting it.
 *
 * Returns 0 and *out on success; -1 when nothing here could read the file as
 * an ISO at all, with the reason already in the sign tool's error buffer. */
static int classify(const char *abs, family_id *out)
{
    iso_view *v = NULL;
    family_probe probe;

    if (iso_view_open(abs, &v) != SGN_OK)
        return -1;
    iso_view_family_probe(v, &probe);
    *out = family_detect(&probe)->id;
    iso_view_close(v);
    return 0;
}

/* A prepared record names files under p2:/falcon/prepared/<sha256>/, and the
 * record is not evidence that they are still there: a folder emptied by hand,
 * or a run interrupted before the manifest was written, both leave a record
 * describing artifacts that do not exist. `falcon-boot-sign prepare` makes
 * exactly this check before it agrees an ISO needs nothing
 * (src/cli/sign/prepare.c); making it here too is what keeps "already
 * prepared, skipping" from being a claim nobody verified, and what stops an
 * ISO from being quietly dropped from the batch it belongs in. */
static int sidecars_intact(const manifest_record *rec)
{
    size_t i;

    if (rec == NULL || rec->splice_n == 0u)
        return 0;
    for (i = 0; i < rec->splice_n; i++) {
        char path[PATH_MAX];
        struct stat st;

        if (snprintf(path, sizeof(path), "%s/%s/%s/%s", P2_MNT, PREP_DIR,
                     rec->sha256, rec->splice[i].name) >= (int)sizeof(path))
            return 0;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
            (uint64_t)st.st_size != rec->splice[i].len)
            return 0;
    }
    return 1;
}

/* One file's whole survey, split out because a payload and an ISO answer the
 * two questions below differently and neither answer belongs in the walk.
 *
 * A payload is not read as an ISO: there is nothing in a PE or a FAT image
 * for bsdtar to list, so classify() would report it unreadable and drop a
 * perfectly blessable file. Its family is decided inside the tool, from the
 * volumes inside the image, and only to refuse the families that boot
 * something after the payload (sgn_payload_refusal).
 *
 * Nor is a payload looked up in the manifest. It has no record by design
 * (DESIGN Section 7.2), so a lookup could only find one somebody wrote by
 * hand, and honouring that would be treating an editable file as evidence
 * about a signature. Whether it is already blessed is asked of the PE itself,
 * later, where the certificate to ask with exists (payload_prefilter). */
static void survey_file(const char *abs, const char *rel, uint64_t size)
{
    sgn_payload_shape shape = sgn_payload_shape_of(rel);
    const manifest_record *rec = NULL;
    manifest_bless state = MANIFEST_BLESS_NONE;
    manifest_kind kind = MANIFEST_KIND_BLESSED;
    family_id fam = FAMILY_UNKNOWN;
    appl_job job;

    if (shape != SGN_PAYLOAD_NONE) {
        job = appl_route_payload(APPL_OURS_UNKNOWN);
    } else {
        state = manifest_lookup(g_records, g_record_n, rel, size, &rec);
        if (rec != NULL)
            kind = rec->kind;
        if (classify(abs, &fam) != 0) {
            /* Not a refusal to proceed, and not a silent drop either: an
             * unreadable file named .iso is offered to neither half, because
             * both would fail on it further in and with less to say about
             * why. */
            say("  unreadable  /%s\n              %s\n", rel, sgn_error());
            g_unreadable++;
            return;
        }
        job = appl_route(state, kind, fam);
    }

    switch (job) {
    case APPL_JOB_DONE_PREPARED:
        if (!sidecars_intact(rec)) {
            say("  prepared    /%s\n"
                "              the manifest says so, but the staged\n"
                "              files are missing or the wrong length;\n"
                "              preparing it again\n", rel);
            add_to(g_prep, &g_prep_n, "to prepare", rel, size, state, fam,
                   shape);
            break;
        }
        say("  prepared    /%s (already; skipping)\n", rel);
        g_done_prepared++;
        break;
    case APPL_JOB_DONE_BLESSED:
        say("  blessed     /%s (already; skipping)\n", rel);
        g_done_blessed++;
        break;
    case APPL_JOB_PREPARE:
        add_to(g_prep, &g_prep_n, "to prepare", rel, size, state, fam, shape);
        break;
    case APPL_JOB_SIGN:
    default:
        add_to(g_cand, &g_cand_n, "to sign", rel, size, state, fam, shape);
        break;
    }
}

/* Recursive walk of p2 looking for what the loader's own scan lists: *.iso,
 * *.img and *.efi (src/efi/main.c). `rel` is the path so far relative to the
 * p2 root, which is the form the manifest records and the form `sign` embeds
 * in the signed cmdline. */
static void walk(const char *abs, const char *rel, unsigned depth)
{
    DIR *d = opendir(abs);
    struct dirent *e;

    if (d == NULL)
        return;

    while ((e = readdir(d)) != NULL) {
        char child_abs[PATH_MAX];
        char child_rel[MANIFEST_PATH_MAX];
        struct stat st;

        if (e->d_name[0] == '.')
            continue;
        if (depth == 0 && strcasecmp(e->d_name, RESERVED_DIR) == 0)
            continue;   /* our own staging directory, never a candidate */
        if (snprintf(child_abs, sizeof(child_abs), "%s/%s", abs, e->d_name) >=
                (int)sizeof(child_abs))
            continue;
        if (snprintf(child_rel, sizeof(child_rel), "%s%s%s", rel,
                     rel[0] != '\0' ? "/" : "", e->d_name) >=
                (int)sizeof(child_rel))
            continue;
        if (lstat(child_abs, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            if (depth + 1u < WALK_MAX_DEPTH)
                walk(child_abs, child_rel, depth + 1u);
            continue;
        }
        if (!S_ISREG(st.st_mode))
            continue;
        if (!ends_with_iso(e->d_name) &&
            sgn_payload_shape_of(e->d_name) == SGN_PAYLOAD_NONE)
            continue;

        survey_file(child_abs, child_rel, (uint64_t)st.st_size);
    }
    (void)closedir(d);
}

/* ---- the pending list and the selection ceremony ----
 *
 * DESIGN Section 7.5 (select-then-sign, 2026-09-13): the user marks the images
 * one at a time, the whole marked set is shown back once, and only a typed
 * "yes" over that reviewed set starts any work. The listing is grouped by
 * folder so it reads like the loader's own folder-view menu (Section 6 stage
 * 1); the grouping is presentation only, everything below still works on the
 * full p2-relative path.
 *
 * ONE implementation serves both gates, because DESIGN Section 7.7 says the
 * consent is the same act for signed media and for patched media: "what the
 * review shows differs; that it is shown, and that the user says yes to it,
 * does not". So the words the two gates differ in are data below, and the
 * marking, the numbering, the grouping, the review and the typed word are
 * code neither gate has its own copy of. */

#define MIB (1024u * 1024u)

/* The per-gate words, plus whether the review has a cost to state. */
typedef struct {
    const char *list_head;   /* header above the grouped listing */
    const char *stale_note;  /* per-image, when the manifest record is stale */
    const char *mark_lead;   /* the sentence introducing the key summary */
    const char *prompt;      /* one word per gate, so the console (and the log)
                              * says which gate is being answered */
    const char *act;         /* imperative: "type 'yes' to <act> them" */
    const char *acting;      /* gerund: "leaving without <acting> anything" */
    const char *acted;       /* participle: "nothing was <acted>" */
    const char *consent;     /* what saying yes means, printed over the yes */
    int         show_cost;   /* the review states the batch's size and that it
                              * will take a while, before it is incurred */
} gate_ui;

static const gate_ui g_bless_ui = {
    "pending unsigned images",
    "blessed earlier; the file has changed since",
    "Mark what you vouch for, then review it:",
    "select> ",
    "sign", "signing", "signed",
    "Signing these says you trust every one of them to run on this\n"
    "machine with Secure Boot on.\n",
    0
};

static const gate_ui g_prep_ui = {
    "Windows images that are not prepared yet",
    "prepared earlier; the file has changed since",
    "Mark the ones you want prepared, then review them:",
    "prepare> ",
    "prepare", "preparing", "prepared",
    "Preparing leaves your ISO byte for byte as the vendor published it:\n"
    "the patched boot.wim is staged beside it. Nothing is signed and no\n"
    "key is read, so p3 stays locked.\n",
    1
};

/* The file name half of a p2-relative path. */
static const char *base_of(const char *rel)
{
    const char *slash = strrchr(rel, '/');

    return (slash != NULL) ? slash + 1 : rel;
}

/* The folder half, "" for an image sitting at the p2 root. */
static void dir_of(char *out, size_t cap, const char *rel)
{
    const char *slash = strrchr(rel, '/');
    size_t n = (slash != NULL) ? (size_t)(slash - rel) : 0u;

    if (n >= cap)
        n = cap - 1u;
    memcpy(out, rel, n);
    out[n] = '\0';
}

/* Path order, with the folder compared before the file name so every folder's
 * images come out contiguous and the root's come first ("" sorts before any
 * folder name). That is the order the numbers, the review and the signing run
 * all use, so the number next to an image never moves under the user. */
static int cand_cmp(const void *a, const void *b)
{
    const candidate *x = (const candidate *)a;
    const candidate *y = (const candidate *)b;
    char dx[MANIFEST_PATH_MAX];
    char dy[MANIFEST_PATH_MAX];
    int c;

    dir_of(dx, sizeof(dx), x->rel);
    dir_of(dy, sizeof(dy), y->rel);
    c = strcmp(dx, dy);
    if (c != 0)
        return c;
    return strcmp(base_of(x->rel), base_of(y->rel));
}

static size_t selected_count(const candidate *q, size_t n)
{
    size_t i;
    size_t sel = 0;

    for (i = 0; i < n; i++) {
        if (q[i].selected)
            sel++;
    }
    return sel;
}

/* A size a human reads at a glance. KiB below a mebibyte, because the library
 * now holds bare payloads as well as images and a 900 KiB Shell.efi listed as
 * "0 MiB" reads as an empty file. */
static void size_str(char *out, size_t cap, uint64_t bytes)
{
    if (bytes < MIB)
        (void)snprintf(out, cap, "%llu KiB",
                       (unsigned long long)(bytes / 1024u));
    else
        (void)snprintf(out, cap, "%llu MiB",
                       (unsigned long long)(bytes / MIB));
}

/* One image's line. Printed on its own after a toggle so a dumb serial console
 * does not have to redraw the whole list for every keystroke. */
static void print_item(const candidate *q, size_t i, const gate_ui *ui)
{
    char size[32];

    size_str(size, sizeof(size), q[i].size);
    say("    [%c] %2zu  %s  (%s)\n", q[i].selected ? 'x' : ' ',
        i + 1u, base_of(q[i].rel), size);
    if (q[i].state == MANIFEST_BLESS_STALE)
        say("           %s\n", ui->stale_note);
}

static void print_list(const candidate *q, size_t n, const gate_ui *ui)
{
    char dir[MANIFEST_PATH_MAX];
    char cur[MANIFEST_PATH_MAX];
    size_t i;
    int have_cur = 0;

    cur[0] = '\0';
    say("\n%s:\n\n", ui->list_head);
    for (i = 0; i < n; i++) {
        dir_of(dir, sizeof(dir), q[i].rel);
        if (!have_cur || strcmp(dir, cur) != 0) {
            /* "/" for the root, "Linux/" for a folder: one shape, and the
             * trailing slash is what says "this is a folder, not an image". */
            say("  %s/\n", dir);
            (void)snprintf(cur, sizeof(cur), "%s", dir);
            have_cur = 1;
        }
        print_item(q, i, ui);
    }
}

/* A bare 1-based image number, and nothing else. Returns 1 and the 0-based
 * index for a number in range, -1 for a number that names no image, 0 for
 * anything that is not a number. Deliberately strict: "2 " or "2y" is a typo,
 * and guessing what a typo meant is not a thing to do in front of a signing
 * key. */
static int parse_index(const char *s, size_t n, size_t *out)
{
    unsigned long v = 0;
    size_t i;

    if (s[0] == '\0')
        return 0;
    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = v * 10u + (unsigned long)(s[i] - '0');
        if (v > (unsigned long)n)
            return -1;   /* keeps the accumulator from running away too */
    }
    if (v == 0u)
        return -1;
    *out = (size_t)v - 1u;
    return 1;
}

/* A byte count as a GiB figure with one decimal. Integer arithmetic: a tenth
 * of a gibibyte is all the precision the cost sentence needs, and a float in
 * PID 1 buys nothing. */
static void gib_str(char *out, size_t cap, uint64_t bytes)
{
    uint64_t tenths = (bytes * 10u) >> 30;

    (void)snprintf(out, cap, "%llu.%llu", (unsigned long long)(tenths / 10u),
                   (unsigned long long)(tenths % 10u));
}

/* What blessing this payload will write to, in the sign tool's own words and
 * before the yes that releases it (DESIGN Section 7.2).
 *
 * The words are the tool's rather than ours, and that is the point. --dry-run
 * is the bless about to be confirmed minus the writes: it opens the image the
 * same way and names every file inside it that would be written to, partition
 * by partition. A sentence composed here would be a second description of
 * what src/cli/sign/payload.c does, free to drift from it, and it could not
 * name what is inside a .img without a second copy of that volume walk.
 *
 * The output is echoed through this program rather than left to the child's
 * inherited console, because this is the statement the whole gate turns on
 * and it has to arrive on the console the user is answering from.
 *
 * A refusal (a family whose boot runs something after the payload) surfaces
 * here too, which is the right moment for it: before anything is vouched
 * for. */
static void payload_disclosure(const candidate *c)
{
    char abs[PATH_MAX];

    (void)snprintf(abs, sizeof(abs), "%s/%.*s", P2_MNT,
                   (int)(sizeof(c->rel) - 1u), c->rel);
    {
        char *argv[] = {
            (char *)SIGN_TOOL, abs,
            (char *)"--key", (char *)KEY_PATH,
            (char *)"--cert", (char *)CERT_PATH,
            (char *)"--dry-run", NULL
        };

        if (run(argv, NULL, 0, 0, "      ") != 0)
            say("      the sign tool would not act on this file; confirming\n"
                "      the set will fail on it and leave it as it is.\n");
    }
}

/* The batch review. Prints every marked path in full and takes the one word
 * that authorises the batch. 1 = confirmed, 0 = back to the list (a typo must
 * not cost the marks), -1 = the console closed.
 *
 * A gate that states a cost states it HERE, over the list and before the yes,
 * because the cost is part of what is being agreed to: preparation reads each
 * image whole to hash it and then repacks its boot.wim, which on USB is
 * minutes of a machine that looks idle. Measured on real hardware
 * 2026-09-13: two Windows ISOs went straight into that with nothing asked and
 * nothing said, and the run was indistinguishable from a hang. */
static int review(const candidate *q, size_t n, size_t sel_n,
                  const gate_ui *ui)
{
    char answer[32];
    uint64_t total = 0;
    size_t payload_n = 0;
    size_t i;

    say("\nreview: %zu image%s marked\n\n", sel_n, (sel_n == 1u) ? "" : "s");
    for (i = 0; i < n; i++) {
        if (!q[i].selected)
            continue;
        total += q[i].size;
        if (ui->show_cost) {
            char size[32];

            size_str(size, sizeof(size), q[i].size);
            say("  /%s  (%s)\n", q[i].rel, size);
        } else {
            say("  /%s\n", q[i].rel);
        }
        if (q[i].shape != SGN_PAYLOAD_NONE) {
            payload_n++;
            payload_disclosure(&q[i]);
        }
    }
    say("\n%s", ui->consent);
    /* Said over the list and under the tool's own statement about each
     * payload, because it is the difference between this gate and the two
     * that came before it: an ISO is left untouched by its bless and a
     * prepared image by its preparation, and these are not. */
    if (payload_n > 0)
        say("%zu of these %s a bare EFI payload, and blessing one WRITES TO\n"
            "THAT FILE: the payload boots as itself, so there is nothing\n"
            "beside it for the signature to go into. Everything that will be\n"
            "written to is named above. Blessing an ISO is not like this; it\n"
            "leaves the ISO exactly as it is.\n",
            payload_n, (payload_n == 1u) ? "is" : "are");
    if (ui->show_cost) {
        char gib[32];

        gib_str(gib, sizeof(gib), total);
        say("It costs time: each image is read whole to hash it (the SHA-256\n"
            "names the staged folder) and then has its boot.wim repacked.\n"
            "That is %zu image%s, %s GiB to hash and repack. On USB it will\n"
            "take a while - minutes per image, much of it with nothing new\n"
            "on screen.\n", sel_n, (sel_n == 1u) ? "" : "s", gib);
    }
    say("type 'yes' to %s them, anything else to go back to the list: ",
        ui->act);
    if (read_line(answer, sizeof(answer)) < 0)
        return -1;
    if (strcmp(answer, "yes") != 0) {
        say("not confirmed; back to the list (the marks are kept).\n");
        return 0;
    }
    return 1;
}

/* Runs the whole ceremony over one queue. Returns 1 when a reviewed set was
 * confirmed, 0 when the user left without vouching for anything. The marks
 * live in q[].selected and start clear, for both gates: nothing is ever
 * pre-marked (DESIGN Sections 7.5 and 7.7). */
static int choose(candidate *q, size_t n, const gate_ui *ui)
{
    print_list(q, n, ui);
    say("\n%s\n"
        "  <n>       toggle image n      all, a / none, n  mark every / clear\n"
        "  list, l   reprint the list    done, d           review the marked set\n"
        "  quit, q   reboot without %s anything\n", ui->mark_lead, ui->acting);

    for (;;) {
        char line[64];
        size_t idx;

        say("%s", ui->prompt);
        if (read_line(line, sizeof(line)) < 0) {
            say("\nconsole closed; nothing was %s.\n", ui->acted);
            return 0;
        }
        /* An empty line is "done": it is what a user leans on Enter for, and
         * on an empty selection "done" only re-prompts, so it cannot act. */
        if (line[0] == '\0' || strcasecmp(line, "done") == 0 ||
            strcasecmp(line, "d") == 0) {
            size_t sel = selected_count(q, n);
            int r;

            if (sel == 0) {
                say("nothing is marked. Mark an image by number, or 'quit' to\n"
                    "leave without %s.\n", ui->acting);
                continue;
            }
            r = review(q, n, sel, ui);
            if (r > 0)
                return 1;
            if (r < 0) {
                say("\nconsole closed; nothing was %s.\n", ui->acted);
                return 0;
            }
            continue;
        }
        if (strcasecmp(line, "quit") == 0 || strcasecmp(line, "q") == 0) {
            say("leaving without %s anything.\n", ui->acting);
            return 0;
        }
        if (strcasecmp(line, "list") == 0 || strcasecmp(line, "l") == 0) {
            print_list(q, n, ui);
            continue;
        }
        if (strcasecmp(line, "all") == 0 || strcasecmp(line, "a") == 0) {
            size_t i;

            for (i = 0; i < n; i++)
                q[i].selected = 1;
            print_list(q, n, ui);
            say("all %zu marked; the review lists every one, read it.\n", n);
            continue;
        }
        if (strcasecmp(line, "none") == 0 || strcasecmp(line, "n") == 0) {
            size_t i;

            for (i = 0; i < n; i++)
                q[i].selected = 0;
            print_list(q, n, ui);
            continue;
        }
        {
            int r = parse_index(line, n, &idx);

            if (r > 0) {
                q[idx].selected = !q[idx].selected;
                print_item(q, idx, ui);
                continue;
            }
            if (r < 0) {
                say("there is no image %s; the list has %zu.\n", line, n);
                continue;
            }
        }
        say("not understood: '%s'. Type an image number, or all (a),\n"
            "none (n), list (l), done (d), quit (q).\n", line);
    }
}

/* ---- the preparation half (DESIGN Sections 6.3, 6.3.1 and 7.7) ----
 *
 * The batch is the unit of WORK and the gate is the unit of CONSENT, and both
 * of those are in the design at once. Section 6.3.1 wants the appliance to
 * prepare every unprepared Windows ISO in one boot, so dropping three images
 * costs one reboot rather than three. Section 7.7 wants patching to pass
 * through the same review-and-yes as signing, because it is the same act of
 * authorising a modification to media the user did not produce: "what the
 * review shows differs; that it is shown, and that the user says yes to it,
 * does not."
 *
 * So the whole batch is released by ONE gate, and the gate is the bless one:
 * nothing pre-marked, the user marks images, every path on screen at the
 * moment of confirmation, a typed "yes" over the review.
 *
 * Regression it fixes, recorded because the reasoning that lost it was
 * plausible: the earlier version treated "preparation vouches for nothing, it
 * holds no key and the platform verifies nothing it touches" as a reason to
 * skip the gate, and read the loader's prepare-offer as the confirmation. On
 * real hardware 2026-09-13 that meant an appliance that booted and immediately
 * began hashing two multi-gigabyte Windows ISOs, having asked nothing and said
 * nothing about the cost. Needing no key is not the same thing as needing no
 * consent.
 */

/* Room left on p2, in bytes; 0 when it cannot be asked. Used only to make a
 * failure legible, never to decide anything: a threshold here would be a
 * guess about an image whose size is not known until wimlib has rewritten
 * it, and prepare's own ENOSPC is the honest answer. */
static uint64_t p2_free_bytes(void)
{
    struct statvfs s;

    if (statvfs(P2_MNT, &s) != 0)
        return 0;
    return (uint64_t)s.f_bavail * (uint64_t)s.f_frsize;
}

/* Below this, a full stick is the likeliest explanation of a failure and is
 * worth naming: DESIGN Section 6.3 measures preparation at about 1.2 GB of
 * scratch plus the roughly 570 MB patched image it leaves behind. */
#define PREP_TIGHT_BYTES ((uint64_t)2u << 30)

/* Whether this image can prepare at all. A build that dropped one of these
 * would otherwise fail per ISO with a message about the ISO; missing 7z is
 * worse still, because Windows media is UDF-only and without a UDF reader it
 * classifies as an unknown Linux image and lands in the wrong queue.
 *
 * PID 1 in an initramfs inherits no environment, so these lookups run against
 * sgn_have_program's own "/usr/bin:/bin" fallback rather than against a PATH.
 * That is where mk-appliance.sh stages both programs, and it is the head of
 * the PATH every child gets (g_child_env), so the two agree; if the staging
 * layout ever moves, this and that list move together. */
static int prep_tools_present(void)
{
    int ok = 1;

    if (!sgn_have_program(WIM_TOOL)) {
        say("WARNING: %s is not in this appliance image, so no Windows ISO\n"
            "         can be prepared on this stick.\n", WIM_TOOL);
        ok = 0;
    }
    if (!sgn_have_program(Z_TOOL)) {
        say("WARNING: %s is not in this appliance image. Windows install\n"
            "         media keeps its files in the UDF namespace only, so\n"
            "         without it such an ISO reads as empty and is listed\n"
            "         below as unsigned Linux media rather than prepared.\n",
            Z_TOOL);
        ok = 0;
    }
    if (access(WINJUMP_PATH, R_OK) != 0) {
        say("WARNING: %s is missing, so preparation has nothing to put in\n"
            "         place of WinPE's shell.\n", WINJUMP_PATH);
        ok = 0;
    }
    if (!ok)
        say("         Rebuild the appliance with tools/appliance/"
            "mk-appliance.sh.\n\n");
    return ok;
}

/* How often the wait for one prepare says it is still going. The hash of a
 * multi-gigabyte ISO is the longest stretch the tool itself has nothing to
 * print during, and a console that has been silent for four minutes is what
 * a hang looks like. Half a minute is often enough to read as alive and rare
 * enough not to bury the tool's own lines. */
#define PREP_BEAT_S 30u

/* Prepares the marked Windows images, in path order, reporting each. One
 * image failing is that image's problem and the batch carries on: the reboot
 * this run ends in is the expensive part, and abandoning the rest of the
 * queue would charge the user another one per bad ISO.
 *
 * Runs only over what the gate confirmed; an unmarked image is a skip, and is
 * counted as one so the summary accounts for every image the survey found. */
static void prepare_batch(size_t *prepared_n, size_t *failed_n,
                          size_t *skipped_n)
{
    size_t sel_n = selected_count(g_prep, g_prep_n);
    size_t done_n = 0;
    size_t i;

    say("\nFALCON-APPLIANCE-PREP-SELECTED %zu\n", sel_n);

    for (i = 0; i < g_prep_n; i++) {
        char abs[PATH_MAX];
        char rel_arg[MANIFEST_PATH_MAX + 2u];
        uint64_t before;
        int rc;

        if (!g_prep[i].selected) {
            (*skipped_n)++;
            continue;
        }
        done_n++;

        /* The precision restates what add_to already guaranteed (the path
         * fits the field, terminator included), spelled out so the bound is
         * one the compiler checks rather than one a reader has to trust. */
        (void)snprintf(abs, sizeof(abs), "%s/%.*s", P2_MNT,
                       (int)(sizeof(g_prep[i].rel) - 1u), g_prep[i].rel);
        (void)snprintf(rel_arg, sizeof(rel_arg), "/%.*s",
                       (int)(sizeof(g_prep[i].rel) - 1u), g_prep[i].rel);

        say("\n[%zu/%zu] /%s  (%llu MiB)\n", done_n, sel_n, g_prep[i].rel,
            (unsigned long long)(g_prep[i].size / MIB));
        if (g_prep[i].state == MANIFEST_BLESS_STALE)
            say("      %s\n", g_prep_ui.stale_note);
        /* Named phase by phase, because the two are minutes apart in cost and
         * a user watching a still console needs to know which one it is in.
         * The tool announces the repack; the hash it announces once and then
         * has nothing to say for, which is what the beat below covers. */
        say("      hashing %llu MiB, then repacking boot.wim; the tool's own\n"
            "      progress follows. The hash is the long quiet part.\n",
            (unsigned long long)(g_prep[i].size / MIB));

        before = p2_free_bytes();
        {
            char *argv[] = {
                (char *)SIGN_TOOL, (char *)"prepare",
                abs,
                (char *)"--stick", (char *)P2_MNT,
                (char *)"--iso-path", rel_arg,
                NULL
            };

            rc = run(argv, NULL, 0, PREP_BEAT_S, NULL);
        }
        if (rc == 0) {
            say("      FALCON-APPLIANCE-PREPARED %s\n", g_prep[i].rel);
            (*prepared_n)++;
            continue;
        }
        say("      FALCON-APPLIANCE-PREP-FAILED %s (exit %d)\n",
            g_prep[i].rel, rc);
        if (before < PREP_TIGHT_BYTES)
            say("      p2 had %llu MiB free when this started, which is the\n"
                "      first thing to check: one prepared image costs about\n"
                "      600 MiB there and the run needs roughly 1.2 GiB more\n"
                "      while it works.\n",
                (unsigned long long)(before / MIB));
        (*failed_n)++;
    }
}

/* The gate, then the batch. Nothing here reads a key or touches p3, and the
 * gate runs before the bless half for the same reason the batch does: an
 * auto-prep boot must cost no passphrase (DESIGN Section 6.3.1). */
static void prepare_half(size_t *prepared_n, size_t *failed_n,
                         size_t *skipped_n)
{
    say("\n%zu Windows image%s on p2 %s not prepared yet. Preparing signs\n"
        "nothing and needs no key, so p3 stays locked for it.\n",
        g_prep_n, (g_prep_n == 1u) ? "" : "s",
        (g_prep_n == 1u) ? "is" : "are");
    say("p2 has %llu MiB free.\n",
        (unsigned long long)(p2_free_bytes() / MIB));
    say("\nFALCON-APPLIANCE-PREP-PENDING %zu\n", g_prep_n);

    if (!choose(g_prep, g_prep_n, &g_prep_ui)) {
        *skipped_n = g_prep_n;
        return;
    }
    prepare_batch(prepared_n, failed_n, skipped_n);
}

/* ---- teardown ---- */

/* Unwound in the order that can actually succeed: p2 first (nothing else
 * depends on it), then p3's mount, then the mapping that carried it. A
 * failure here is reported and pressed on with, because the next step is a
 * reboot either way and leaving the user staring at a half-closed stick
 * helps nobody. */
static void teardown(int p2_mounted, int p3_mounted, int mapped)
{
    if (p2_mounted && umount(P2_MNT) != 0)
        say("note: could not unmount p2: %s\n", strerror(errno));
    if (p3_mounted && umount(P3_MNT) != 0)
        say("note: could not unmount p3: %s\n", strerror(errno));
    if (mapped) {
        char *argv[] = { (char *)"/usr/bin/cryptsetup", (char *)"close",
                         (char *)MAPPER, NULL };

        if (run(argv, NULL, 0, 0, NULL) != 0)
            say("note: could not close the p3 mapping\n");
    }
    sync();
}

static void finish(int p2_mounted, int p3_mounted, int mapped)
{
    char line[8];

    /* The summary (or an error) is the last thing the user needs to read, and
     * an immediate reboot snatches it off the screen. Waiting on Enter costs a
     * keystroke. A failed read now means every console this machine has went
     * quiet, not that the one console was the wrong one (see the console
     * section above), but it must still not fall straight through: the case
     * where nothing can be read is exactly the case where a screen someone is
     * reading may be all there is. A run with nobody at it ends on its own,
     * just later. */
    say("\npress Enter to reboot to the loader.\n");
    if (read_line(line, sizeof(line)) < 0) {
        say("(no readable console; rebooting in 30 seconds)\n");
        (void)sleep(30);
    }
    say("rebooting.\n");
    teardown(p2_mounted, p3_mounted, mapped);
    (void)sleep(1);
    (void)reboot(RB_AUTOBOOT);
    for (;;)
        (void)pause();
}

/* "Already blessed" for a bare payload, applied to the queue the survey built.
 *
 * It is a property of the file rather than of a record: our exact certificate
 * in the PE's certificate table, which is the tool's own idempotence rule
 * (src/cli/sign/payload.h) rather than a second answer invented here. The
 * survey cannot ask it, because the certificate that defines "ours" lives on
 * p3 and p3 is still locked while p2 is walked. So it is asked once, here,
 * between the unlock and the listing the user marks. Asking it earlier would
 * mean reading a certificate from somewhere unencrypted and letting that
 * decide what this stick signs with, which is a second and editable answer to
 * a question p3 already answers.
 *
 * A .efi is asked directly. A payload inside a .img is not: the question
 * lives inside the image, sgn_payload_signed_with takes a path, and reaching
 * those paths means the volume walk payload.c already implements. Rather than
 * a second copy of it, such an image keeps its place in the list and the tool
 * answers per volume when it runs - it signs nothing there and says "already
 * signed with this key, left alone", which reaches the console with the rest
 * of its output. */
static void payload_prefilter(void)
{
    uint8_t *der = NULL;
    size_t der_len = 0;
    size_t kept = 0;
    size_t i;

    if (sgn_payload_cert_der(CERT_PATH, &der, &der_len) != SGN_OK) {
        say("note: %s could not be read as a certificate (%s), so no payload\n"
            "      can be recognised as already carrying it.\n",
            CERT_PATH, sgn_error());
        return;
    }

    for (i = 0; i < g_cand_n; i++) {
        appl_ours ours = APPL_OURS_UNKNOWN;

        if (g_cand[i].shape == SGN_PAYLOAD_EFI) {
            char abs[PATH_MAX];

            (void)snprintf(abs, sizeof(abs), "%s/%.*s", P2_MNT,
                           (int)(sizeof(g_cand[i].rel) - 1u), g_cand[i].rel);
            ours = sgn_payload_signed_with(abs, der, der_len) ? APPL_OURS_YES
                                                              : APPL_OURS_NO;
        }
        if (g_cand[i].shape != SGN_PAYLOAD_NONE &&
            appl_route_payload(ours) == APPL_JOB_DONE_BLESSED) {
            say("  blessed     /%s (already; this stick's certificate is in "
                "it)\n", g_cand[i].rel);
            g_done_blessed++;
            continue;
        }
        g_cand[kept++] = g_cand[i];
    }
    g_cand_n = kept;
    free(der);
}

/* ---- the bless half (DESIGN Section 7.5) ----
 *
 * Everything that needs the key lives in here, and nothing calls it unless
 * the survey found an image that needs one. Failures report and RETURN rather
 * than rebooting: a preparation batch may already have succeeded, and its
 * summary is still owed to the user.
 *
 * *mapped and *p3_mounted are set as each is opened, so the caller can unwind
 * whatever this got as far as. sgn_arg launders the one const member that has
 * to reach an execve argument vector; nothing writes through it.
 */
static void bless_half(const stick_parts *parts, int *mapped, int *p3_mounted,
                       size_t *signed_n, size_t *failed_n, size_t *skipped_n)
{
    char pass[PASS_MAX];
    char dev[32];
    char dev_arg[48];
    int need_dev = 0;
    int tries;
    size_t i;

    dev_arg[0] = '\0';

    /* ---- unlock p3 ----
     *
     * The passphrase goes to cryptsetup down a pipe as --key-file -, so it
     * never appears in argv, in the environment, or in any file. --readonly
     * because the appliance has no business writing to the shelf: the key is
     * read, the outputs go to p2.
     *
     * First attempt: the credential the loader delivered. The user already
     * typed the passphrase once to reach the chooser, and the Section 5.1
     * machinery hands it to every UKI the loader launches, this one
     * included; asking again would be the double prompt the whole handoff
     * design exists to remove. The interactive loop below stays as the
     * fallback for a missing or stale credential (a buffer-load fallback
     * launch delivers none), so a failed credential costs one message, not
     * the session. */
    {
        ssize_t clen = read_file("/.extra/credentials/" CRED_FILE,
                                 pass, sizeof(pass));

        if (clen > 0) {
            char *argv[] = {
                (char *)"/usr/bin/cryptsetup", (char *)"open",
                (char *)"--type", (char *)"luks2",
                (char *)"--readonly",
                (char *)"--key-file", (char *)"-",
                sgn_arg(parts->p3), (char *)MAPPER, NULL
            };

            say("unlocking p3 with the passphrase from the loader...\n");
            if (run(argv, pass, (size_t)clen, 0, NULL) == 0)
                *mapped = 1;
            else
                say("the delivered credential did not open p3; "
                    "falling back to the prompt.\n");
        }
        explicit_bzero(pass, sizeof(pass));
    }
    for (tries = 1; !*mapped && tries <= UNLOCK_TRIES; tries++) {
        char *argv[] = {
            (char *)"/usr/bin/cryptsetup", (char *)"open",
            (char *)"--type", (char *)"luks2",
            (char *)"--readonly",
            (char *)"--key-file", (char *)"-",
            sgn_arg(parts->p3), (char *)MAPPER, NULL
        };
        ssize_t len;
        int rc;

        say("passphrase for p3 (attempt %d of %d): ", tries, UNLOCK_TRIES);
        len = read_secret(pass, sizeof(pass));
        if (len <= 0) {
            say("no passphrase given; nothing was unlocked.\n");
            explicit_bzero(pass, sizeof(pass));
            *skipped_n = g_cand_n;
            return;
        }
        rc = run(argv, pass, (size_t)len, 0, NULL);
        explicit_bzero(pass, sizeof(pass));
        if (rc == 0) {
            *mapped = 1;
            break;
        }
        say("wrong passphrase (cryptsetup exit %d).\n", rc);
    }
    if (!*mapped) {
        say("\nout of attempts. p3 stays locked and no key was read.\n");
        *skipped_n = g_cand_n;
        return;
    }
    say("p3 unlocked (read-only).\n");

    if (mount(MAPPER_NODE, P3_MNT, "ext4", MS_RDONLY, NULL) != 0) {
        say("ERROR: mounting p3: %s\n", strerror(errno));
        *skipped_n = g_cand_n;
        return;
    }
    *p3_mounted = 1;

    if (access(KEY_PATH, R_OK) != 0 || access(CERT_PATH, R_OK) != 0) {
        say("ERROR: no MOK keypair on p3 (%s / %s).\n"
            "       A stick built with --p3-plain has no on-stick key, so it\n"
            "       cannot bless anything here; sign from a host instead.\n"
            "       Preparing Windows media needs no key and is unaffected.\n",
            KEY_PATH, CERT_PATH);
        *skipped_n = g_cand_n;
        return;
    }
    say("MOK keypair found on p3.\n");

    payload_prefilter();
    if (g_cand_n == 0) {
        say("\nnothing left to sign: every payload on p2 already carries this\n"
            "stick's certificate. FALCON-APPLIANCE-PENDING 0\n");
        return;
    }

    /* Needed only by the wrap path: it is the %DEV% the signed cmdline
     * carries, and neither preparation nor a bare payload has a cmdline to
     * put it in. A queue of payloads alone must not die on a volume serial
     * nothing in it would use. */
    for (i = 0; i < g_cand_n; i++)
        need_dev |= (g_cand[i].shape == SGN_PAYLOAD_NONE);
    if (need_dev) {
        if (exfat_uuid(parts->p2, dev, sizeof(dev)) != 0) {
            say("ERROR: could not read p2's volume serial; the signed cmdline\n"
                "       would have no way to name the partition to boot "
                "from.\n");
            *skipped_n = g_cand_n;
            return;
        }
        (void)snprintf(dev_arg, sizeof(dev_arg), "UUID=%s", dev);
        say("p2 volume: %s\n", dev_arg);
    }

    say("\nFALCON-APPLIANCE-PENDING %zu\n", g_cand_n);

    /* ---- mark, review, confirm, then the bless ----
     *
     * DESIGN Section 7.5: the marks are the per-image vouch and the typed
     * "yes" over the reviewed set releases the batch. Nothing below runs
     * unless both happened. */
    if (!choose(g_cand, g_cand_n, &g_bless_ui)) {
        /* Nothing was vouched for, so every pending image is a skip. */
        *skipped_n = g_cand_n;
        return;
    }

    {
        size_t sel_n = selected_count(g_cand, g_cand_n);
        size_t done_n = 0;

        say("\nFALCON-APPLIANCE-SELECTED %zu\n", sel_n);

        for (i = 0; i < g_cand_n; i++) {
            char abs[PATH_MAX];
            char rel_arg[MANIFEST_PATH_MAX + 2u];
            int rc;

            if (!g_cand[i].selected) {
                (*skipped_n)++;
                continue;
            }
            done_n++;

            /* The precision restates what add_to already guaranteed (the path
             * fits the field, terminator included). It is spelled out here
             * because the bound is what keeps a p2 path from running off the
             * end of either buffer, and a bound stated in the call is one the
             * compiler checks rather than one a reader has to trust. */
            (void)snprintf(abs, sizeof(abs), "%s/%.*s", P2_MNT,
                           (int)(sizeof(g_cand[i].rel) - 1u), g_cand[i].rel);
            (void)snprintf(rel_arg, sizeof(rel_arg), "/%.*s",
                           (int)(sizeof(g_cand[i].rel) - 1u), g_cand[i].rel);

            say("\n[%zu/%zu] /%s\n", done_n, sel_n, g_cand[i].rel);
            if (g_cand[i].shape != SGN_PAYLOAD_NONE) {
                /* No --iso-path, --dev or --stub: there is no wrap to build,
                 * so the tool rejects them rather than ignoring them
                 * (src/cli/sign/main.c, bless_payload). Its output is echoed
                 * for the same reason the review echoed the dry run - this is
                 * the run that writes to the user's file, and what it did
                 * belongs on the console that asked them about it. */
                char *argv[] = {
                    (char *)SIGN_TOOL,
                    abs,
                    (char *)"--stick", (char *)P2_MNT,
                    (char *)"--key", (char *)KEY_PATH,
                    (char *)"--cert", (char *)CERT_PATH,
                    NULL
                };

                say("      signing it in place...\n");
                rc = run(argv, NULL, 0, 0, "      ");
            } else {
                char *argv[] = {
                    (char *)SIGN_TOOL,
                    abs,
                    (char *)"--stick", (char *)P2_MNT,
                    (char *)"--key", (char *)KEY_PATH,
                    (char *)"--cert", (char *)CERT_PATH,
                    (char *)"--iso-path", rel_arg,
                    (char *)"--dev", dev_arg,
                    (char *)"--stub", (char *)STUB_PATH,
                    NULL
                };

                say("      signing...\n");
                rc = run(argv, NULL, 0, 0, NULL);
            }
            /* One image failing is that image's problem: the rest of the batch
             * was vouched for too and still deserves to be signed. */
            if (rc == 0) {
                say("      FALCON-APPLIANCE-SIGNED %s\n", g_cand[i].rel);
                (*signed_n)++;
            } else {
                say("      FALCON-APPLIANCE-FAILED %s (exit %d)\n",
                    g_cand[i].rel, rc);
                (*failed_n)++;
            }
        }
    }
}

/* ---- the flow ---- */

int main(void)
{
    stick_parts parts;
    int mapped = 0;
    int p3_mounted = 0;
    int p2_mounted = 0;
    size_t prepared_n = 0;
    size_t prep_failed_n = 0;
    size_t prep_skipped_n = 0;
    size_t signed_n = 0;
    size_t failed_n = 0;
    size_t skipped_n = 0;
    struct rlimit no_core = { 0, 0 };

    /* Before anything reads a passphrase: this process is about to hold one,
     * and a core file would be a copy of it on whatever filesystem is current. */
    (void)setrlimit(RLIMIT_CORE, &no_core);

    console_open();
    early_mounts();

    say("\n"
        "falcon-boot signing appliance\n"
        "=============================\n"
        "Prepares Windows install media so Setup can find its own ISO again,\n"
        "and signs everything else on p2 - ISOs, and the bare EFI payloads\n"
        "beside them - with the MOK private key encrypted on p3.\n\n");

    /* The sign tool's ISO reader is linked in here for the survey. Its own
     * diagnostics are about listing a file, which is this program's plumbing
     * rather than its output, so only warnings and errors reach the console.
     * The tool's INFO-level progress during a real prepare or bless still
     * does: that runs in a child process with its own threshold. */
    sgn_log_set_level(SGN_LOG_WARN);

    load_modules();

    if (!find_stick(&parts)) {
        say("ERROR: no falcon stick found (need an exFAT p2 and a LUKS p3 on\n"
            "       one disk). Nothing was unlocked.\n");
        say_block_devices();
        finish(0, 0, 0);
    }
    say("  p2 (ISO library): %s\n  p3 (UKI shelf):   %s\n\n",
        parts.p2, parts.p3);

    /* ---- the survey ----
     *
     * p2 and nothing else. Every input the survey and the preparation half
     * read lives there, so p3 is left alone until the bless half below has
     * been shown to have work: an auto-prep boot (DESIGN Section 6.3.1) then
     * decrypts no key and asks for no passphrase at all. */
    if (mount(parts.p2, P2_MNT, "exfat", 0, NULL) != 0) {
        say("ERROR: mounting p2: %s\n", strerror(errno));
        finish(0, 0, 0);
    }
    p2_mounted = 1;

    (void)prep_tools_present();

    load_manifest();
    say("surveying p2...\n");
    walk(P2_MNT, "", 0);

    /* Path order once, here, so the numbers the user types, the order the
     * review prints and the order either batch runs in are all the same one. */
    qsort(g_cand, g_cand_n, sizeof(g_cand[0]), cand_cmp);
    qsort(g_prep, g_prep_n, sizeof(g_prep[0]), cand_cmp);

    say("survey: %zu to prepare, %zu to sign, %zu already prepared,\n"
        "        %zu already blessed, %zu unreadable\n",
        g_prep_n, g_cand_n, g_done_prepared, g_done_blessed, g_unreadable);

    /* A mixed run proposes two different operations, so it says so once,
     * before the first gate, rather than leaving the user to work out why
     * they are being asked twice. */
    if (g_prep_n > 0 && g_cand_n > 0)
        say("\nTwo kinds of work are waiting, and each is asked about on its\n"
            "own: %zu Windows image%s to prepare (patched, never signed, no\n"
            "key) and %zu image%s to sign with the MOK key on p3. Saying yes\n"
            "to one is not saying yes to the other.\n",
            g_prep_n, (g_prep_n == 1u) ? "" : "s",
            g_cand_n, (g_cand_n == 1u) ? "" : "s");

    /* Preparation first, and while p3 is still locked. */
    if (g_prep_n > 0)
        prepare_half(&prepared_n, &prep_failed_n, &prep_skipped_n);

    if (g_cand_n == 0)
        say("\nnothing to sign: nothing on p2 is waiting for the MOK key, so\n"
            "p3 stays locked. FALCON-APPLIANCE-PENDING 0\n");
    else
        bless_half(&parts, &mapped, &p3_mounted, &signed_n, &failed_n,
                   &skipped_n);

    say("\nsummary: FALCON-APPLIANCE-SUMMARY prepared=%zu prep-failed=%zu "
        "prep-skipped=%zu signed=%zu failed=%zu skipped=%zu\n",
        prepared_n, prep_failed_n, prep_skipped_n, signed_n, failed_n,
        skipped_n);

    finish(p2_mounted, p3_mounted, mapped);
    return 0;   /* not reached; finish() reboots */
}
