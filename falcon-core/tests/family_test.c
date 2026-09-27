/* Tests for falcon-core/src/media/family.c, the ISO fingerprint-to-descriptor
 * lookup of docs/DESIGN.md Section 6.2.
 *
 * The module reaches the filesystem only through two callbacks, so a whole
 * medium is modelled here as a list of paths plus an optional content map:
 * "exists" is set membership and "read" serves the mapped content. That makes
 * a test tree three lines long and, more to the point, makes the ambiguous
 * cases constructible. A real SystemRescue ISO is archiso underneath and
 * carries archiso's loader entries; the only way to prove the precedence rule
 * that keeps it from being classified as Arch is to build a tree that matches
 * both fingerprints at once, which no fixture image can be talked into being.
 *
 * Three things are pinned:
 *
 *   identity     one positive per family, asserting the descriptor id AND the
 *                cmdline template, because the template is what the boot path
 *                will consume and a wrong-but-adjacent family (Debian live vs
 *                casper) differs in nothing else.
 *
 *   precedence   the ordering in family_detect is load-bearing and invisible:
 *                a tree matching two families must resolve to the one the
 *                module means, so each ambiguous pair gets its own test.
 *
 *   registry     family_by_id round-trips every row, and the templates only
 *                ever use the two placeholders the header documents. A typo
 *                in a template is a cmdline that silently fails to find its
 *                ISO at boot; here it is a failed assertion.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <media/family.h>

/* ------------------------------------------------------------ fake tree */

typedef struct {
    const char *path;
    const char *content;   /* NULL for a path that exists but is not read */
} fake_file;

typedef struct {
    const fake_file *files;
    size_t           n;
    unsigned         exists_calls;
    unsigned         read_calls;
    int              lie_about_length; /* report past the buffer offered */
    int              over_report;      /* report the whole buffer as filled */
} fake_tree;

static int fake_exists(void *ctx, const char *path)
{
    fake_tree *t = ctx;
    size_t i;

    t->exists_calls++;
    for (i = 0; i < t->n; i++) {
        if (strcmp(t->files[i].path, path) == 0)
            return 1;
    }
    return 0;
}

static int fake_read(void *ctx, const char *path, uint64_t off, void *buf,
                     size_t len, size_t *got)
{
    fake_tree *t = ctx;
    size_t i;

    t->read_calls++;
    for (i = 0; i < t->n; i++) {
        const char *c = t->files[i].content;
        size_t clen;
        size_t take;

        if (strcmp(t->files[i].path, path) != 0 || c == NULL)
            continue;
        clen = strlen(c);
        if (off >= clen) {
            *got = 0;
            return 0;
        }
        take = clen - (size_t)off;
        if (take > len)
            take = len;
        memcpy(buf, c + off, take);
        /* A callback that claims more than it wrote is a bug the module has
         * to survive rather than read uninitialised bytes over. The two lies
         * are refused by different rules: one is past the buffer it was
         * offered, the other is inside it and looks exactly like a file that
         * happened to fill the window. */
        if (t->lie_about_length)
            *got = len + 1u;
        else if (t->over_report)
            *got = len;
        else
            *got = take;
        return 0;
    }
    return -1; /* not present, or present but not readable */
}

static void tree_probe(family_probe *p, fake_tree *t, const fake_file *files,
                       size_t n)
{
    memset(t, 0, sizeof(*t));
    t->files = files;
    t->n = n;
    p->exists = fake_exists;
    p->read = fake_read;
    p->ctx = t;
}

/* Asserts the tree classifies as id, and that the descriptor carries the
 * template the boot path will substitute into. NULL template means the family
 * needs no fragment at all, which is a different claim from "empty string"
 * and is asserted as such. */
static void expect_family(const fake_file *files, size_t n, family_id id,
                          const char *tmpl)
{
    const family_desc *d;
    family_probe p;
    fake_tree t;

    tree_probe(&p, &t, files, n);
    d = family_detect(&p);
    assert_non_null(d);
    assert_int_equal(d->id, id);
    assert_non_null(d->name);
    if (tmpl == NULL)
        assert_null(d->cmdline_template);
    else
        assert_string_equal(d->cmdline_template, tmpl);
    /* Whatever else it is, it must be the registry's own row. */
    assert_ptr_equal(d, family_by_id(id));
}

#define CASPER_TMPL  "iso-scan/filename=%ISO%"
#define LIVE_TMPL    "findiso=%ISO%"
#define DRACUT_TMPL  "iso-scan/filename=%ISO%"
#define ARCHISO_TMPL "img_dev=%DEV% img_loop=%ISO%"
#define SYSRESC_TMPL ARCHISO_TMPL " archisobasedir=sysresccd"
#define GENTOO_TMPL  "dokeymap nodhcp root=live:CDLABEL=%LABEL% rd.live.dir=/ " \
                     "rd.live.squashimg=image.squashfs cdroot " \
                     "iso-scan/filename=%ISO%"

/* --------------------------------------------------------- one per family */

