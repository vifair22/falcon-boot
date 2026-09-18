/* Tests for falcon-core/src/media/bootcfg.c.
 *
 * Every case is a small tree of config files in memory, each reproducing the
 * shape one distro's generated config actually has (the corpus in
 * tools/coverage/corpus.tsv is where each shape was read): Debian's source
 * chain and ${iso_path} in the main grub.cfg, Fedora's search --set=root and
 * ($root) prefixes and default="1", Ubuntu's loopback.cfg, Arch's loopback.cfg
 * whose device is computed at run time plus its systemd-boot entries,
 * SystemRescue's whole menu inside an eval and a CD boot that only has a
 * loopback.cfg, openSUSE's conditional ${isoboot} and inline "{ true; }"
 * blocks, Manjaro's menu built in a loop, Alpine's several initrds, and an
 * isolinux config with INCLUDE and APPEND initrd=.
 *
 * What is pinned is what a consumer boots: the entry picked for each kind,
 * its kernel, its initrds and its rendered cmdline. Then the edges: the
 * bounds, the refusals, and the rendering rules.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <media/bootcfg.h>

typedef struct {
    const char *path;
    const char *content;
} ff;

typedef struct {
    const ff *files;
    size_t n;
    unsigned reads;
} ftree;

static int fr_read(void *ctx, const char *path, char *buf, size_t cap,
                   size_t *len)
{
    ftree *t = ctx;
    size_t i;

    t->reads++;
    for (i = 0; i < t->n; i++) {
        size_t n;

        if (strcasecmp(t->files[i].path, path) != 0)
            continue;
        n = strlen(t->files[i].content);
        *len = n;
        if (n > cap)
            return 1;
        memcpy(buf, t->files[i].content, n);
        return 0;
    }
    return 1;
}

static int fr_list(void *ctx, const char *dir,
                   int (*cb)(void *arg, const char *name), void *arg)
{
    ftree *t = ctx;
    size_t dl = strlen(dir), i;
    int found = 0;

    for (i = 0; i < t->n; i++) {
        const char *p = t->files[i].path;

        if (strncasecmp(p, dir, dl) != 0 || p[dl] != '/' ||
            strchr(p + dl + 1, '/') != NULL)
            continue;
        found = 1;
        if (cb(arg, p + dl + 1))
            break;
    }
    return found ? 0 : 1;
}

static int fr_exists(void *ctx, const char *path)
{
    ftree *t = ctx;
    size_t i;

    for (i = 0; i < t->n; i++) {
        if (strcasecmp(t->files[i].path, path) == 0)
            return 1;
    }
    return 0;
}

static char g_scratch[256 * 1024];
static bootcfg_result g_res;

/* Reads kind from files; returns the rc and leaves the result in g_res. */
static int run(const ff *files, size_t n, bootcfg_kind kind, int with_exists)
{
    ftree t = { files, n, 0 };
    bootcfg_src s = { fr_read, fr_list, with_exists ? fr_exists : NULL, &t };

    return bootcfg_read(&s, kind, g_scratch, sizeof g_scratch, &g_res);
}

static const bootcfg_entry *picked(void)
{
    assert_true(g_res.pick < g_res.n);
    return &g_res.entry[g_res.pick];
}

static void rendered(const char *iso, const char *want)
{
    char out[BOOTCFG_CMDLINE_MAX];

    assert_int_equal(bootcfg_render(picked(), iso, out, sizeof out),
                     BOOTCFG_OK);
    assert_string_equal(out, want);
}

#define N(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------- Debian */

