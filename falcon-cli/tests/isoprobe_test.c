/* Tests for the bless tool's ISO view (falcon-cli/src/sign/isoprobe.c).
 *
 * The view indexes an ISO with falcon-core's ISO9660 and UDF drivers and
 * answers the questions the bless flow asks: does this path
 * exist (for falcon-core/src/media/family.c), which member is the kernel,
 * how long is a member, and what are its bytes. The drivers themselves are
 * tested in falcon-core; what is pinned here is the view's own behaviour.
 *
 *   lookup     the matching family.h specifies: absolute paths, case folded
 *              on both sides (Rock Ridge keeps the author's case, oscdimg's
 *              UDF names are lower case), a directory matched on a component
 *              boundary, a trailing '*' as a suffix wildcard. Tested on views
 *              built from path lists, which need no image.
 *
 *   tables     the kernel and initramfs tables resolve against what an ISO
 *              actually holds, and say why when they cannot.
 *
 *   images     the fixtures falcon-core/tools and falcon-cli/tools build: an
 *              Arch-shaped ISO9660 image, the Rock Ridge fixture with its
 *              symlink, and the UDF fixture. Opening, family probing with
 *              content fingerprints, sizes, extraction byte for byte, and the
 *              refusals (not an ISO, a symlink, a corrupt directory).
 *
 *   label      the PVD's volume label, which goes into a signed cmdline and
 *              is refused rather than escaped when a cmdline cannot carry it.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sign/sign.h>
#include <sign/isoprobe.h>
#include <media/family.h>
#include <crypto/sha256.h>

#ifndef ISOVIEW_IMG
#define ISOVIEW_IMG "build/test-isoview.iso"
#endif
#ifndef MEDIA_FIXTURE_DIR
#define MEDIA_FIXTURE_DIR "build"
#endif

/* ------------------------------------------------------------ lookup */

/* A Windows install medium's layout, as oscdimg writes it: lower case. */
static const char *const g_windows[] = {
    "autorun.inf", "boot/", "boot/bcd", "efi/", "efi/boot/",
    "efi/boot/bootx64.efi", "efi/microsoft/boot/bcd", "setup.exe",
    "sources/", "sources/boot.wim", "sources/install.wim", NULL
};

static const char *const g_casper[] = {
    "/casper/", "/casper/vmlinuz", "/casper/initrd", "/casper/filesystem.squashfs",
    "/.disk/info", "/boot/grub/grub.cfg", "/EFI/BOOT/BOOTX64.EFI", NULL
};

static void test_isoprobe_windows_layout(void **state)
{
    iso_view *v = NULL;
    family_probe probe;
    const char *match = NULL;
    uint64_t size = 99;

    (void)state;
    assert_int_equal(iso_view_from_paths(g_windows, NULL, &v), SGN_OK);
    assert_int_equal(iso_view_entries(v), 11);
    assert_string_equal(iso_view_reader(v), "none");
    iso_view_family_probe(v, &probe);
    assert_int_equal(family_detect(&probe)->id, FAMILY_WINDOWS);
    /* Upper case asked, lower case recorded. */
    assert_true(iso_view_find(v, "/SOURCES/INSTALL.WIM", &match));
    assert_string_equal(match, "sources/install.wim");
    assert_true(iso_view_size(v, match, &size));
    assert_int_equal(size, 0);
    /* A directory has no size, and a foreign pointer names nothing. */
    assert_true(iso_view_find(v, "/sources", &match));
    assert_false(iso_view_size(v, match, &size));
    assert_false(iso_view_size(v, "sources/boot.wim", &size));
    iso_view_close(v);
}