static void test_family_windows(void **state)
{
    static const fake_file wim[] = {
        { "/sources", NULL },
        { "/sources/install.wim", NULL },
        { "/boot", NULL },
    };
    static const fake_file esd[] = {
        { "/sources", NULL },
        { "/sources/install.esd", NULL },
    };

    (void)state;
    /* No template and no injection: the injection column means the
     * extent-map/device-mapper hook, which is a Linux initramfs mechanism,
     * and WinPE re-finds its medium from Windows userland instead. */
    expect_family(wim, 3, FAMILY_WINDOWS, NULL);
    expect_family(esd, 2, FAMILY_WINDOWS, NULL);
    assert_int_equal(family_by_id(FAMILY_WINDOWS)->needs_injection, 0);
}

/* Windows 2000/XP: no /sources, no WinPE - the text-mode setup loader and its
 * script live in /i386. Detected as Windows so the loader takes the
 * driver-injection route (windows-route.md 7.4). Both halves are required, so
 * an unrelated ISO that happens to carry a txtsetup.sif is not mistaken for it. */
static void test_family_windows_xp(void **state)
{
    static const fake_file xp[] = {
        { "/i386", NULL },
        { "/i386/txtsetup.sif", NULL },
        { "/i386/setupldr.bin", NULL },
    };
    static const fake_file half[] = {
        { "/i386", NULL },
        { "/i386/txtsetup.sif", NULL },
    };

    (void)state;
    expect_family(xp, 3, FAMILY_WINDOWS, NULL);
    expect_family(half, 2, FAMILY_UNKNOWN, NULL);
}

/* Which volume of the medium is fingerprinted decides the answer, and on
 * Microsoft's media it is not the bootable one. oscdimg leaves the ISO 9660
 * namespace holding a single stub README.TXT and puts every real file in the
 * UDF namespace; the El Torito boot image is a 1.4 MB FAT volume carrying
 * nothing but the loader. Both trees below are real: the first is what
 * fingerprinting the boot volume alone can see, the second is the UDF volume.
 *
 * The UDF tree is spelled entirely in lower case on purpose. The fake probe
 * matches with strcmp, so it stands in for a case-sensitive provider, and the
 * Windows fingerprint has to be written in the case the media uses to match
 * through one. */
static void test_family_windows_only_on_the_udf_volume(void **state)
{
    static const fake_file eltorito[] = {
        { "/EFI", NULL },
        { "/EFI/BOOT", NULL },
        { "/EFI/BOOT/BOOTX64.EFI", NULL },
    };
    static const fake_file udf[] = {
        { "/boot", NULL },
        { "/efi/boot/bootx64.efi", NULL },
        { "/efi/microsoft/boot/bcd", NULL },
        { "/setup.exe", NULL },
        { "/sources", NULL },
        { "/sources/boot.wim", NULL },
        { "/sources/install.wim", NULL },
    };

    (void)state;
    expect_family(eltorito, 3, FAMILY_UNKNOWN, NULL);
    expect_family(udf, 7, FAMILY_WINDOWS, NULL);
}

/* WinPE with no install image: Macrium Reflect 8's rescue disc, as its UDF
 * volume spells it. A boot.wim alone is not enough; the boot manager beside
 * it is what says Windows. */
static void test_family_winpe(void **state)
{
    static const fake_file macrium[] = {
        { "/bootmgr", NULL },
        { "/EFI/Microsoft/Boot/BCD", NULL },
        { "/sources", NULL },
        { "/sources/boot.wim", NULL },
    };
    static const fake_file wim_only[] = {
        { "/sources", NULL },
        { "/sources/boot.wim", NULL },
    };

    (void)state;
    expect_family(macrium, 4, FAMILY_WINDOWS, NULL);
    expect_family(wim_only, 2, FAMILY_UNKNOWN, NULL);
}

static void test_family_systemrescue(void **state)
{
    static const fake_file dir[] = {
        { "/sysresccd", NULL },
        { "/sysresccd/boot/x86_64/vmlinuz", NULL },
    };
    static const fake_file ver[] = {
        { "/sysresccd/version", NULL },
    };

    (void)state;
    expect_family(dir, 2, FAMILY_SYSTEMRESCUE, SYSRESC_TMPL);
    expect_family(ver, 1, FAMILY_SYSTEMRESCUE, SYSRESC_TMPL);
}

static void test_family_casper(void **state)
{
    static const fake_file t[] = {
        { "/casper", NULL },
        { "/casper/vmlinuz", NULL },
        { "/casper/filesystem.squashfs", NULL },
    };
    static const fake_file only_kernel[] = {
        { "/casper/vmlinuz", NULL },
    };

    (void)state;
    expect_family(t, 3, FAMILY_DEBIAN_CASPER, CASPER_TMPL);
    expect_family(only_kernel, 1, FAMILY_DEBIAN_CASPER, CASPER_TMPL);
}

static void test_family_debian_live(void **state)
{
    static const fake_file t[] = {
        { "/live", NULL },
        { "/live/filesystem.squashfs", NULL },
        { "/live/vmlinuz", NULL },
    };
    static const fake_file only_squash[] = {
        { "/live/filesystem.squashfs", NULL },
    };

    (void)state;
    /* live-boot's findiso, not casper's iso-scan: the two are different
     * implementations and the templates must not be interchangeable. */
    expect_family(t, 3, FAMILY_DEBIAN_LIVE, LIVE_TMPL);
    expect_family(only_squash, 1, FAMILY_DEBIAN_LIVE, LIVE_TMPL);
    assert_string_not_equal(LIVE_TMPL, CASPER_TMPL);
}