static const ff debian[] = {
    { "/boot/grub/grub.cfg",
      "source /boot/grub/config.cfg\n"
      "\n"
      "# Live boot\n"
      "menuentry \"Live system (amd64)\" --hotkey=l {\n"
      "\tlinux\t/live/vmlinuz-6.12 boot=live components quiet splash findiso=${iso_path}\n"
      "\tinitrd\t/live/initrd.img-6.12\n"
      "}\n"
      "# menuentry \"commented\" {\n"
      "if true; then\n"
      "source\t/boot/grub/install_start.cfg\n"
      "submenu 'Advanced install options ...' --hotkey=a {\n"
      "\tsource /boot/grub/install.cfg\n"
      "}\n"
      "fi\n" },
    { "/boot/grub/config.cfg", "set timeout=30\nset gfxmode=auto\n" },
    { "/boot/grub/install_start.cfg",
      "menuentry 'Start installer' { linux /install/vmlinuz vga=788 --- quiet\n"
      "initrd /install/initrd.gz\n}\n" },
    { "/boot/grub/install.cfg",
      "menuentry 'Expert' { linux /install/vmlinuz priority=low\n}\n" },
    { "/boot/grub/loopback.cfg", "source /boot/grub/grub.cfg\n" },
    { "/live/vmlinuz-6.12", "" },
    { "/live/initrd.img-6.12", "" },
    { "/install/vmlinuz", "" },
    { "/install/initrd.gz", "" },
};

static void test_bootcfg_debian(void **state)
{
    (void)state;
    assert_int_equal(run(debian, N(debian), BOOTCFG_CD, 1), BOOTCFG_OK);
    assert_string_equal(picked()->title, "Live system (amd64)");
    assert_string_equal(picked()->kernel, "/live/vmlinuz-6.12");
    assert_int_equal(picked()->n_initrd, 1);
    assert_string_equal(picked()->initrd[0], "/live/initrd.img-6.12");
    assert_string_equal(g_res.source, "/boot/grub/grub.cfg");
    /* A CD boot leaves ${iso_path} empty, and the empty argument goes. */
    rendered(NULL, "boot=live components quiet splash");
    /* Both entries of the source chain and the submenu were read; the one
     * in the submenu is marked nested. */
    assert_int_equal(g_res.n, 3);
    assert_true(g_res.entry[2].nested);

    assert_int_equal(run(debian, N(debian), BOOTCFG_LOOP, 1), BOOTCFG_OK);
    assert_true(picked()->loop);
    assert_true(bootcfg_takes_iso_path(picked()));
    rendered("/x/debian.iso",
             "boot=live components quiet splash findiso=/x/debian.iso");
}

/* ------------------------------------------------------------- Fedora */

static const ff fedora[] = {
    { "/boot/grub2/grub.cfg",
      "set default=\"1\"\n"
      "function load_video {\n"
      "  insmod efi_gop\n"
      "  menuentry 'inside a function' { linux /nope }\n"
      "}\n"
      "if [ \"$grub_platform\" == \"efi\" ]; then\n"
      "\tset basicgfx=\"nomodeset\"\n"
      "else\n"
      "\tset basicgfx=\"nomodeset vga=791\"\n"
      "fi\n"
      "search --file --set=root /boot/0xf20784ad\n"
      "menuentry \"Start Fedora-Workstation-Live\" --class fedora --class gnu-linux {\n"
      "\tlinux ($root)/boot/x86_64/loader/linux quiet rhgb  root=live:CDLABEL=Fedora-WS-Live-44 rd.live.image\n"
      "\tinitrd ($root)/boot/x86_64/loader/initrd\n"
      "}\n"
      "menuentry \"Test this media & start\" --class fedora {\n"
      "\tlinux ($root)/boot/x86_64/loader/linux rd.live.check\n"
      "\tinitrd ($root)/boot/x86_64/loader/initrd\n"
      "}\n"
      "submenu \"Troubleshooting -->\" {\n"
      "\tmenuentry \"basic graphics\" {\n"
      "\t\tlinux ($root)/boot/x86_64/loader/linux ${basicgfx}\n"
      "\t}\n"
      "}\n" },
    { "/boot/grub2/loopback.cfg", "source /boot/grub2/grub.cfg\n" },
    { "/boot/x86_64/loader/linux", "" },
    { "/boot/x86_64/loader/initrd", "" },
};

static void test_bootcfg_fedora(void **state)
{
    (void)state;
    /* The first entry, not set default="1": that is the media check. */
    assert_int_equal(run(fedora, N(fedora), BOOTCFG_CD, 1), BOOTCFG_OK);
    assert_string_equal(picked()->title, "Start Fedora-Workstation-Live");
    assert_string_equal(picked()->kernel, "/boot/x86_64/loader/linux");
    assert_false(picked()->unresolved);
    rendered(NULL, "quiet rhgb root=live:CDLABEL=Fedora-WS-Live-44 rd.live.image");
    /* The function body's menuentry was never read. */
    assert_int_equal(g_res.n, 3);
    /* The known platform took the efi branch. */
    assert_string_equal(g_res.entry[2].cmdline, "nomodeset");
    /* No loopback entry takes ${iso_path}. */
    assert_int_equal(run(fedora, N(fedora), BOOTCFG_LOOP, 1), BOOTCFG_ERR_NONE);
}