static void test_isoprobe_casper_layout(void **state)
{
    static const uint64_t sizes[] = { 0, 1234, 55, 7, 1, 2, 3 };
    iso_view *v = NULL;
    family_probe probe;
    const char *match = NULL;
    uint64_t size = 0;
    uint8_t buf[8];
    size_t got = 9;

    (void)state;
    assert_int_equal(iso_view_from_paths(g_casper, sizes, &v), SGN_OK);
    iso_view_family_probe(v, &probe);
    assert_int_equal(family_detect(&probe)->id, FAMILY_DEBIAN_CASPER);
    assert_string_equal(iso_kernel_path(v, FAMILY_DEBIAN_CASPER),
                        "casper/vmlinuz");
    assert_string_equal(iso_initrd_path(v, FAMILY_DEBIAN_CASPER,
                                        "casper/vmlinuz"), "casper/initrd");
    assert_true(iso_view_find(v, "/casper/vmlinuz", &match));
    assert_true(iso_view_size(v, match, &size));
    assert_int_equal(size, 1234);
    /* No image behind the view: content is unreadable, never invented. */
    assert_int_equal(probe.read(probe.ctx, "/.disk/info", 0, buf, sizeof buf,
                                &got), 1);
    assert_int_equal(got, 0);
    assert_int_equal(iso_view_extract(v, match, "/dev/null"), SGN_ERR_FORMAT);
    iso_view_close(v);
}

static void test_isoprobe_matching_rules(void **state)
{
    static const char *const paths[] = {
        "./live/vmlinuz-6.12.107+deb13-amd64", "livecd-thing/x", "Arch/Boot/x",
        "EFI/BOOT/", "", NULL
    };
    iso_view *v = NULL;
    const char *match = NULL;

    (void)state;
    assert_int_equal(iso_view_from_paths(paths, NULL, &v), SGN_OK);
    /* Case folds in both directions. */
    assert_true(iso_view_find(v, "/arch/boot/x", NULL));
    assert_true(iso_view_find(v, "/ARCH/BOOT/X", NULL));
    assert_true(iso_view_find(v, "/efi/boot", &match));
    assert_string_equal(match, "EFI/BOOT");
    /* A directory matches on a component boundary: "/live" is not
     * "/livecd-thing", though "/live" is a parent of the kernel. */
    assert_true(iso_view_find(v, "/live", &match));
    assert_string_equal(match, "live/vmlinuz-6.12.107+deb13-amd64");
    assert_false(iso_view_find(v, "/livec", NULL));
    /* The wildcard is a suffix match and hands back the concrete name. */
    assert_true(iso_view_find(v, "/live/vmlinuz*", &match));
    assert_string_equal(match, "live/vmlinuz-6.12.107+deb13-amd64");
    assert_string_equal(iso_kernel_path(v, FAMILY_DEBIAN_LIVE),
                        "live/vmlinuz-6.12.107+deb13-amd64");
    assert_false(iso_view_find(v, "/zzz*", NULL));
    /* Nothing asked, nothing found. */
    assert_false(iso_view_find(v, "/", NULL));
    assert_false(iso_view_find(v, "*", NULL));
    assert_false(iso_view_find(v, NULL, &match));
    assert_null(match);
    assert_false(iso_view_find(NULL, "/x", NULL));
    iso_view_close(v);
    iso_view_close(NULL);

    assert_int_equal(iso_view_from_paths(NULL, NULL, &v), SGN_ERR_INTERNAL);
    assert_int_equal(iso_view_entries(NULL), 0);
    assert_string_equal(iso_view_reader(NULL), "none");
}

/* ------------------------------------------------------------ tables */

static void test_isoprobe_kernel_table_refusals(void **state)
{
    static const char *const paths[] = { "arch/boot/x86_64/other", NULL };
    iso_view *v = NULL;

    (void)state;
    assert_int_equal(iso_view_from_paths(paths, NULL, &v), SGN_OK);
    /* A family with a row whose candidates are all absent. */
    assert_null(iso_kernel_path(v, FAMILY_ARCH));
    assert_non_null(strstr(sgn_error(), "no kernel found"));
    /* A family with no row: it boots no Linux kernel. */
    assert_null(iso_kernel_path(v, FAMILY_WINDOWS));
    assert_non_null(strstr(sgn_error(), "no kernel to wrap"));
    assert_null(iso_kernel_path(NULL, FAMILY_ARCH));
    /* No initramfs beside the kernel. */
    assert_null(iso_initrd_path(v, FAMILY_ARCH, "arch/boot/x86_64/other"));
    assert_non_null(strstr(sgn_error(), "no initramfs"));
    assert_null(iso_initrd_path(v, FAMILY_ARCH, NULL));
    iso_view_close(v);
}