static void test_family_fedora_dracut(void **state)
{
    static const fake_file live[] = {
        { "/LiveOS", NULL },
        { "/LiveOS/squashfs.img", NULL },
    };
    static const fake_file installer[] = {
        { "/images/pxeboot/vmlinuz", NULL },
        { "/images/pxeboot/initrd.img", NULL },
    };

    (void)state;
    expect_family(live, 2, FAMILY_FEDORA_DRACUT, DRACUT_TMPL);
    /* Installer media has no /LiveOS at all. */
    expect_family(installer, 2, FAMILY_FEDORA_DRACUT, DRACUT_TMPL);
    /* dmsquash-live stacks on device-mapper by construction, so this one
     * column is known rather than unknown. */
    assert_int_equal(family_by_id(FAMILY_FEDORA_DRACUT)->initramfs_has_dm,
                     FAMILY_TRI_YES);
}

/* The layout of the 20260510 minimal ISO: genkernel's names for the kernel
 * and initramfs, dracut's squashfs at the root. The kernel alone is not
 * enough; with the squashfs beside it, it is. */
static void test_family_gentoo(void **state)
{
    static const fake_file full[] = {
        { "/boot/gentoo", NULL },
        { "/boot/gentoo.igz", NULL },
        { "/image.squashfs", NULL },
        { "/memtest.efi64", NULL },
    };
    static const fake_file igz[] = {
        { "/boot/gentoo.igz", NULL },
    };
    static const fake_file kernel_and_squash[] = {
        { "/boot/gentoo", NULL },
        { "/image.squashfs", NULL },
    };
    static const fake_file kernel_only[] = {
        { "/boot/gentoo", NULL },
    };

    (void)state;
    expect_family(full, 4, FAMILY_GENTOO, GENTOO_TMPL);
    expect_family(igz, 1, FAMILY_GENTOO, GENTOO_TMPL);
    expect_family(kernel_and_squash, 2, FAMILY_GENTOO, GENTOO_TMPL);
    expect_family(kernel_only, 1, FAMILY_UNKNOWN, NULL);
    assert_int_equal(family_by_id(FAMILY_GENTOO)->initramfs_has_exfat,
                     FAMILY_TRI_YES);
}

static void test_family_arch_layout(void **state)
{
    static const fake_file t[] = {
        { "/arch", NULL },
        { "/arch/boot/x86_64/vmlinuz-linux", NULL },
        { "/arch/x86_64/airootfs.sfs", NULL },
    };
    static const fake_file ver[] = {
        { "/arch/version", NULL },
    };

    (void)state;
    expect_family(t, 3, FAMILY_ARCH, ARCHISO_TMPL);
    expect_family(ver, 1, FAMILY_ARCH, ARCHISO_TMPL);
}

/* The content fingerprint: a medium with no /arch directory visible, only a
 * systemd-boot entry that names the base directory. Each filename archiso has
 * shipped over the years has to work, which is the point of the candidate
 * list in family.c. */
static void test_family_arch_loader_entry(void **state)
{
    static const char *const entries[] = {
        "/loader/entries/01-archiso-x86_64-linux.conf",
        "/loader/entries/02-archiso-x86_64-speech-linux.conf",
        "/loader/entries/archiso-x86_64-linux.conf",
        "/loader/entries/archiso-x86_64.conf",
    };
    static const char conf[] =
        "title   Arch Linux install medium\n"
        "linux   /arch/boot/x86_64/vmlinuz-linux\n"
        "initrd  /arch/boot/x86_64/initramfs-linux.img\n"
        "options archisobasedir=arch archisosearchuuid=ABCD-1234\n";
    size_t i;

    (void)state;
    for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        fake_file f[1];

        f[0].path = entries[i];
        f[0].content = conf;
        expect_family(f, 1, FAMILY_ARCH, ARCHISO_TMPL);
    }
}

/* A loader entry that exists but names someone else's base directory is not
 * an archiso: the needle is the whole fingerprint. */
static void test_family_arch_loader_entry_other_basedir(void **state)
{
    static const char conf[] =
        "title   Something else\n"
        "options archisobasedir2=nope root=/dev/sda1\n";
    static const fake_file f[] = {
        { "/loader/entries/archiso-x86_64.conf", conf },
    };

    (void)state;
    /* "archisobasedir=" with the '=' is what is searched for, so a longer
     * option name that merely starts the same way must not match. */
    expect_family(f, 1, FAMILY_UNKNOWN, NULL);
}

static void test_family_memtest(void **state)
{
    static const fake_file a[] = { { "/boot/memtest", NULL } };
    static const fake_file b[] = { { "/boot/memtest.bin", NULL } };
    static const fake_file c[] = { { "/EFI/BOOT/memtest.efi", NULL } };
    static const fake_file d[] = { { "/memtest.efi", NULL } };

    (void)state;
    /* memtest needs no cmdline, no initramfs and no backing media once
     * loaded; the NULL template is the record of that. */
    expect_family(a, 1, FAMILY_MEMTEST, NULL);
    expect_family(b, 1, FAMILY_MEMTEST, NULL);
    expect_family(c, 1, FAMILY_MEMTEST, NULL);
    expect_family(d, 1, FAMILY_MEMTEST, NULL);
    assert_int_equal(family_by_id(FAMILY_MEMTEST)->initramfs_has_exfat,
                     FAMILY_TRI_NO);
}

