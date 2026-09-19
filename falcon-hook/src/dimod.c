/* falcon-hook: a block driver the initramfs lacks, brought in from the ISO
 * (docs/notes/coverage-roadmap.md M4).
 *
 * The Debian installer's initramfs has neither dm-mod nor loop, built in or
 * as a module; the ISO carries both, in the kernel's own module packages
 * under /pool (loop-modules-<release>-di_*.udeb). Those packages are exactly
 * what the installer unpacks all day with its own udpkg, so the hook needs no
 * decompressor of its own: it reads the one package for the running kernel
 * out of the ISO through the extent map, hands it to udpkg, and asks the
 * initramfs's modprobe for loop. Then the loop fallback in hook.c has a
 * driver to use.
 *
 * Only for an initramfs that has udpkg; anything else keeps what it has.
 */

#include "sys.h"
#include "dimod.h"
#include "hookcore.h"

#include <fs/iso9660.h>
#include <lib/string.h>

/* The biggest package read: a loop-modules udeb is about 20 KB. */
#define PKG_MAX (4u * 1024u * 1024u)

typedef struct {
    int fd;
    const extmap_hdr *h;
    const uint8_t *map;
    uint64_t base;
} iso_ctx;

/* ISO bytes [off, off+len) from the partition, through the extents. */
static int iso_read(void *vctx, uint64_t off, uint64_t len, void *buf)
{
    const iso_ctx *c = vctx;
    uint8_t *out = buf;
    uint64_t start = 0;
    uint32_t i;

    for (i = 0; i < c->h->n_extents && len > 0; i++) {
        extmap_extent e;

        extmap_extent_at(c->map, i, &e);
        if (off < start + e.length) {
            uint64_t within = off - start;
            uint64_t n = e.length - within;

            if (n > len)
                n = len;
            if (sys_pread(c->fd, out, (size_t)n, c->base + e.offset + within) !=
                (long)n)
                return -1;
            out += n;
            off += n;
            len -= n;
        }
        start += e.length;
    }
    return len == 0 ? 0 : -1;
}

static int name_ascii(const uint16_t *name, char *out, size_t cap)
{
    size_t i;

    for (i = 0; name[i] != 0; i++) {
        if (name[i] >= 0x80u || i + 1u >= cap)
            return 0;
        out[i] = (char)name[i];
    }
    out[i] = '\0';
    return 1;
}

