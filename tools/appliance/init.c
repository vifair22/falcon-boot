/* falcon-boot: the on-stick signing appliance's PID 1 (DESIGN Section 7.5,
 * build order step 8).
 *
 * The appliance is a signed UKI that boots under full Secure Boot, unlocks p3
 * with the user's passphrase, reads the MOK private key that lives there, has
 * the user mark the unsigned ISOs on p2 one by one and then confirm the marked
 * set as a whole, and hands each confirmed one to the SAME `falcon-boot-sign`
 * implementation the host route runs (here the static link,
 * /usr/bin/falcon-boot-sign-static). Then it reboots. That is the whole
 * program; there is no shell in this initramfs and nothing else to run.
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
 *   - The passphrase never appears in argv (it goes to cryptsetup on a pipe),
 *     never reaches a file, and is wiped from this process's memory as soon
 *     as the unlock returns. Core dumps are disabled at entry for the same
 *     reason.
 *   - p3 is unlocked and mounted READ-ONLY. Nothing in the signing flow writes
 *     to the shelf: the UKI wrap and the manifest record both land on p2. A
 *     read-only mapping means a bug in anything downstream cannot damage the
 *     one copy of the key.
 *   - Nothing is signed that the user did not mark by hand and then confirm.
 *     The per-image mark plus the typed "yes" over the reviewed list together
 *     ARE the "I vouch for these" act the design rests on: they are what stops
 *     an ISO someone dropped on the stick from being blessed because it
 *     happened to be sitting there. The list is never pre-marked, and the
 *     confirmation prints every path it is about to sign, so nothing can be
 *     swept in unseen (DESIGN Section 7.5, select-then-sign).
 *   - There is no network in this initramfs (no interface is brought up, and
 *     nothing here would use one), so the decrypted-key window cannot be
 *     reached from outside the machine.
 *
 * The partitions are identified out of the GPT through src/part/part.c, the
 * same parser AND the same matcher (part_match_role) the loader uses, so the
 * appliance and the loader cannot disagree about which partition is which. The manifest is read
 * through src/manifest/manifest.c, the same parser the loader will use, so
 * "already blessed" means here exactly what it will mean at the menu.
 *
 * Built static by tools/appliance/mk-appliance.sh: it is PID 1 in an
 * initramfs whose dynamic loader exists for the other binaries, not for this
 * one.
 */

/* _GNU_SOURCE comes from the host flag set in the Makefile (HOST_CFLAGS),
 * which mk-appliance.sh copies verbatim. */

#include <manifest/manifest.h>
#include <part/part.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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
 * kernel builds ext4 in but ships device-mapper and exFAT as modules, so the
 * appliance carries them rather than assuming: without dm-crypt there is no
 * unlock, and without exfat there is no p2.
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

/* ---- console ---- */

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)vprintf(fmt, ap);
    va_end(ap);
    (void)fflush(stdout);
}

/* The kernel gives an initramfs no console of its own, so /dev has to exist
 * before the first print or every message goes to a closed descriptor and the
 * run looks like a hang (the same rule tools/uki-init/init.c records). */
static void console_open(void)
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

static int find_stick(stick_parts *out)
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

/* fork/exec with our own environment and, optionally, `in` on stdin. No
 * shell anywhere: every argument is passed as a vector, so nothing a file
 * name contains can become syntax. Returns the exit status, or -1. */