/* NixOS's minimal ISO, as it ships (26.05): memtest86+ is a menu extra
 * beside a Linux live root, not the medium.
 *
 * This asserted FAMILY_UNKNOWN while NixOS was special-cased into one purely
 * to keep the memtest last resort off it. It has a row now, which says the
 * same thing about the medium without the medium having to be nameless, and
 * the memtest extra still must not claim it. */
static void test_family_memtest_extra_on_nixos(void **state)
{
    static const fake_file n[] = {
        { "/boot/memtest.bin", NULL },
        { "/nix-store.squashfs", NULL },
    };
    static const fake_file installer[] = {
        { "/boot/memtest.bin", NULL },
        { "/EFI", NULL },
        { "/EFI/nixos-installer-image", NULL },
    };

    (void)state;
    expect_family(n, 2, FAMILY_NIXOS, NULL);
    expect_family(installer, 3, FAMILY_NIXOS, NULL);
}

/* The other four media that had no row and so read as unknown or as memtest.
 * Each fingerprint is the one its own tooling writes. */
static void test_family_installer_media(void **state)
{
    static const fake_file alpine[] = {
        { "/.alpine-release", NULL },
        { "/apks", NULL },
        { "/apks/.boot_repository", NULL },
    };
    static const fake_file deb[] = {
        { "/.disk", NULL },
        { "/.disk/base_installable", NULL },
        { "/.disk/cd_type", NULL },
    };
    static const fake_file pve[] = {
        { "/.pve-cd-id.txt", NULL },
        { "/boot", NULL },
        { "/boot/memtest", NULL },
    };
    static const fake_file esxi[] = {
        { "/B.B00", NULL },
        { "/BOOT.CFG", NULL },
        { "/.DISCINFO", NULL },
    };

    (void)state;
    expect_family(alpine, 3, FAMILY_ALPINE, NULL);
    expect_family(deb, 3, FAMILY_DEBIAN_INSTALLER, NULL);
    /* Proxmox ships /boot/memtest too, so this also pins the ordering. */
    expect_family(pve, 3, FAMILY_PROXMOX, NULL);
    expect_family(esxi, 3, FAMILY_ESXI, NULL);
}

/* DevLink recovery: its isolinux layout and a config naming it. It owns
 * COM1 (its agent's port), and it is the only family that does. A medium
 * with the same generic 8.3 file names but another config is not it. */
static void test_family_devlink(void **state)
{
    static const fake_file devlink[] = {
        { "/isolinux/isolinux.bin", NULL },
        { "/isolinux/bzimage", NULL },
        { "/isolinux/initrd.xz", NULL },
        { "/isolinux/isolinux.cfg",
          "UI menu.c32\nDEFAULT rescue\nLABEL rescue\n"
          "  MENU LABEL DevLink recovery\n  KERNEL /isolinux/bzimage\n" },
    };
    static const fake_file other[] = {
        { "/isolinux/isolinux.bin", NULL },
        { "/isolinux/bzimage", NULL },
        { "/isolinux/initrd.xz", NULL },
        { "/isolinux/isolinux.cfg",
          "DEFAULT linux\nLABEL linux\n  KERNEL /isolinux/bzimage\n" },
    };
    const family_desc *d;
    size_t i, n;

    (void)state;
    expect_family(devlink, 4, FAMILY_DEVLINK, NULL);
    d = family_by_id(FAMILY_DEVLINK);
    assert_int_equal(d->owns_serial, 1);
    assert_int_equal(d->needs_injection, 0);
    expect_family(other, 4, FAMILY_UNKNOWN, NULL);

    d = family_table(&n);
    for (i = 0; i < n; i++)
        assert_int_equal(d[i].owns_serial, d[i].id == FAMILY_DEVLINK);
}

/* BOOT.CFG on its own is a name another medium could plausibly use, so the
 * ESXi fingerprint needs both halves. */
static void test_family_esxi_needs_both_halves(void **state)
{
    static const fake_file half[] = {
        { "/BOOT.CFG", NULL },
    };

    (void)state;
    expect_family(half, 1, FAMILY_UNKNOWN, NULL);
}

/* PassMark's MemTest86 USB image, as it actually ships (read off
 * memtest86-usb.img v10.x, 2026-09-13): every FAT volume inside it is a bare
 * /EFI/BOOT with the loader under the removable-media default name, so the
 * only fingerprints that reach it are the two files beside that loader. This
 * is the tree the host tool sees when it mounts one of those volumes, and
 * getting it wrong means the bless tool either refuses the image or blesses
 * it as something it is not. */
static void test_family_memtest_usb_image_volume(void **state)
{
    static const fake_file splash[] = {
        { "/EFI", NULL },
        { "/EFI/BOOT", NULL },
        { "/EFI/BOOT/BOOTX64.EFI", NULL },
        { "/EFI/BOOT/mt86.png", NULL },
    };
    static const fake_file blacklist[] = {
        { "/EFI/BOOT/BOOTX64.EFI", NULL },
        { "/EFI/BOOT/blacklist.cfg", NULL },
    };
    /* The same volume without either: a bare /EFI/BOOT is every removable
     * medium ever made, so it must NOT classify as memtest on that alone. */
    static const fake_file bare[] = {
        { "/EFI", NULL },
        { "/EFI/BOOT", NULL },
        { "/EFI/BOOT/BOOTX64.EFI", NULL },
    };

    (void)state;
    expect_family(splash, 4, FAMILY_MEMTEST, NULL);
    expect_family(blacklist, 2, FAMILY_MEMTEST, NULL);
    expect_family(bare, 3, FAMILY_UNKNOWN, NULL);
}