/* ---------------------------------------------------- Ubuntu loopback */

static const ff ubuntu[] = {
    { "/boot/grub/grub.cfg",
      "menuentry \"Try or Install Ubuntu\" {\n"
      "\tset gfxpayload=keep\n"
      "\tlinux\t/casper/vmlinuz  --- quiet splash\n"
      "\tinitrd\t/casper/initrd\n"
      "}\n" },
    { "/boot/grub/loopback.cfg",
      "menuentry \"Try or Install Ubuntu\" {\n"
      "    linux  /casper/vmlinuz  iso-scan/filename=${iso_path} --- quiet splash\n"
      "    initrd /casper/initrd\n"
      "}\n" },
    { "/casper/vmlinuz", "" },
    { "/casper/initrd", "" },
};

static void test_bootcfg_ubuntu(void **state)
{
    (void)state;
    assert_int_equal(run(ubuntu, N(ubuntu), BOOTCFG_LOOP, 1), BOOTCFG_OK);
    assert_string_equal(g_res.source, "/boot/grub/loopback.cfg");
    rendered("/ubuntu.iso", "iso-scan/filename=/ubuntu.iso --- quiet splash");
    assert_int_equal(run(ubuntu, N(ubuntu), BOOTCFG_CD, 1), BOOTCFG_OK);
    rendered(NULL, "--- quiet splash");
}

/* --------------------------------------------------- Arch: run time */

static const ff arch[] = {
    { "/boot/grub/loopback.cfg",
      "search --no-floppy --set=archiso_img_dev --file \"${iso_path}\"\n"
      "probe --set archiso_img_dev_uuid --fs-uuid \"${archiso_img_dev}\"\n"
      "default=archlinux\n"
      "menuentry \"Arch Linux install medium\" --class arch --id 'archlinux' {\n"
      "    linux /arch/boot/x86_64/vmlinuz-linux archisobasedir=arch img_dev=UUID=${archiso_img_dev_uuid} img_loop=\"${iso_path}\"\n"
      "    initrd /arch/boot/x86_64/initramfs-linux.img\n"
      "}\n"
      "if [ \"${grub_platform}\" == 'efi' -a -f '/boot/memtest86+/memtest.efi' ]; then\n"
      "    menuentry 'Run Memtest86+ (RAM test)' --class memtest86 {\n"
      "        linux /boot/memtest86+/memtest.efi\n"
      "    }\n"
      "fi\n" },
    { "/loader/entries/02-archiso-speech.conf",
      "title Arch Linux (speech)\nlinux /arch/boot/x86_64/vmlinuz-linux\n"
      "options accessibility=on\n" },
    { "/loader/entries/01-archiso-linux.conf",
      "title    Arch Linux install medium (x86_64, UEFI)\r\n"
      "sort-key 01\n"
      "# a comment\n"
      "linux    /arch/boot/x86_64/vmlinuz-linux\n"
      "initrd   /arch/boot/x86_64/initramfs-linux.img\n"
      "options  archisobasedir=arch archisosearchuuid=2026-09-01\n"
      "options  second=line\n" },
    { "/loader/entries/readme.txt", "not an entry" },
    { "/arch/boot/x86_64/vmlinuz-linux", "" },
    { "/arch/boot/x86_64/initramfs-linux.img", "" },
    { "/boot/memtest86+/memtest.efi", "" },
};