static void test_isoprobe_gentoo_names(void **state)
{
    static const char *const paths[] = {
        "boot/gentoo", "boot/gentoo.igz", "image.squashfs", NULL
    };
    iso_view *v = NULL;
    const char *k;

    (void)state;
    assert_int_equal(iso_view_from_paths(paths, NULL, &v), SGN_OK);
    k = iso_kernel_path(v, FAMILY_GENTOO);
    assert_string_equal(k, "boot/gentoo");
    assert_string_equal(iso_initrd_path(v, FAMILY_GENTOO, k),
                        "boot/gentoo.igz");
    iso_view_close(v);
}

/* ------------------------------------------------------------ images */

static int fixture_path(char *out, size_t cap, const char *name)
{
    return snprintf(out, cap, "%s/%s", MEDIA_FIXTURE_DIR, name) < (int)cap;
}

static void sha_file(const char *path, char *hex)
{
    static uint8_t buf[1u << 16];
    static const char digits[] = "0123456789abcdef";
    sha256_ctx c;
    uint8_t h[32];
    FILE *f = fopen(path, "rb");
    size_t n;
    int i;

    assert_non_null(f);
    sha256_init(&c);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        sha256_update(&c, buf, n);
    fclose(f);
    sha256_final(&c, h);
    for (i = 0; i < 32; i++) {
        hex[2 * i] = digits[h[i] >> 4];
        hex[2 * i + 1] = digits[h[i] & 15u];
    }
    hex[64] = '\0';
}

static void test_isoview_arch_image(void **state)
{
    iso_view *v = NULL;
    family_probe probe;
    const char *k = NULL;
    uint64_t size = 0;
    char buf[64];
    size_t got = 0;
    char dst[] = "/tmp/falcon-isoview.XXXXXX";
    int fd;
    FILE *f;

    (void)state;
    assert_int_equal(iso_view_open(ISOVIEW_IMG, &v), SGN_OK);
    assert_string_equal(iso_view_reader(v), "iso9660");
    iso_view_family_probe(v, &probe);
    assert_int_equal(family_detect(&probe)->id, FAMILY_ARCH);

    k = iso_kernel_path(v, FAMILY_ARCH);
    assert_string_equal(k, "arch/boot/x86_64/vmlinuz-linux");
    assert_true(iso_view_size(v, k, &size));
    assert_int_equal(size, 20 + 96 * 1024);

    /* The family callback reads content: offsets, short reads, past-end. */
    assert_int_equal(probe.read(probe.ctx, "/readme.txt", 0, buf, 11, &got), 0);
    assert_int_equal(got, 11);
    assert_memory_equal(buf, "falcon-boot", 11);
    assert_int_equal(probe.read(probe.ctx, "/readme.txt", 7, buf, sizeof buf,
                                &got), 0);
    assert_int_equal(got, 28 - 7);
    assert_int_equal(probe.read(probe.ctx, "/readme.txt", 500, buf, 4, &got),
                     0);
    assert_int_equal(got, 0);
    assert_int_equal(probe.read(probe.ctx, "/nope", 0, buf, 4, &got), 1);
    assert_int_equal(probe.read(probe.ctx, "/arch", 0, buf, 4, &got), 1);

    /* Extraction, byte for byte. */
    fd = mkstemp(dst);
    assert_true(fd >= 0);
    close(fd);
    assert_true(iso_view_find(v, "/MixedCase/Thing.TXT", &k));
    assert_int_equal(iso_view_extract(v, k, dst), SGN_OK);
    f = fopen(dst, "rb");
    assert_non_null(f);
    got = fread(buf, 1, sizeof buf, f);
    fclose(f);
    assert_int_equal(got, 17);
    assert_memory_equal(buf, "mixed case entry\n", 17);
    /* A destination that cannot be created. */
    assert_int_equal(iso_view_extract(v, k, "/nonexistent-dir/x"), SGN_ERR_IO);
    assert_int_equal(iso_view_extract(v, NULL, dst), SGN_ERR_INTERNAL);
    unlink(dst);
    iso_view_close(v);
}