/* A bare .efi on p2 has no tree at all, so its file name is the whole
 * evidence (DESIGN Section 7.2). The classifier is deliberately the weakest
 * in the module and nothing may branch on it; what is pinned here is that it
 * recognises the names memtest ships under, that it is case-insensitive
 * because exFAT is, and above all that it claims nothing about a payload it
 * does not recognise. */
static void test_family_payload_by_name(void **state)
{
    (void)state;
    assert_int_equal(family_detect_payload("memtest86.efi")->id,
                     FAMILY_MEMTEST);
    assert_int_equal(family_detect_payload("MemTest86.efi")->id,
                     FAMILY_MEMTEST);
    assert_int_equal(family_detect_payload("MEMTEST.EFI")->id,
                     FAMILY_MEMTEST);
    assert_int_equal(family_detect_payload("mt86.efi")->id, FAMILY_MEMTEST);
    assert_int_equal(family_detect_payload("memtest")->id, FAMILY_MEMTEST);

    assert_int_equal(family_detect_payload("Shell.efi")->id, FAMILY_UNKNOWN);
    assert_int_equal(family_detect_payload("BOOTX64.EFI")->id,
                     FAMILY_UNKNOWN);
    assert_int_equal(family_detect_payload("")->id, FAMILY_UNKNOWN);
    assert_int_equal(family_detect_payload("m")->id, FAMILY_UNKNOWN);
    /* Total, like everything else in this module: no caller has an error
     * path to write for a name it could not classify. */
    assert_int_equal(family_detect_payload(NULL)->id, FAMILY_UNKNOWN);
}

/* The name classifier must not reach the fingerprint path or vice versa: a
 * tree is classified by what is in it, a payload by what it is called, and an
 * ISO called memtest.iso full of Arch is Arch. */
static void test_family_payload_does_not_override_a_tree(void **state)
{
    static const fake_file arch[] = { { "/arch/version", NULL } };

    (void)state;
    expect_family(arch, 1, FAMILY_ARCH, "img_dev=%DEV% img_loop=%ISO%");
    assert_int_equal(family_detect_payload("memtest-arch.efi")->id,
                     FAMILY_MEMTEST);
}

static void test_family_unknown(void **state)
{
    static const fake_file empty[] = { { "/unused", NULL } };
    static const fake_file plausible[] = {
        { "/EFI/BOOT/BOOTX64.EFI", NULL },
        { "/isolinux/isolinux.cfg", NULL },
        { "/README.TXT", NULL },
    };

    (void)state;
    /* A tree with nothing in it at all. */
    expect_family(empty, 0, FAMILY_UNKNOWN, NULL);
    /* A bootable ISO that is none of the families we know: still unknown, and
     * unknown claims no strategy. */
    expect_family(plausible, 3, FAMILY_UNKNOWN, NULL);
    assert_int_equal(family_by_id(FAMILY_UNKNOWN)->needs_injection, 0);
    assert_int_equal(family_by_id(FAMILY_UNKNOWN)->initramfs_has_exfat,
                     FAMILY_TRI_UNKNOWN);
}

/* ------------------------------------------------------------ precedence */

/* SystemRescue is archiso underneath. Its media can carry both /arch and
 * archiso's loader entries, and classifying it as Arch would hand it
 * archisobasedir=arch when its own base directory is sysresccd. */
static void test_family_systemrescue_beats_arch(void **state)
{
    static const char conf[] =
        "options archisobasedir=sysresccd archisolabel=RESCUE\n";
    static const fake_file layout[] = {
        { "/sysresccd", NULL },
        { "/arch", NULL },
        { "/arch/boot/x86_64/vmlinuz-linux", NULL },
        { "/arch/version", NULL },
    };
    static const fake_file content_only[] = {
        { "/loader/entries/01-archiso-x86_64-linux.conf", conf },
        { "/arch", NULL },
    };

    (void)state;
    expect_family(layout, 4, FAMILY_SYSTEMRESCUE, SYSRESC_TMPL);
    /* Content fingerprint alone, with the Arch layout also present: the
     * sysresccd needle is checked first and wins. */
    expect_family(content_only, 2, FAMILY_SYSTEMRESCUE, SYSRESC_TMPL);
}

/* Ubuntu derivatives sometimes ship a /live directory of leftovers while
 * actually booting from /casper. findiso would not find anything. */
static void test_family_casper_beats_live(void **state)
{
    static const fake_file t[] = {
        { "/casper", NULL },
        { "/casper/vmlinuz", NULL },
        { "/live", NULL },
        { "/live/filesystem.squashfs", NULL },
    };

    (void)state;
    expect_family(t, 4, FAMILY_DEBIAN_CASPER, CASPER_TMPL);
}