static void test_bootcfg_arch(void **state)
{
    size_t i;
    int saw_unresolved = 0;

    (void)state;
    /* The loopback entry's device is a run-time value: it is read, marked
     * and refused, and the file-boot kind has nothing to offer. */
    assert_int_equal(run(arch, N(arch), BOOTCFG_LOOP, 1), BOOTCFG_ERR_NONE);
    for (i = 0; i < g_res.n; i++) {
        if (g_res.entry[i].unresolved)
            saw_unresolved = 1;
    }
    assert_true(saw_unresolved);
    /* The CD kind falls through to systemd-boot, in file-name order. */
    assert_int_equal(run(arch, N(arch), BOOTCFG_CD, 1), BOOTCFG_OK);
    assert_string_equal(g_res.source, "/loader/entries");
    assert_int_equal(picked()->format, BOOTCFG_FMT_SDBOOT);
    assert_string_equal(picked()->title,
                        "Arch Linux install medium (x86_64, UEFI)");
    assert_string_equal(picked()->initrd[0],
                        "/arch/boot/x86_64/initramfs-linux.img");
    rendered(NULL, "archisobasedir=arch archisosearchuuid=2026-09-01 second=line");
}

/* ------------------------------------------ SystemRescue: eval, -z */

static const ff sysrescue[] = {
    { "/boot/grub/loopback.cfg",
      "if [ -z \"$iso_path\" ]\n"
      "then\n"
      "\techo \"Warning: 'iso_path' is not set.\"\n"
      "elif search --file --set=archiso_grub_dev --no-floppy $iso_path\n"
      "then\n"
      "\tif probe --fs-uuid --set=archiso_fs_uuid $archiso_grub_dev\n"
      "\tthen\n"
      "\t\tarchiso_param=\"img_dev=/dev/disk/by-uuid/$archiso_fs_uuid img_loop=$iso_path\"\n"
      "\tfi\n"
      "fi\n"
      "srcd_skip_init=yes\n"
      "source /boot/grub/grubsrcd.cfg\n" },
    { "/boot/grub/grubsrcd.cfg",
      "if [ -z \"$srcd_skip_init\" ]; then\n"
      "\tset timeout=30\n"
      "fi\n"
      "if [ -z \"$archiso_param\" ]; then\n"
      "\tarchiso_param=\"archisolabel=RESCUE1302\"\n"
      "fi\n"
      "eval \"\n"
      "menuentry 'Boot SystemRescue using default options' {\n"
      "\tlinux /sysresccd/boot/x86_64/vmlinuz archisobasedir=sysresccd $archiso_param iomem=relaxed\n"
      "\tinitrd /sysresccd/boot/intel_ucode.img /sysresccd/boot/amd_ucode.img /sysresccd/boot/x86_64/sysresccd.img\n"
      "}\n"
      "\"\n" },
    { "/sysresccd/boot/x86_64/vmlinuz", "" },
    { "/sysresccd/boot/intel_ucode.img", "" },
    { "/sysresccd/boot/amd_ucode.img", "" },
    { "/sysresccd/boot/x86_64/sysresccd.img", "" },
};

static void test_bootcfg_sysrescue(void **state)
{
    (void)state;
    /* As a file, the device is a run-time value: nothing to offer. */
    assert_int_equal(run(sysrescue, N(sysrescue), BOOTCFG_LOOP, 1),
                     BOOTCFG_ERR_NONE);
    /* As a CD, its loopback.cfg is read with ${iso_path} empty: the -z
     * branch, no search, and the label default applies. */
    assert_int_equal(run(sysrescue, N(sysrescue), BOOTCFG_CD, 1), BOOTCFG_OK);
    assert_string_equal(g_res.source, "/boot/grub/loopback.cfg");
    assert_int_equal(picked()->n_initrd, 3);
    assert_string_equal(picked()->initrd[2], "/sysresccd/boot/x86_64/sysresccd.img");
    rendered(NULL, "archisobasedir=sysresccd archisolabel=RESCUE1302 iomem=relaxed");
}

/* ----------------------------------- openSUSE: conditional, inline {} */