static int count_name(void *arg, const char *name)
{
    size_t *n = arg;

    (void)name;
    (*n)++;
    return 0;
}

static int stop_at_first(void *arg, const char *name)
{
    size_t *n = arg;

    (void)name;
    (*n)++;
    return 1;
}

/* The boot configuration callbacks over the index: exact existence, one
 * directory level listed, whole files read or refused for size. */
static void test_isoview_bootcfg_source(void **state)
{
    iso_view *v = NULL;
    bootcfg_src src;
    char buf[64];
    size_t len = 0, n = 0;

    (void)state;
    assert_int_equal(iso_view_open(ISOVIEW_IMG, &v), SGN_OK);
    iso_view_bootcfg_src(v, &src);
    iso_view_bootcfg_src(v, NULL);

    assert_true(src.exists(src.ctx, "/arch/boot/grub/grub.cfg"));
    assert_true(src.exists(src.ctx, "/ARCH/BOOT/GRUB/GRUB.CFG"));
    assert_false(src.exists(src.ctx, "/arch/boot"));        /* a directory */
    assert_false(src.exists(src.ctx, "/arch/boot/grub/grub"));
    assert_false(src.exists(src.ctx, "/nope"));

    assert_int_equal(src.read(src.ctx, "/arch/boot/grub/grub.cfg", buf,
                              sizeof buf, &len), 0);
    assert_int_equal(len, 14);
    assert_memory_equal(buf, "set timeout=5\n", 14);
    assert_int_equal(src.read(src.ctx, "/arch/boot/grub/grub.cfg", buf, 4,
                              &len), 1);
    assert_int_equal(len, 14);                              /* too big */
    assert_int_equal(src.read(src.ctx, "/nope", buf, sizeof buf, &len), 1);
    assert_int_equal(src.read(src.ctx, "/arch", buf, sizeof buf, &len), 1);

    /* One level: x86_64 holds two files, boot holds only directories. */
    assert_int_equal(src.list(src.ctx, "/arch/boot/x86_64", count_name, &n), 0);
    assert_int_equal(n, 2);
    n = 0;
    assert_int_equal(src.list(src.ctx, "/arch/boot", count_name, &n), 0);
    assert_int_equal(n, 0);
    n = 0;
    assert_int_equal(src.list(src.ctx, "/arch/boot/x86_64", stop_at_first,
                              &n), 0);
    assert_int_equal(n, 1);
    assert_int_equal(src.list(src.ctx, "/missing", count_name, &n), 1);
    assert_int_equal(src.list(src.ctx, "/", count_name, &n), 1);

    /* With nothing that boots, the parser says so. */
    {
        static char scratch[64 * 1024];
        static bootcfg_result res;

        assert_int_equal(bootcfg_read(&src, BOOTCFG_CD, scratch,
                                      sizeof scratch, &res),
                         BOOTCFG_ERR_NONE);
    }
    iso_view_close(v);

    /* A view with no image behind it reads nothing. */
    {
        static const char *const paths[] = { "boot/grub/grub.cfg", NULL };

        assert_int_equal(iso_view_from_paths(paths, NULL, &v), SGN_OK);
        iso_view_bootcfg_src(v, &src);
        assert_int_equal(src.read(src.ctx, "/boot/grub/grub.cfg", buf,
                                  sizeof buf, &len), 1);
        iso_view_close(v);
    }
}