static void test_family_precedence_chain(void **state)
{
    /* Everything at once. Each case removes the winner and asserts the next
     * one down, which walks the whole ordering in family_detect and would
     * catch a reordering that no single-family test can see. */
    static const fake_file all[] = {
        { "/sources/install.wim", NULL },
        { "/sysresccd", NULL },
        { "/casper", NULL },
        { "/live", NULL },
        { "/LiveOS", NULL },
        { "/boot/gentoo.igz", NULL },
        { "/arch", NULL },
        { "/boot/memtest", NULL },
    };
    static const family_id order[] = {
        FAMILY_WINDOWS, FAMILY_SYSTEMRESCUE, FAMILY_DEBIAN_CASPER,
        FAMILY_DEBIAN_LIVE, FAMILY_FEDORA_DRACUT, FAMILY_GENTOO, FAMILY_ARCH,
        FAMILY_MEMTEST,
    };
    size_t skipped;

    (void)state;
    for (skipped = 0; skipped < sizeof(order) / sizeof(order[0]); skipped++) {
        const family_desc *d;
        family_probe p;
        fake_tree t;

        tree_probe(&p, &t, all + skipped, sizeof(all) / sizeof(all[0]) -
                   skipped);
        d = family_detect(&p);
        assert_int_equal(d->id, order[skipped]);
    }

    /* And with every fingerprint removed, unknown. */
    {
        family_probe p;
        fake_tree t;

        tree_probe(&p, &t, all, 0);
        assert_int_equal(family_detect(&p)->id, FAMILY_UNKNOWN);
    }
}

/* ------------------------------------------------------------- callbacks */

static void test_family_null_probe(void **state)
{
    const family_desc *d = family_detect(NULL);

    (void)state;
    /* Never NULL, whatever it is handed: the caller gets a real descriptor to
     * branch on rather than a pointer to check. */
    assert_non_null(d);
    assert_int_equal(d->id, FAMILY_UNKNOWN);
}

/* A probe with no read callback is the layout-only mode the header documents.
 * Path-based families must still classify; content-fingerprinted ones fall
 * through to unknown instead of crashing on the missing callback. */
static void test_family_null_read_callback(void **state)
{
    static const char conf[] = "options archisobasedir=arch\n";
    static const fake_file layout[] = {
        { "/casper/vmlinuz", NULL },
    };
    static const fake_file content_only[] = {
        { "/loader/entries/archiso-x86_64.conf", conf },
    };
    family_probe p;
    fake_tree t;

    (void)state;
    tree_probe(&p, &t, layout, 1);
    p.read = NULL;
    assert_int_equal(family_detect(&p)->id, FAMILY_DEBIAN_CASPER);

    tree_probe(&p, &t, content_only, 1);
    p.read = NULL;
    assert_int_equal(family_detect(&p)->id, FAMILY_UNKNOWN);
    assert_int_equal(t.read_calls, 0);
}

/* The mirror: no exists callback. Everything layout-based disappears, and a
 * content fingerprint can still classify the medium on its own. */
static void test_family_null_exists_callback(void **state)
{
    static const char conf[] = "options archisobasedir=arch\n";
    static const fake_file content_only[] = {
        { "/loader/entries/archiso-x86_64.conf", conf },
    };
    static const fake_file layout[] = {
        { "/casper/vmlinuz", NULL },
    };
    family_probe p;
    fake_tree t;

    (void)state;
    tree_probe(&p, &t, content_only, 1);
    p.exists = NULL;
    assert_int_equal(family_detect(&p)->id, FAMILY_ARCH);

    tree_probe(&p, &t, layout, 1);
    p.exists = NULL;
    assert_int_equal(family_detect(&p)->id, FAMILY_UNKNOWN);

    /* Neither callback at all. */
    p.read = NULL;
    assert_int_equal(family_detect(&p)->id, FAMILY_UNKNOWN);
}

/* A read that fails is a fingerprint that does not match, not an error the
 * caller has to handle. fake_read returns -1 for a path with no content. */
static void test_family_unreadable_fingerprint(void **state)
{
    static const fake_file t[] = {
        { "/loader/entries/archiso-x86_64.conf", NULL },
    };

    (void)state;
    expect_family(t, 1, FAMILY_UNKNOWN, NULL);
}

/* A file shorter than the needle cannot contain it. Worth its own case
 * because the search is a bounded scan and an unguarded one would read past
 * the delivered bytes on exactly this input: a truncated or empty loader
 * entry, which is what a half-written ISO on the stick looks like. */
static void test_family_fingerprint_shorter_than_needle(void **state)
{
    static const fake_file truncated[] = {
        { "/loader/entries/archiso-x86_64.conf", "ti" },
    };
    static const fake_file empty[] = {
        { "/loader/entries/archiso-x86_64.conf", "" },
    };

    (void)state;
    expect_family(truncated, 1, FAMILY_UNKNOWN, NULL);
    expect_family(empty, 1, FAMILY_UNKNOWN, NULL);
}

/* Writes the needle across a stack region larger than the module's read
 * window, so a probe frame laid down afterwards sits on bytes that would match
 * if anything scanned past what a callback actually delivered.
 *
 * Nothing here asserts the two frames overlap, and the test passes either way;
 * when they do overlap this is the difference between a deterministic answer
 * and a fingerprint conjured out of stack residue. */