/* ASCII case folded, for an ISO9660 name space without Rock Ridge. */
static int name_is(const uint16_t *name, const char *ascii)
{
    size_t i;

    for (i = 0; ascii[i] != '\0'; i++) {
        uint16_t a = name[i], b = (uint16_t)(unsigned char)ascii[i];

        if (a >= 'A' && a <= 'Z')
            a = (uint16_t)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (uint16_t)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return name[i] == 0;
}

static iso9660_vol g_vol;
static iso9660_dir g_d1, g_d2;
static iso9660_file g_file;
static uint8_t g_pkg[PKG_MAX];

/* Finds the package in /pool/main/<letter>/<source>/ and reads it. The
 * source directory differs by distro (linux-signed-amd64 on Debian, linux on
 * Kali), so every directory under /pool/main/l is looked in. */
static long find_package(const char *release, const char *prefix)
{
    static const char *const path[] = { "pool", "main", "l" };
    iso9660_dirent e, sub;
    size_t depth;
    int rc;

    if (iso9660_dir_open_root(&g_vol, &g_d1) != 0)
        return -1;
    for (depth = 0; depth < sizeof path / sizeof path[0]; depth++) {
        int found = 0;

        while ((rc = iso9660_dir_next(&g_d1, &e)) == 1) {
            if (e.is_dir && name_is(e.name, path[depth])) {
                found = 1;
                break;
            }
        }
        if (!found || iso9660_dir_open(&g_vol, &g_d1, &e) != 0)
            return -1;
    }
    while ((rc = iso9660_dir_next(&g_d1, &sub)) == 1) {
        if (!sub.is_dir || iso9660_dir_open(&g_vol, &g_d2, &sub) != 0)
            continue;
        while ((rc = iso9660_dir_next(&g_d2, &e)) == 1) {
            char n[256];

            if (e.is_dir || !name_ascii(e.name, n, sizeof n) ||
                !hook_module_package(n, release, prefix))
                continue;
            if (e.size == 0 || e.size > PKG_MAX ||
                iso9660_file_open(&g_vol, &g_file, &e) != 0 ||
                iso9660_file_read(&g_file, 0, e.size, g_pkg) != 0)
                return -1;
            return (long)e.size;
        }
    }
    return -1;
}

static int run(const char *path, char *const argv[], char **envp)
{
    long pid;
    int status = 0;

    pid = sys_fork();
    if (pid == 0) {
        (void)sys_execve(path, argv, envp);
        sys_exit(127);
    }
    if (pid < 0 || sys_wait(pid, &status) < 0)
        return -1;
    return status;
}

int dimod_bring(const char *prefix, const char *module,
                const extmap_hdr *h, const uint8_t *map, uint32_t maj,
                uint32_t min, uint64_t base, char **envp)
{
    static const char part[] = "/dev/.falcon-part";
    static const char pkg[] = "/tmp/falcon-module.udeb";
    static char uts[6u * HOOK_UTS_FIELD];
    const char *udpkg = (const char *)0;
    iso_ctx c;
    long fd, n;
    char *uargv[4], *margv[4];

    if (sys_exists("/bin/udpkg") == 0)
        udpkg = "/bin/udpkg";
    else if (sys_exists("/usr/bin/udpkg") == 0)
        udpkg = "/usr/bin/udpkg";
    if (udpkg == (const char *)0 || sys_uname(uts) < 0)
        return 0;

    (void)sys_mknod(part, HOOK_S_IFBLK | 0600, hook_devnum(maj, min));
    c.fd = (int)sys_open(part, HOOK_O_RDONLY | HOOK_O_CLOEXEC, 0);
    (void)sys_unlink(part);
    if (c.fd < 0)
        return 0;
    c.h = h;
    c.map = map;
    c.base = base;
    n = -1;
    if (iso9660_mount(&g_vol, iso_read, &c) == 0)
        n = find_package(uts + 2u * HOOK_UTS_FIELD, prefix);
    (void)sys_close(c.fd);
    if (n <= 0)
        return 0;

    (void)sys_mkdir("/tmp", 01777);
    fd = sys_open(pkg, HOOK_O_WRONLY | HOOK_O_CREAT | HOOK_O_TRUNC |
                  HOOK_O_CLOEXEC, 0644);
    if (fd < 0)
        return 0;
    if (sys_write((int)fd, g_pkg, (size_t)n) != n) {
        (void)sys_close((int)fd);
        (void)sys_unlink(pkg);
        return 0;
    }
    (void)sys_close((int)fd);

    uargv[0] = (char *)(uintptr_t)udpkg;
    uargv[1] = (char *)(uintptr_t)"--unpack";
    uargv[2] = (char *)(uintptr_t)pkg;
    uargv[3] = (char *)0;
    n = run(udpkg, uargv, envp);
    (void)sys_unlink(pkg);
    if (n != 0)
        return 0;

    /* The unpacked module is not in modules.dep until depmod has seen it. */
    if (sys_exists("/sbin/depmod") == 0) {
        char *dargv[3];

        dargv[0] = (char *)(uintptr_t)"depmod";
        dargv[1] = (char *)(uintptr_t)"-a";
        dargv[2] = (char *)0;
        (void)run("/sbin/depmod", dargv, envp);
    }
    margv[0] = (char *)(uintptr_t)"modprobe";
    margv[1] = (char *)(uintptr_t)"-q";
    margv[2] = (char *)(uintptr_t)module;
    margv[3] = (char *)0;
    return run("/sbin/modprobe", margv, envp) == 0;
}