static void test_isoview_rock_ridge_and_udf(void **state)
{
    char iso[512], img[512], big[512], ref[65], ours[65];
    char dst[] = "/tmp/falcon-isoview.XXXXXX";
    iso_view *v = NULL;
    const char *m = NULL;
    uint64_t size = 0;
    int fd;

    (void)state;
    assert_true(fixture_path(iso, sizeof iso, "test-iso-rr.iso"));
    assert_int_equal(iso_view_open(iso, &v), SGN_OK);
    assert_string_equal(iso_view_reader(v), "iso9660");
    assert_true(iso_view_entries(v) > 400);
    /* A Rock Ridge symlink is found, and refused as a member to read. */
    assert_true(iso_view_find(v, "/link-to-s1", &m));
    assert_int_equal(iso_view_extract(v, m, "/dev/null"), SGN_ERR_FORMAT);
    assert_non_null(strstr(sgn_error(), "symlink"));
    iso_view_close(v);

    /* The UDF fixture: a fragmented big.bin extracted whole. */
    assert_true(fixture_path(img, sizeof img, "test-udf.img"));
    assert_true(fixture_path(big, sizeof big, "test-udf.bigfile"));
    assert_int_equal(iso_view_open(img, &v), SGN_OK);
    assert_string_equal(iso_view_reader(v), "udf");
    assert_true(iso_view_find(v, "/BIG.BIN", &m));
    assert_true(iso_view_size(v, m, &size));
    assert_int_equal(size, 24u * 131072u);
    fd = mkstemp(dst);
    assert_true(fd >= 0);
    close(fd);
    assert_int_equal(iso_view_extract(v, m, dst), SGN_OK);
    sha_file(dst, ours);
    sha_file(big, ref);
    assert_string_equal(ours, ref);
    unlink(dst);
    iso_view_close(v);
}

static void test_isoview_refusals(void **state)
{
    char p[512];
    iso_view *v = (iso_view *)&v;

    (void)state;
    assert_int_equal(iso_view_open("/nonexistent.iso", &v), SGN_ERR_IO);
    assert_null(v);
    assert_int_equal(iso_view_open(NULL, &v), SGN_ERR_INTERNAL);
    assert_int_equal(iso_view_open("/x", NULL), SGN_ERR_INTERNAL);
    assert_int_equal(iso_view_open_cb(NULL, NULL, "x", &v), SGN_ERR_INTERNAL);

    /* An ext4 image: a filesystem, but not media. */
    assert_true(fixture_path(p, sizeof p, "test-ext4.img"));
    if (access(p, R_OK) == 0) {
        assert_int_equal(iso_view_open(p, &v), SGN_ERR_FORMAT);
        assert_non_null(strstr(sgn_error(), "not an ISO9660 or UDF"));
    }
    /* UDF 2.50: recognised, refused, and the message says why. */
    assert_true(fixture_path(p, sizeof p, "test-udf-250.img"));
    assert_int_equal(iso_view_open(p, &v), SGN_ERR_FORMAT);
    assert_non_null(strstr(sgn_error(), "feature"));
}

/* A corrupt directory, through the callback seam: the Arch-shaped image in
 * memory with one byte of a subdirectory's first record broken. */
static uint8_t *g_img;
static size_t g_img_len;

static int mem_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    if (off + len > g_img_len)
        return -1;
    memcpy(buf, g_img + off, (size_t)len);
    return 0;
}