static int run(char *const argv[], const char *in, size_t in_len)
{
    int pipefd[2] = { -1, -1 };
    pid_t pid;
    int status = 0;

    if (in != NULL && pipe(pipefd) != 0) {
        say("ERROR: pipe: %s\n", strerror(errno));
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        say("ERROR: fork: %s\n", strerror(errno));
        if (in != NULL) {
            (void)close(pipefd[0]);
            (void)close(pipefd[1]);
        }
        return -1;
    }
    if (pid == 0) {
        if (in != NULL) {
            (void)close(pipefd[1]);
            (void)dup2(pipefd[0], 0);
            (void)close(pipefd[0]);
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
    if (waitpid(pid, &status, 0) < 0)
        return -1;
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
    struct termios old;
    struct termios quiet;
    int have_tty = (tcgetattr(0, &old) == 0);
    size_t len = 0;

    if (have_tty) {
        quiet = old;
        quiet.c_lflag &= (tcflag_t)~(tcflag_t)ECHO;
        (void)tcsetattr(0, TCSAFLUSH, &quiet);
    }

    for (;;) {
        char c;
        ssize_t n = read(0, &c, 1);

        if (n <= 0) {
            len = 0;
            break;
        }
        if (c == '\n' || c == '\r')
            break;
        if (len + 1u < cap)
            buf[len++] = c;
    }

    if (have_tty)
        (void)tcsetattr(0, TCSAFLUSH, &old);
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
 * word they are typing, because typing it is the whole ceremony. */
static ssize_t read_line(char *buf, size_t cap)
{
    size_t len = 0;

    for (;;) {
        char c;
        ssize_t n = read(0, &c, 1);

        if (n <= 0)
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
    char           rel[MANIFEST_PATH_MAX];  /* path relative to the p2 root */
    uint64_t       size;
    manifest_bless state;                   /* NONE or STALE; FRESH is skipped */
    int            selected;                /* marked by hand; never preset */
} candidate;

static manifest_record g_records[MAX_RECORDS];
static size_t          g_record_n;
static candidate       g_cand[MAX_CANDIDATES];
static size_t          g_cand_n;
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

static void add_candidate(const char *rel, uint64_t size, manifest_bless st)
{
    if (g_cand_n >= MAX_CANDIDATES) {
        say("note: more than %u pending ISOs; the rest are not listed\n",
            (unsigned)MAX_CANDIDATES);
        return;
    }
    if (snprintf(g_cand[g_cand_n].rel, sizeof(g_cand[g_cand_n].rel), "%s",
                 rel) >= (int)sizeof(g_cand[g_cand_n].rel)) {
        say("note: skipping '%s': its path is too long for a manifest record\n",
            rel);
        return;
    }
    g_cand[g_cand_n].size = size;
    g_cand[g_cand_n].state = st;
    g_cand[g_cand_n].selected = 0;
    g_cand_n++;
}

/* Recursive walk of p2 looking for *.iso. `rel` is the path so far relative
 * to the p2 root, which is the form the manifest records and the form `sign`
 * embeds in the signed cmdline. */
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
        if (!S_ISREG(st.st_mode) || !ends_with_iso(e->d_name))
            continue;

        {
            uint64_t size = (uint64_t)st.st_size;
            manifest_bless state = manifest_lookup(g_records, g_record_n,
                                                   child_rel, size, NULL);

            if (state == MANIFEST_BLESS_FRESH)
                continue;   /* already blessed, and the ISO has not changed */
            add_candidate(child_rel, size, state);
        }
    }
    (void)closedir(d);
}

/* ---- the pending list and the selection ceremony ----
 *
 * DESIGN Section 7.5 (select-then-sign, 2026-09-13): the user marks the images
 * one at a time, the whole marked set is shown back once, and only a typed
 * "yes" over that reviewed set starts any signing. The listing is grouped by
 * folder so it reads like the loader's own folder-view menu (Section 6 stage
 * 1); the grouping is presentation only, everything below still works on the
 * full p2-relative path. */

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

static size_t selected_count(void)
{
    size_t i;
    size_t n = 0;

    for (i = 0; i < g_cand_n; i++) {
        if (g_cand[i].selected)
            n++;
    }
    return n;
}

/* One image's line. Printed on its own after a toggle so a dumb serial console
 * does not have to redraw the whole list for every keystroke. */
static void print_item(size_t i)
{
    say("    [%c] %2zu  %s  (%llu MiB)\n", g_cand[i].selected ? 'x' : ' ',
        i + 1u, base_of(g_cand[i].rel),
        (unsigned long long)(g_cand[i].size / (1024u * 1024u)));
    if (g_cand[i].state == MANIFEST_BLESS_STALE)
        say("           blessed earlier; the file has changed since\n");
}

static void print_list(void)
{
    char dir[MANIFEST_PATH_MAX];
    char cur[MANIFEST_PATH_MAX];
    size_t i;
    int have_cur = 0;

    cur[0] = '\0';
    say("\npending unsigned images:\n\n");
    for (i = 0; i < g_cand_n; i++) {
        dir_of(dir, sizeof(dir), g_cand[i].rel);
        if (!have_cur || strcmp(dir, cur) != 0) {
            /* "/" for the root, "Linux/" for a folder: one shape, and the
             * trailing slash is what says "this is a folder, not an image". */
            say("  %s/\n", dir);
            (void)snprintf(cur, sizeof(cur), "%s", dir);
            have_cur = 1;
        }
        print_item(i);
    }
}

/* A bare 1-based image number, and nothing else. Returns 1 and the 0-based
 * index for a number in range, -1 for a number that names no image, 0 for
 * anything that is not a number. Deliberately strict: "2 " or "2y" is a typo,
 * and guessing what a typo meant is not a thing to do in front of a signing
 * key. */
static int parse_index(const char *s, size_t *out)
{
    unsigned long v = 0;
    size_t i;

    if (s[0] == '\0')
        return 0;
    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = v * 10u + (unsigned long)(s[i] - '0');
        if (v > (unsigned long)g_cand_n)
            return -1;   /* keeps the accumulator from running away too */
    }
    if (v == 0u)
        return -1;
    *out = (size_t)v - 1u;
    return 1;
}

/* The batch review. Prints every marked path in full and takes the one word
 * that authorises the batch. 1 = confirmed, 0 = back to the list (a typo must
 * not cost the marks), -1 = the console closed. */
static int review(size_t n)
{
    char answer[32];
    size_t i;

    say("\nreview: %zu image%s marked\n\n", n, (n == 1u) ? "" : "s");
    for (i = 0; i < g_cand_n; i++) {
        if (g_cand[i].selected)
            say("  /%s\n", g_cand[i].rel);
    }
    say("\nSigning these says you trust every one of them to run on this\n"
        "machine with Secure Boot on.\n"
        "type 'yes' to sign them, anything else to go back to the list: ");
    if (read_line(answer, sizeof(answer)) < 0)
        return -1;
    if (strcmp(answer, "yes") != 0) {
        say("not confirmed; back to the list (the marks are kept).\n");
        return 0;
    }
    return 1;
}

/* Runs the whole ceremony. Returns 1 when a reviewed set was confirmed, 0 when
 * the user left without vouching for anything. The marks live in
 * g_cand[].selected and start clear. */
static int choose_candidates(void)
{
    print_list();
    say("\nMark what you vouch for, then review it:\n"
        "  <n>       toggle image n      all, a / none, n  mark every / clear\n"
        "  list, l   reprint the list    done, d           review the marked set\n"
        "  quit, q   reboot without signing anything\n");

    for (;;) {
        char line[64];
        size_t idx;

        say("select> ");
        if (read_line(line, sizeof(line)) < 0) {
            say("\nconsole closed; nothing was signed.\n");
            return 0;
        }
        /* An empty line is "done": it is what a user leans on Enter for, and
         * on an empty selection "done" only re-prompts, so it cannot sign. */
        if (line[0] == '\0' || strcasecmp(line, "done") == 0 ||
            strcasecmp(line, "d") == 0) {
            size_t n = selected_count();
            int r;

            if (n == 0) {
                say("nothing is marked. Mark an image by number, or 'quit' to\n"
                    "leave without signing.\n");
                continue;
            }
            r = review(n);
            if (r > 0)
                return 1;
            if (r < 0) {
                say("\nconsole closed; nothing was signed.\n");
                return 0;
            }
            continue;
        }
        if (strcasecmp(line, "quit") == 0 || strcasecmp(line, "q") == 0) {
            say("leaving without signing anything.\n");
            return 0;
        }
        if (strcasecmp(line, "list") == 0 || strcasecmp(line, "l") == 0) {
            print_list();
            continue;
        }
        if (strcasecmp(line, "all") == 0 || strcasecmp(line, "a") == 0) {
            size_t i;

            for (i = 0; i < g_cand_n; i++)
                g_cand[i].selected = 1;
            print_list();
            say("all %zu marked; the review lists every one, read it.\n",
                g_cand_n);
            continue;
        }
        if (strcasecmp(line, "none") == 0 || strcasecmp(line, "n") == 0) {
            size_t i;

            for (i = 0; i < g_cand_n; i++)
                g_cand[i].selected = 0;
            print_list();
            continue;
        }
        {
            int r = parse_index(line, &idx);

            if (r > 0) {
                g_cand[idx].selected = !g_cand[idx].selected;
                print_item(idx);
                continue;
            }
            if (r < 0) {
                say("there is no image %s; the list has %zu.\n", line,
                    g_cand_n);
                continue;
            }
        }
        say("not understood: '%s'. Type an image number, or all (a),\n"
            "none (n), list (l), done (d), quit (q).\n", line);
    }
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

        if (run(argv, NULL, 0) != 0)
            say("note: could not close the p3 mapping\n");
    }
    sync();
}