static volatile char stack_sink;

static void dirty_stack(const char *needle)
{
    char scratch[8192];
    size_t nl = strlen(needle);
    size_t i;

    for (i = 0; i + nl <= sizeof(scratch); i += nl)
        memcpy(scratch + i, needle, nl);
    /* So the writes cannot be discarded as dead. */
    stack_sink = scratch[sizeof(scratch) - 1u];
}

/* A callback that reports more bytes than it delivered must not be able to
 * turn whatever was on the stack into a fingerprint. Two shapes, refused by
 * two different rules:
 *
 *   past the buffer    got exceeds the length offered, so the callback cannot
 *                      describe its own result and the read is dropped whole.
 *   inside the buffer  got is within the offer but past what was written,
 *                      which is indistinguishable from a file that filled the
 *                      window. The scan runs, and must run over the zeroes the
 *                      module lays down first. */
static void test_family_lying_read_callback(void **state)
{
    static const char conf[] = "options archisobasedir=arch\n";
    /* No needle anywhere in it: the only way this classifies as Arch is bytes
     * the callback never delivered. */
    static const char quiet_conf[] = "title   Something else\n";
    static const fake_file t[] = {
        { "/loader/entries/archiso-x86_64.conf", conf },
    };
    static const fake_file quiet[] = {
        { "/loader/entries/archiso-x86_64.conf", quiet_conf },
    };
    family_probe p;
    fake_tree tr;

    (void)state;
    tree_probe(&p, &tr, t, 1);
    tr.lie_about_length = 1;
    assert_int_equal(family_detect(&p)->id, FAMILY_UNKNOWN);
    assert_true(tr.read_calls > 0);

    /* Both needles the module searches for, because either one left on the
     * stack would classify the medium. */
    dirty_stack("archisobasedir=arch");
    tree_probe(&p, &tr, quiet, 1);
    tr.over_report = 1;
    assert_int_equal(family_detect(&p)->id, FAMILY_UNKNOWN);
    assert_true(tr.read_calls > 0);

    dirty_stack("archisobasedir=sysresccd");
    tree_probe(&p, &tr, quiet, 1);
    tr.over_report = 1;
    assert_int_equal(family_detect(&p)->id, FAMILY_UNKNOWN);
}

/* ------------------------------------------------------------- registry */

/* A FreeBSD medium: its kernel and the defaults file its loader reads. The
 * pair is what pfSense, OPNsense, GhostBSD and TrueNAS all keep, and no Linux
 * layout in the table can match it. */
static void test_family_bsd(void **state)
{
    static const fake_file both[] = {
        { "/boot/kernel/kernel", NULL },
        { "/boot/defaults/loader.conf", NULL },
    };
    static const fake_file kernel_only[] = {
        { "/boot/kernel/kernel", NULL },
    };

    (void)state;
    expect_family(both, 2, FAMILY_BSD, NULL);
    /* Half the fingerprint is not the fingerprint. */
    expect_family(kernel_only, 1, FAMILY_UNKNOWN, NULL);
    assert_int_equal(family_by_id(FAMILY_BSD)->needs_injection, 0);
}

static void test_family_registry_round_trip(void **state)
{
    size_t count = 0;
    const family_desc *t = family_table(&count);
    size_t i;

    (void)state;
    assert_non_null(t);
    assert_int_equal(count, 18);

    for (i = 0; i < count; i++) {
        /* Every row is reachable by its own id, and reachable as the same
         * object: callers compare descriptor pointers. */
        assert_ptr_equal(family_by_id(t[i].id), &t[i]);
        assert_non_null(t[i].name);
        assert_true(t[i].name[0] != '\0');
        assert_true(t[i].needs_injection <= 1u);
        /* Cast so the range check is a real comparison rather than something
         * the compiler folds away from the enum's declared range. */
        assert_true((unsigned)t[i].initramfs_has_exfat <=
                    (unsigned)FAMILY_TRI_YES);
        assert_true((unsigned)t[i].initramfs_has_dm <=
                    (unsigned)FAMILY_TRI_YES);
    }

    /* Ids are unique: a duplicate would make the round-trip above pass while
     * one of the rows was unreachable. */
    for (i = 0; i < count; i++) {
        size_t j;

        for (j = i + 1; j < count; j++)
            assert_true(t[i].id != t[j].id);
    }

    /* family.c falls back to g_families[0] for an unknown id, so row 0 has to
     * be the unknown one. */
    assert_int_equal(t[0].id, FAMILY_UNKNOWN);

    /* An id that is not in the table yields the unknown descriptor, not NULL
     * and not a stray row. */
    assert_ptr_equal(family_by_id((family_id)9999), &t[0]);
    assert_ptr_equal(family_by_id((family_id)255), &t[0]);

    /* The count argument is optional. */
    assert_ptr_equal(family_table(NULL), t);
}

/* Every character of every string in the registry must be plain printable
 * ASCII: these end up in a UEFI text-mode menu and on a kernel cmdline, where
 * anything else is at best mojibake and at worst an unbootable option. */