static void test_isoview_corrupt_tree(void **state)
{
    FILE *f;
    iso_view *v = NULL;
    struct stat st;
    uint32_t root, lba = 0;
    uint8_t *r;

    (void)state;
    assert_int_equal(stat(ISOVIEW_IMG, &st), 0);
    g_img_len = (size_t)st.st_size;
    g_img = malloc(g_img_len);
    assert_non_null(g_img);
    f = fopen(ISOVIEW_IMG, "rb");
    assert_non_null(f);
    assert_int_equal(fread(g_img, 1, g_img_len, f), g_img_len);
    fclose(f);

    assert_int_equal(iso_view_open_cb(mem_read, NULL, "mem", &v), SGN_OK);
    iso_view_close(v);

    /* Find the root's first real entry and make its record overrun the
     * block: the index must refuse the tree, not skip the entry. */
    root = (uint32_t)g_img[16 * 2048 + 156 + 2] |
           ((uint32_t)g_img[16 * 2048 + 156 + 3] << 8) |
           ((uint32_t)g_img[16 * 2048 + 156 + 4] << 16);
    r = g_img + (size_t)root * 2048;
    r += r[0];                  /* past "." */
    r += r[0];                  /* past ".." */
    lba = r[0];
    r[0] = 255;
    r[32] = 250;
    assert_int_equal(iso_view_open_cb(mem_read, NULL, "mem", &v),
                     SGN_ERR_FORMAT);
    assert_non_null(strstr(sgn_error(), "corrupt"));
    (void)lba;

    /* Nothing at all readable. */
    g_img_len = 100;
    assert_int_equal(iso_view_open_cb(mem_read, NULL, "mem", &v),
                     SGN_ERR_FORMAT);
    free(g_img);
    g_img = NULL;
}

/* ------------------------------------------------------------ label */

/* A minimal ISO9660 image in a file: PVD at 16 with the given label and
 * descriptor type, a terminator at 17, and a root directory at 18. */
static void write_pvd(const char *path, const char *label, int type)
{
    static uint8_t img[20 * 2048];
    uint8_t *d = img + 16 * 2048;
    uint8_t *root = img + 18 * 2048;
    FILE *f;

    memset(img, 0, sizeof img);
    d[0] = (uint8_t)type;
    memcpy(d + 1, "CD001", 5);
    d[6] = 1;
    memset(d + 40, ' ', 32);
    memcpy(d + 40, label, strlen(label));
    d[128] = 0x00;
    d[129] = 0x08;              /* logical block size 2048, little half */
    d[156] = 34;
    d[156 + 2] = 18;
    d[156 + 11] = 0x08;         /* root length 2048 */
    d[156 + 25] = 2;
    d[156 + 32] = 1;
    img[17 * 2048] = 255;
    memcpy(img + 17 * 2048 + 1, "CD001", 5);
    root[0] = 34;
    root[2] = 18;
    root[11] = 0x08;
    root[25] = 2;
    root[32] = 1;
    f = fopen(path, "wb");
    assert_non_null(f);
    assert_int_equal(fwrite(img, 1, sizeof img, f), sizeof img);
    fclose(f);
}

static void test_isoprobe_volume_label(void **state)
{
    char path[] = "/tmp/falcon-pvd.XXXXXX";
    char out[64];
    char tiny[4];
    int fd = mkstemp(path);

    (void)state;
    assert_true(fd >= 0);
    close(fd);

    write_pvd(path, "Gentoo-amd64-20260510", 1);
    assert_int_equal(iso_volume_label(path, out, sizeof(out)), SGN_OK);
    assert_string_equal(out, "Gentoo-amd64-20260510");
    assert_int_equal(iso_volume_label(path, tiny, sizeof(tiny)),
                     SGN_ERR_NOSPACE);

    /* A space would be a second kernel argument in the signed cmdline. */
    write_pvd(path, "Gentoo x rd.break", 1);
    assert_int_equal(iso_volume_label(path, out, sizeof(out)), SGN_ERR_FORMAT);
    write_pvd(path, "", 1);
    assert_int_equal(iso_volume_label(path, out, sizeof(out)), SGN_ERR_FORMAT);
    write_pvd(path, "LABEL", 2);            /* not the primary descriptor */
    assert_int_equal(iso_volume_label(path, out, sizeof(out)), SGN_ERR_FORMAT);

    assert_int_equal(truncate(path, 100), 0);
    assert_int_equal(iso_volume_label(path, out, sizeof(out)), SGN_ERR_FORMAT);
    unlink(path);
    assert_int_equal(iso_volume_label(path, out, sizeof(out)), SGN_ERR_IO);
    assert_int_equal(iso_volume_label(NULL, out, sizeof(out)),
                     SGN_ERR_INTERNAL);
}