static const ff opensuse[] = {
    { "/EFI/BOOT/grub.cfg", "configfile ($root)/boot/grub2/grub.cfg\n" },
    { "/boot/grub2/grub.cfg",
      "if [ -n \"$extra_cmdline\" ]; then\n"
      "  submenu \"Bootable snapshot\" {\n"
      "    menuentry \"If OK, run snapper rollback and reboot.\" { true; }\n"
      "  }\n"
      "fi\n"
      "if [ -n \"${iso_path}\" ]; then\n"
      "    isoboot=\"iso-scan/filename=${iso_path}\"\n"
      "fi\n"
      "menuentry \"openSUSE Tumbleweed XFCE Live\" --class os --unrestricted {\n"
      "    linux ($root)/boot/x86_64/loader/linux ${extra_cmdline} ${isoboot} splash=silent root=live:CDLABEL=openSUSE_TW rd.live.image\n"
      "    initrd ($root)/boot/x86_64/loader/initrd\n"
      "}\n"
      "menuentry \"Boot from Hard Disk\" --class os { exit }\n" },
    { "/boot/grub2/loopback.cfg", "source /boot/grub2/grub.cfg\n" },
    { "/boot/x86_64/loader/linux", "" },
    { "/boot/x86_64/loader/initrd", "" },
};

static void test_bootcfg_opensuse(void **state)
{
    (void)state;
    assert_int_equal(run(opensuse, N(opensuse), BOOTCFG_LOOP, 1), BOOTCFG_OK);
    rendered("/su.iso", "iso-scan/filename=/su.iso splash=silent "
                        "root=live:CDLABEL=openSUSE_TW rd.live.image");
    /* On a CD the conditional never set $isoboot: nothing to drop. */
    assert_int_equal(run(opensuse, N(opensuse), BOOTCFG_CD, 1), BOOTCFG_OK);
    rendered(NULL, "splash=silent root=live:CDLABEL=openSUSE_TW rd.live.image");
    assert_false(bootcfg_takes_iso_path(picked()));
}

/* -------------------------------------- Manjaro: a menu built in loops */

static const ff manjaro[] = {
    { "/boot/grub/grub.cfg",
      "for kern in /boot/vmlinuz-*; do\n"
      "  menuentry \"$kern\" { linux $kern }\n"
      "done\n"
      "menuentry \"Boot with open source drivers\" {\n"
      "  linux /boot/vmlinuz-$kernel driver=free\n"
      "  initrd /boot/initramfs-x86_64.img\n"
      "}\n" },
    { "/boot/vmlinuz-x86_64", "" },
    { "/boot/initramfs-x86_64.img", "" },
};

static void test_bootcfg_manjaro(void **state)
{
    (void)state;
    /* The loop is skipped whole, and the entry's kernel, built from a
     * variable nobody set, names no file. Nothing to boot. */
    assert_int_equal(run(manjaro, N(manjaro), BOOTCFG_CD, 1), BOOTCFG_ERR_NONE);
    assert_int_equal(g_res.n, 1);
    assert_string_equal(g_res.entry[0].kernel, "/boot/vmlinuz-");
    /* Without the exists callback the parser cannot know; it picks it. */
    assert_int_equal(run(manjaro, N(manjaro), BOOTCFG_CD, 0), BOOTCFG_OK);
}

/* ------------------------------------------------------------ isolinux */

static const ff isolinux[] = {
    { "/isolinux/isolinux.cfg",
      "UI vesamenu.c32\n"
      "DEFAULT live\n"
      "INCLUDE menu.cfg\n" },
    { "/isolinux/menu.cfg",
      "label hdt\n"
      "  menu label ^Hardware Detection Tool\n"
      "  kernel hdt.c32\n"
      "LABEL memtest\n"
      "  KERNEL /boot/memtest\n"
      "LABEL live\n"
      "  MENU LABEL Live\n"
      "  LINUX /live/vmlinuz\n"
      "  APPEND initrd=/live/ucode.img,initrd.gz boot=live components\n"
      "LABEL other\n"
      "  KERNEL vmlinuz2\n"
      "  INITRD initrd2.gz\n"
      "  APPEND quiet\n" },
    { "/live/vmlinuz", "" },
    { "/live/ucode.img", "" },
    { "/isolinux/initrd.gz", "" },
};