static void test_family_strings_are_ascii(void **state)
{
    size_t count = 0;
    const family_desc *t = family_table(&count);
    size_t i;

    (void)state;
    for (i = 0; i < count; i++) {
        const char *s;
        size_t j;

        for (s = t[i].name; *s != '\0'; s++)
            assert_true((unsigned char)*s >= 0x20u &&
                        (unsigned char)*s < 0x7Fu);
        if (t[i].cmdline_template == NULL)
            continue;
        s = t[i].cmdline_template;
        for (j = 0; s[j] != '\0'; j++) {
            assert_true((unsigned char)s[j] >= 0x20u &&
                        (unsigned char)s[j] < 0x7Fu);
            /* A cmdline is whitespace-separated; a tab or a newline in a
             * template would split an option in a way nobody intended. */
            assert_true(s[j] != '\t');
        }
        assert_true(j > 0);
    }
}

/* The only placeholders the header defines are %ISO%, %DEV% and %LABEL%. A
 * template with %ISOFILE% or a stray '%' would be substituted wrongly by the
 * consumer and produce a cmdline that fails at boot with no diagnostic. */
static void test_family_templates_use_known_tokens(void **state)
{
    size_t count = 0;
    const family_desc *t = family_table(&count);
    size_t i;

    (void)state;
    for (i = 0; i < count; i++) {
        const char *s = t[i].cmdline_template;
        size_t j = 0;
        unsigned tokens = 0;

        if (s == NULL)
            continue;
        while (s[j] != '\0') {
            const char *end;
            size_t len;

            if (s[j] != '%') {
                j++;
                continue;
            }
            end = strchr(s + j + 1, '%');
            /* An unpaired '%' is a malformed template. */
            assert_non_null(end);
            len = (size_t)(end - (s + j + 1));
            assert_true((len == 3 && (memcmp(s + j + 1, "ISO", 3) == 0 ||
                                      memcmp(s + j + 1, "DEV", 3) == 0)) ||
                        (len == 5 && memcmp(s + j + 1, "LABEL", 5) == 0));
            tokens++;
            j += len + 2u;
        }
        /* Every template that exists at all names the ISO: a fragment with no
         * placeholder would be a constant, which no family needs. */
        assert_true(tokens > 0);
        assert_non_null(strstr(s, "%ISO%"));
    }

    /* And the two families that need the device get it. */
    assert_non_null(strstr(family_by_id(FAMILY_ARCH)->cmdline_template,
                           "%DEV%"));
    assert_non_null(strstr(family_by_id(FAMILY_SYSTEMRESCUE)->cmdline_template,
                           "%DEV%"));
}

/* Detection must classify from the fingerprints alone, without needing a
 * particular order of probe calls or a particular number of them. This pins
 * the cheap-first property: a medium with an early match does not read every
 * loader entry on the ISO. */
static void test_family_probe_effort(void **state)
{
    static const fake_file win[] = { { "/sources/install.wim", NULL } };
    family_probe p;
    fake_tree t;

    (void)state;
    tree_probe(&p, &t, win, 1);
    assert_int_equal(family_detect(&p)->id, FAMILY_WINDOWS);
    /* Windows is the first check, so at most the two /sources probes. */
    assert_true(t.exists_calls <= 2u);
    assert_int_equal(t.read_calls, 0);
}

/* ------------------------------------------------------------------ run */

int run_family_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_family_windows),
        cmocka_unit_test(test_family_windows_xp),
        cmocka_unit_test(test_family_windows_only_on_the_udf_volume),
        cmocka_unit_test(test_family_winpe),
        cmocka_unit_test(test_family_systemrescue),
        cmocka_unit_test(test_family_casper),
        cmocka_unit_test(test_family_debian_live),
        cmocka_unit_test(test_family_fedora_dracut),
        cmocka_unit_test(test_family_arch_layout),
        cmocka_unit_test(test_family_arch_loader_entry),
        cmocka_unit_test(test_family_arch_loader_entry_other_basedir),
        cmocka_unit_test(test_family_memtest),
        cmocka_unit_test(test_family_memtest_extra_on_nixos),
        cmocka_unit_test(test_family_installer_media),
        cmocka_unit_test(test_family_esxi_needs_both_halves),
        cmocka_unit_test(test_family_devlink),
        cmocka_unit_test(test_family_gentoo),
        cmocka_unit_test(test_family_memtest_usb_image_volume),
        cmocka_unit_test(test_family_payload_by_name),
        cmocka_unit_test(test_family_payload_does_not_override_a_tree),
        cmocka_unit_test(test_family_unknown),
        cmocka_unit_test(test_family_systemrescue_beats_arch),
        cmocka_unit_test(test_family_casper_beats_live),
        cmocka_unit_test(test_family_precedence_chain),
        cmocka_unit_test(test_family_null_probe),
        cmocka_unit_test(test_family_null_read_callback),
        cmocka_unit_test(test_family_null_exists_callback),
        cmocka_unit_test(test_family_unreadable_fingerprint),
        cmocka_unit_test(test_family_fingerprint_shorter_than_needle),
        cmocka_unit_test(test_family_lying_read_callback),
        cmocka_unit_test(test_family_bsd),
        cmocka_unit_test(test_family_registry_round_trip),
        cmocka_unit_test(test_family_strings_are_ascii),
        cmocka_unit_test(test_family_templates_use_known_tokens),
        cmocka_unit_test(test_family_probe_effort),
    };

    return cmocka_run_group_tests_name("ISO family detection", tests, NULL,
                                       NULL);
}