/* ------------------------------------------------------------ PATH lookup */

static void test_which_reports_the_full_path(void **state)
{
    char dir[] = "/tmp/falcon-which.XXXXXX";
    char prog[PATH_MAX], out[PATH_MAX], tiny[4];
    char *saved;
    const char *p = getenv("PATH");
    FILE *f;

    (void)state;
    assert_non_null(mkdtemp(dir));
    saved = strdup(p != NULL ? p : "/usr/bin:/bin");
    (void)snprintf(prog, sizeof prog, "%s/fake-tool", dir);
    f = fopen(prog, "w");
    assert_non_null(f);
    (void)fprintf(f, "#!/bin/sh\n");
    fclose(f);
    assert_int_equal(chmod(prog, 0755), 0);
    assert_int_equal(setenv("PATH", dir, 1), 0);

    assert_int_equal(sgn_which("fake-tool", out, sizeof(out)), 1);
    assert_string_equal(out, prog);
    assert_int_equal(sgn_have_program("fake-tool"), 1);
    /* A name with a slash is taken as the path itself. */
    assert_int_equal(sgn_which(prog, out, sizeof(out)), 1);
    assert_string_equal(out, prog);
    assert_int_equal(sgn_which("no-such-program", out, sizeof(out)), 0);
    assert_int_equal(sgn_which("fake-tool", tiny, sizeof(tiny)), 0);
    assert_int_equal(sgn_which(prog, tiny, sizeof(tiny)), 0);
    assert_int_equal(sgn_which(NULL, out, sizeof(out)), 0);
    assert_int_equal(sgn_which("fake-tool", NULL, 0), 0);

    (void)setenv("PATH", saved, 1);
    free(saved);
    unlink(prog);
    rmdir(dir);
}

int run_isoprobe_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_isoprobe_windows_layout),
        cmocka_unit_test(test_isoprobe_casper_layout),
        cmocka_unit_test(test_isoprobe_matching_rules),
        cmocka_unit_test(test_isoprobe_kernel_table_refusals),
        cmocka_unit_test(test_isoprobe_gentoo_names),
        cmocka_unit_test(test_isoprobe_volume_label),
        cmocka_unit_test(test_which_reports_the_full_path),
    };
    const struct CMUnitTest image_tests[] = {
        cmocka_unit_test(test_isoview_arch_image),
        cmocka_unit_test(test_isoview_bootcfg_source),
        cmocka_unit_test(test_isoview_rock_ridge_and_udf),
        cmocka_unit_test(test_isoview_refusals),
        cmocka_unit_test(test_isoview_corrupt_tree),
    };
    static const char *const need[] = {
        "test-iso-rr.iso", "test-udf.img", "test-udf.bigfile",
        "test-udf-250.img",
    };
    char p[512];
    int failed;
    size_t i;

    failed = cmocka_run_group_tests_name("ISO view (bless tool)", tests,
                                         NULL, NULL);
    if (access(ISOVIEW_IMG, R_OK) != 0) {
        printf("\n[  SKIPPED ] ISO view fixture: %s not found.\n"
               "             Build it with: make test-isoview (needs xorriso).\n",
               ISOVIEW_IMG);
        return failed + test_skip_rc();
    }
    for (i = 0; i < sizeof need / sizeof need[0]; i++) {
        (void)snprintf(p, sizeof p, "%s/%s", MEDIA_FIXTURE_DIR, need[i]);
        if (access(p, R_OK) != 0) {
            printf("\n[  SKIPPED ] ISO view: %s not found.\n"
                   "             Build it with: make test-iso9660 test-udf\n",
                   p);
            return failed + test_skip_rc();
        }
    }
    return failed + cmocka_run_group_tests_name("ISO view (real images)",
                                                image_tests, NULL, NULL);
}