static void test_bootcfg_isolinux(void **state)
{
    (void)state;
    assert_int_equal(run(isolinux, N(isolinux), BOOTCFG_CD, 1), BOOTCFG_OK);
    assert_string_equal(g_res.source, "/isolinux/isolinux.cfg");
    assert_int_equal(picked()->format, BOOTCFG_FMT_ISOLINUX);
    assert_string_equal(picked()->kernel, "/live/vmlinuz");
    assert_int_equal(picked()->n_initrd, 2);
    /* A relative path is against the config's directory. */
    assert_string_equal(picked()->initrd[1], "/isolinux/initrd.gz");
    rendered(NULL, "boot=live components");
    /* The last label's INITRD and relative KERNEL. */
    assert_string_equal(g_res.entry[3].kernel, "/isolinux/vmlinuz2");
    assert_string_equal(g_res.entry[3].initrd[0], "/isolinux/initrd2.gz");
}

/* ---------------------------------------------------- Alpine, quoting */

static const ff alpine[] = {
    { "/boot/grub/grub.cfg",
      "set timeout=1\n"
      "menuentry \"Linux lts\" {\n"
      "linux\t/boot/vmlinuz-lts modules=loop,squashfs quiet \"a b\" 'c$d' e\\ f \\\n"
      "  cont\n"
      "initrd\t/boot/intel-ucode.img /boot/amd-ucode.img /boot/initramfs-lts\n"
      "}\n" },
    { "/boot/vmlinuz-lts", "" },
    { "/boot/intel-ucode.img", "" },
    { "/boot/amd-ucode.img", "" },
    { "/boot/initramfs-lts", "" },
};

static void test_bootcfg_quoting_and_initrds(void **state)
{
    (void)state;
    assert_int_equal(run(alpine, N(alpine), BOOTCFG_CD, 1), BOOTCFG_OK);
    assert_int_equal(picked()->n_initrd, 3);
    /* Double quotes group, single quotes stop expansion, a backslash
     * escapes a space and continues a line; a value with a space in it is
     * quoted for the kernel again. */
    assert_string_equal(picked()->cmdline,
                        "modules=loop,squashfs quiet \"a b\" c$d \"e f\" cont");
}

/* ------------------------------------------------------------- bounds */

static void test_bootcfg_refusals(void **state)
{
    static const ff none[] = { { "/readme.txt", "hello" } };
    static const ff loop_include[] = {
        { "/boot/grub/grub.cfg", "source /boot/grub/grub.cfg\n" },
    };
    static const ff unclosed[] = {
        { "/boot/grub/grub.cfg",
          "menuentry 'open' {\n  linux /k a\n  initrd /i\n" },
        { "/k", "" }, { "/i", "" },
    };
    ftree t = { none, 1, 0 };
    bootcfg_src s = { fr_read, NULL, NULL, &t };
    bootcfg_src bad = { NULL, NULL, NULL, &t };

    (void)state;
    assert_int_equal(run(none, N(none), BOOTCFG_CD, 1), BOOTCFG_ERR_NONE);
    assert_int_equal(g_res.pick, BOOTCFG_ENTRIES_MAX);
    assert_int_equal(bootcfg_read(NULL, BOOTCFG_CD, g_scratch, 16, &g_res),
                     BOOTCFG_ERR_BAD_ARG);
    assert_int_equal(bootcfg_read(&bad, BOOTCFG_CD, g_scratch, 16, &g_res),
                     BOOTCFG_ERR_BAD_ARG);
    assert_int_equal(bootcfg_read(&s, BOOTCFG_CD, NULL, 16, &g_res),
                     BOOTCFG_ERR_BAD_ARG);
    assert_int_equal(bootcfg_read(&s, BOOTCFG_CD, g_scratch, 1, &g_res),
                     BOOTCFG_ERR_BAD_ARG);
    assert_int_equal(bootcfg_read(&s, BOOTCFG_CD, g_scratch, 16, NULL),
                     BOOTCFG_ERR_BAD_ARG);

    /* A config that sources itself stops at the depth bound. */
    assert_int_equal(run(loop_include, N(loop_include), BOOTCFG_CD, 1),
                     BOOTCFG_ERR_NONE);
    /* An unterminated block ends at end of file, entry and all. */
    assert_int_equal(run(unclosed, N(unclosed), BOOTCFG_CD, 1), BOOTCFG_OK);
    rendered(NULL, "a");

    /* A config too big for the scratch. */
    {
        ftree t2 = { debian, N(debian), 0 };
        bootcfg_src s2 = { fr_read, NULL, NULL, &t2 };

        assert_int_equal(bootcfg_read(&s2, BOOTCFG_CD, g_scratch, 64, &g_res),
                         BOOTCFG_ERR_NOSPACE);
    }
}