static void finish(int p2_mounted, int p3_mounted, int mapped)
{
    char line[8];

    /* The summary (or an error) is the last thing the user needs to read,
     * and an immediate reboot snatches it off the screen. Waiting on Enter
     * costs a keystroke; EOF (a dead console) falls through so a headless
     * run still ends on its own. */
    say("\npress Enter to reboot to the loader.\n");
    (void)read_line(line, sizeof(line));
    say("rebooting.\n");
    teardown(p2_mounted, p3_mounted, mapped);
    (void)sleep(1);
    (void)reboot(RB_AUTOBOOT);
    for (;;)
        (void)pause();
}

/* ---- the flow ---- */

int main(void)
{
    char pass[PASS_MAX];
    stick_parts parts;
    char dev[32];
    char dev_arg[48];
    int mapped = 0;
    int p3_mounted = 0;
    int p2_mounted = 0;
    int tries;
    size_t i;
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
        "Signs ISOs on p2 with the MOK private key that lives encrypted on p3.\n\n");

    load_modules();

    if (!find_stick(&parts)) {
        say("ERROR: no falcon stick found (need an exFAT p2 and a LUKS p3 on\n"
            "       one disk). Nothing was unlocked.\n");
        finish(0, 0, 0);
    }
    say("  p2 (ISO library): %s\n  p3 (UKI shelf):   %s\n\n",
        parts.p2, parts.p3);

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
                parts.p3, (char *)MAPPER, NULL
            };

            say("unlocking p3 with the passphrase from the loader...\n");
            if (run(argv, pass, (size_t)clen) == 0)
                mapped = 1;
            else
                say("the delivered credential did not open p3; "
                    "falling back to the prompt.\n");
        }
        explicit_bzero(pass, sizeof(pass));
    }
    for (tries = 1; !mapped && tries <= UNLOCK_TRIES; tries++) {
        char *argv[] = {
            (char *)"/usr/bin/cryptsetup", (char *)"open",
            (char *)"--type", (char *)"luks2",
            (char *)"--readonly",
            (char *)"--key-file", (char *)"-",
            parts.p3, (char *)MAPPER, NULL
        };
        ssize_t len;
        int rc;

        say("passphrase for p3 (attempt %d of %d): ", tries, UNLOCK_TRIES);
        len = read_secret(pass, sizeof(pass));
        if (len <= 0) {
            say("no passphrase given; nothing was unlocked.\n");
            explicit_bzero(pass, sizeof(pass));
            finish(0, 0, 0);
        }
        rc = run(argv, pass, (size_t)len);
        explicit_bzero(pass, sizeof(pass));
        if (rc == 0) {
            mapped = 1;
            break;
        }
        say("wrong passphrase (cryptsetup exit %d).\n", rc);
    }
    if (!mapped) {
        say("\nout of attempts. p3 stays locked and no key was read.\n");
        finish(0, 0, 0);
    }
    say("p3 unlocked (read-only).\n");

    if (mount(MAPPER_NODE, P3_MNT, "ext4", MS_RDONLY, NULL) != 0) {
        say("ERROR: mounting p3: %s\n", strerror(errno));
        finish(0, 0, mapped);
    }
    p3_mounted = 1;

    if (access(KEY_PATH, R_OK) != 0 || access(CERT_PATH, R_OK) != 0) {
        say("ERROR: no MOK keypair on p3 (%s / %s).\n"
            "       A stick built with --p3-plain has no on-stick key and so\n"
            "       no signing appliance; sign from a host instead.\n",
            KEY_PATH, CERT_PATH);
        finish(0, p3_mounted, mapped);
    }
    say("MOK keypair found on p3.\n");

    if (mount(parts.p2, P2_MNT, "exfat", 0, NULL) != 0) {
        say("ERROR: mounting p2: %s\n", strerror(errno));
        finish(0, p3_mounted, mapped);
    }
    p2_mounted = 1;

    if (exfat_uuid(parts.p2, dev, sizeof(dev)) != 0) {
        say("ERROR: could not read p2's volume serial; the signed cmdline\n"
            "       would have no way to name the partition to boot from.\n");
        finish(p2_mounted, p3_mounted, mapped);
    }
    (void)snprintf(dev_arg, sizeof(dev_arg), "UUID=%s", dev);
    say("p2 volume: %s\n\n", dev_arg);

    /* ---- what is pending ---- */

    load_manifest();
    walk(P2_MNT, "", 0);

    if (g_cand_n == 0) {
        say("\nnothing to sign: every ISO on p2 already has a fresh manifest\n"
            "record. FALCON-APPLIANCE-PENDING 0\n");
        finish(p2_mounted, p3_mounted, mapped);
    }

    /* Path order once, here, so the numbers the user types, the order the
     * review prints and the order the batch signs in are all the same one. */
    qsort(g_cand, g_cand_n, sizeof(g_cand[0]), cand_cmp);

    say("\nFALCON-APPLIANCE-PENDING %zu\n", g_cand_n);

    /* ---- mark, review, confirm, then the bless ----
     *
     * DESIGN Section 7.5: the marks are the per-image vouch and the typed
     * "yes" over the reviewed set releases the batch. Nothing below runs
     * unless both happened. */
    if (!choose_candidates()) {
        /* Nothing was vouched for, so every pending image is a skip. */
        skipped_n = g_cand_n;
    } else {
        size_t sel_n = selected_count();
        size_t done_n = 0;

        say("\nFALCON-APPLIANCE-SELECTED %zu\n", sel_n);

        for (i = 0; i < g_cand_n; i++) {
            char abs[PATH_MAX];
            char rel_arg[MANIFEST_PATH_MAX + 2u];
            int rc;

            if (!g_cand[i].selected) {
                skipped_n++;
                continue;
            }
            done_n++;

            /* The precision restates what add_candidate already guaranteed
             * (the path fits the field, terminator included). It is spelled
             * out here because the bound is what keeps a p2 path from running
             * off the end of either buffer, and a bound stated in the call is
             * one the compiler checks rather than one a reader has to trust. */
            (void)snprintf(abs, sizeof(abs), "%s/%.*s", P2_MNT,
                           (int)(sizeof(g_cand[i].rel) - 1u), g_cand[i].rel);
            (void)snprintf(rel_arg, sizeof(rel_arg), "/%.*s",
                           (int)(sizeof(g_cand[i].rel) - 1u), g_cand[i].rel);

            {
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

                say("\n[%zu/%zu] /%s\n", done_n, sel_n, g_cand[i].rel);
                say("      signing...\n");
                rc = run(argv, NULL, 0);
            }
            /* One image failing is that image's problem: the rest of the batch
             * was vouched for too and still deserves to be signed. */
            if (rc == 0) {
                say("      FALCON-APPLIANCE-SIGNED %s\n", g_cand[i].rel);
                signed_n++;
            } else {
                say("      FALCON-APPLIANCE-FAILED %s (exit %d)\n",
                    g_cand[i].rel, rc);
                failed_n++;
            }
        }
    }

    say("\nsummary: FALCON-APPLIANCE-SUMMARY signed=%zu failed=%zu skipped=%zu\n",
        signed_n, failed_n, skipped_n);

    finish(p2_mounted, p3_mounted, mapped);
    return 0;   /* not reached; finish() reboots */
}