static void test_bootcfg_limits(void **state)
{
    static char big[64 * 1024];
    static char longline[8 * 1024];
    ff files[3];
    size_t i, n = 0;

    (void)state;
    /* More entries than the result holds: the rest are dropped, the first
     * is still picked. */
    for (i = 0; i < BOOTCFG_ENTRIES_MAX + 5u; i++)
        n += (size_t)snprintf(big + n, sizeof big - n,
                              "menuentry 'e%zu' { linux /k x%zu }\n", i, i);
    files[0].path = "/boot/grub/grub.cfg";
    files[0].content = big;
    files[1].path = "/k";
    files[1].content = "";
    assert_int_equal(run(files, 2, BOOTCFG_CD, 1), BOOTCFG_OK);
    assert_int_equal(g_res.n, BOOTCFG_ENTRIES_MAX);
    rendered(NULL, "x0");

    /* A cmdline longer than an entry holds is truncated and says so. */
    n = (size_t)snprintf(longline, sizeof longline, "menuentry 'long' { linux /k");
    for (i = 0; i < 400; i++)
        n += (size_t)snprintf(longline + n, sizeof longline - n, " arg%zu", i);
    (void)snprintf(longline + n, sizeof longline - n, " }\n");
    files[0].content = longline;
    assert_int_equal(run(files, 2, BOOTCFG_CD, 1), BOOTCFG_OK);
    assert_true(picked()->truncated);
    assert_true(strlen(picked()->cmdline) > 500u);
}

/* ------------------------------------------------------------- render */

static void test_bootcfg_render(void **state)
{
    bootcfg_entry e;
    char out[64], tiny[4];

    (void)state;
    memset(&e, 0, sizeof e);
    strcpy(e.cmdline, "a  img_loop=\"" BOOTCFG_ISO_TOKEN "\" " BOOTCFG_ISO_TOKEN
                      " x=pre" BOOTCFG_ISO_TOKEN "  b");
    assert_int_equal(bootcfg_render(&e, "/i.iso", out, sizeof out), BOOTCFG_OK);
    assert_string_equal(out, "a img_loop=\"/i.iso\" /i.iso x=pre/i.iso b");
    /* Without a path: an argument that was only the token goes, one with
     * something else in it keeps that. */
    assert_int_equal(bootcfg_render(&e, NULL, out, sizeof out), BOOTCFG_OK);
    assert_string_equal(out, "a x=pre b");
    assert_int_equal(bootcfg_render(&e, "/i.iso", tiny, sizeof tiny),
                     BOOTCFG_ERR_NOSPACE);
    strcpy(e.cmdline, "abcdef");
    assert_int_equal(bootcfg_render(&e, NULL, tiny, sizeof tiny),
                     BOOTCFG_ERR_NOSPACE);
    assert_int_equal(bootcfg_render(NULL, NULL, out, sizeof out),
                     BOOTCFG_ERR_BAD_ARG);
    assert_int_equal(bootcfg_render(&e, NULL, NULL, 1), BOOTCFG_ERR_BAD_ARG);
    assert_false(bootcfg_takes_iso_path(NULL));
    assert_false(bootcfg_takes_iso_path(&e));
}

int run_bootcfg_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_bootcfg_debian),
        cmocka_unit_test(test_bootcfg_fedora),
        cmocka_unit_test(test_bootcfg_ubuntu),
        cmocka_unit_test(test_bootcfg_arch),
        cmocka_unit_test(test_bootcfg_sysrescue),
        cmocka_unit_test(test_bootcfg_opensuse),
        cmocka_unit_test(test_bootcfg_manjaro),
        cmocka_unit_test(test_bootcfg_isolinux),
        cmocka_unit_test(test_bootcfg_quoting_and_initrds),
        cmocka_unit_test(test_bootcfg_refusals),
        cmocka_unit_test(test_bootcfg_limits),
        cmocka_unit_test(test_bootcfg_render),
    };

    return cmocka_run_group_tests_name("bootcfg", tests, NULL, NULL);
}
