/* ISO family detection and registry (docs/DESIGN.md Section 6.2).
 *
 * Fingerprints are the base directory each family's initramfs is built
 * around, because that is the one thing a family cannot rename without also
 * changing its own boot path:
 *
 *   Ubuntu/casper     /casper          vmlinuz + *.squashfs, boot=casper
 *   Debian live       /live            live-boot, filesystem.squashfs
 *   Fedora/dracut     /LiveOS          squashfs.img for dmsquash-live, or
 *                     /images/pxeboot  vmlinuz+initrd on installer media
 *   archiso           /arch            archisobasedir=arch
 *   SystemRescue      /sysresccd       archiso with its own base directory
 *   memtest           /boot/memtest    no Linux userland at all
 *   Windows           /sources         install.wim or install.esd
 *   Gentoo            /boot/gentoo.igz genkernel's names, kept when the live
 *                                      media moved to dracut
 *
 * Where a layout check is ambiguous there is a content check behind it: the
 * systemd-boot entries archiso ships name their base directory, which is what
 * separates a stock archiso from a derivative that renamed the directory.
 */

#include <media/family.h>
#include <lib/string.h>

/* Read window for the content fingerprints. Loader entries are a few hundred
 * bytes; anything that needs more than this is not a fingerprint. */
#define PROBE_HEAD 1024u

static const family_desc g_families[] = {
    /* FAMILY_UNKNOWN carries no strategy at all: needs_injection is 0 because
     * the field means "this family needs injection", and for an unclassified
     * medium we claim nothing. Callers must branch on the id, not on the
     * flags, before acting on an unknown. */
    { FAMILY_UNKNOWN, "unknown", (void *)0, 0,
      FAMILY_TRI_UNKNOWN, FAMILY_TRI_UNKNOWN },

    { FAMILY_DEBIAN_CASPER, "Ubuntu/casper",
      "iso-scan/filename=%ISO%", 0,
      FAMILY_TRI_UNKNOWN, FAMILY_TRI_UNKNOWN },

    /* live-boot's own option; casper's iso-scan is a different implementation
     * with a different name, so the two cannot share a row. */
    { FAMILY_DEBIAN_LIVE, "Debian live",
      "findiso=%ISO%", 0,
      FAMILY_TRI_UNKNOWN, FAMILY_TRI_UNKNOWN },

    /* dracut's 90dmsquash-live module stacks the live overlay on a
     * device-mapper snapshot, so device-mapper is present in every dracut
     * live initramfs by construction. exFAT is a kernel module and nothing
     * guarantees it is included; unknown until measured. */
    { FAMILY_FEDORA_DRACUT, "Fedora/dracut",
      "iso-scan/filename=%ISO%", 0,
      FAMILY_TRI_UNKNOWN, FAMILY_TRI_YES },

    /* exfat/dm both YES: measured on the archiso 2026.01.01 initramfs,
     * which ships exfat.ko.zst, loop.ko.zst and dm-mod.ko.zst (bless-tool
     * verification, 2026-09-13). */
    { FAMILY_ARCH, "Arch/archiso",
      "img_dev=%DEV% img_loop=%ISO%", 0,
      FAMILY_TRI_YES, FAMILY_TRI_YES },

    /* archiso derivative with its own base directory; without the
     * archisobasedir override the stock hook looks in /arch and finds
     * nothing (bless-tool verification, 2026-09-13). */
    { FAMILY_SYSTEMRESCUE, "SystemRescue",
      "img_dev=%DEV% img_loop=%ISO% archisobasedir=sysresccd", 0,
      FAMILY_TRI_UNKNOWN, FAMILY_TRI_UNKNOWN },

    /* memtest is a bare EFI binary: it needs no cmdline, no initramfs and no
     * backing media once loaded, which is what the three columns below say.
     * They do NOT say it needs no blessing. What the platform verifies for a
     * bare payload is the payload itself, so Secure Boot wants a signature on
     * it however self-contained it is (DESIGN Section 7.2, decided 2026-09-13
     * after this family proved unbootable on a db with no Microsoft CA). That
     * is the bless tool's business and not a column here. */
    { FAMILY_MEMTEST, "memtest",
      (void *)0, 0,
      FAMILY_TRI_NO, FAMILY_TRI_NO },

    /* No cmdline and no injection. The injection column means one specific
     * mechanism, the extent-map/device-mapper hook of DESIGN Section 6.2, and
     * that is a Linux initramfs mechanism with nothing to answer here: WinPE
     * boots into a RAM disk and re-finds the medium from Windows userland, so
     * the post-ExitBootServices media problem the hook exists for does not
     * arise. Whatever Windows does need happens before the firmware ever runs
     * and is not this field. */
    { FAMILY_WINDOWS, "Windows install media",
      (void *)0, 0,
      FAMILY_TRI_NO, FAMILY_TRI_NO },

    /* Gentoo's live media boots dracut's dmsquash-live, which finds its root
     * by the ISO's volume label, and that label carries the release date
     * (Gentoo-amd64-20260510), hence %LABEL% rather than a literal. The rest
     * is the stock GRUB entry's cmdline, with iso-scan added to find the ISO
     * as a file. exfat/dm both YES: measured on the 20260510 initramfs, which
     * ships exfat.ko, loop.ko and dracut's iso-scan (2026-09-18). */
    { FAMILY_GENTOO, "Gentoo",
      "dokeymap nodhcp root=live:CDLABEL=%LABEL% rd.live.dir=/ "
      "rd.live.squashimg=image.squashfs cdroot iso-scan/filename=%ISO%", 0,
      FAMILY_TRI_YES, FAMILY_TRI_YES },
};

#define FAMILY_COUNT (sizeof(g_families) / sizeof(g_families[0]))

static size_t cstr_len(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

static int buf_contains(const uint8_t *buf, size_t n, const char *needle)
{
    size_t nl = cstr_len(needle);
    size_t i;

    if (nl == 0 || nl > n)
        return 0;
    for (i = 0; i + nl <= n; i++) {
        if (memcmp(buf + i, needle, nl) == 0)
            return 1;
    }
    return 0;
}

static int probe_exists(const family_probe *p, const char *path)
{
    if (p->exists == (void *)0)
        return 0;
    return p->exists(p->ctx, path) != 0;
}

/* Does the head of path contain needle? A file that cannot be read is a
 * fingerprint that does not match, never an error the caller must handle. */
static int probe_head_contains(const family_probe *p, const char *path,
                               const char *needle)
{
    uint8_t head[PROBE_HEAD];
    size_t got = 0;

    if (p->read == (void *)0)
        return 0;
    /* Zeroed before the call, not after: a callback that reports more bytes
     * than it actually wrote then leaves the scan running over deterministic
     * zeroes instead of whatever the stack happened to hold. */
    memset(head, 0, sizeof(head));
    if (p->read(p->ctx, path, 0, head, sizeof(head), &got) != 0)
        return 0;
    /* sizeof(head) is both the buffer and the length asked for, so this one
     * bound catches an overrun and a callback claiming more than it was
     * offered. Rejected rather than clamped: a callback that cannot describe
     * the size of its own result is not one to believe about the content. */
    if (got > sizeof(head))
        return 0;
    return buf_contains(head, got, needle);
}

/* archiso ships systemd-boot entries that spell out archisobasedir=<dir>.
 * The entry filenames have changed across archiso releases, so the candidate
 * list is the versions in circulation rather than one guess. */
static const char *const g_loader_entries[] = {
    "/loader/entries/01-archiso-linux.conf",
    "/loader/entries/01-archiso-x86_64-linux.conf",
    "/loader/entries/02-archiso-x86_64-speech-linux.conf",
    "/loader/entries/archiso-x86_64-linux.conf",
    "/loader/entries/archiso-x86_64.conf",
};

static int loader_entries_mention(const family_probe *p, const char *needle)
{
    size_t i;

    for (i = 0; i < sizeof(g_loader_entries) / sizeof(g_loader_entries[0]);
         i++) {
        if (probe_head_contains(p, g_loader_entries[i], needle))
            return 1;
    }
    return 0;
}

/* ASCII case-insensitive "does hay contain needle", needle already lower
 * case. The names this runs over are file names off exFAT, which is where the
 * case comes from: the same binary ships as MemTest86.efi, memtest86.efi and
 * MT86.EFI depending on who wrote the stick. */
static int name_contains(const char *hay, const char *needle)
{
    size_t nl = cstr_len(needle);
    size_t hl = cstr_len(hay);
    size_t i, j;

    if (nl == 0 || nl > hl)
        return 0;
    for (i = 0; i + nl <= hl; i++) {
        for (j = 0; j < nl; j++) {
            char c = hay[i + j];

            if (c >= 'A' && c <= 'Z')
                c = (char)(c + ('a' - 'A'));
            if (c != needle[j])
                break;
        }
        if (j == nl)
            return 1;
    }
    return 0;
}

/* The names a memtest payload ships under. Both families spell it this way:
 * memtest86+ builds memtest.efi/memtest86.efi, and PassMark's MemTest86 uses
 * mt86 as its short form throughout its own media (mt86.png sits beside the
 * loader inside the .img this was written for). */
static const char *const g_memtest_names[] = { "memtest", "mt86" };

const family_desc *family_detect_payload(const char *name)
{
    size_t i;

    /* A bare payload has no tree to fingerprint, so the file name is the only
     * evidence there is, and it is weak evidence: it is a label for the menu
     * and for a report line, and nothing downstream may act on it. Every
     * decision that matters about a payload - whether it may run - stays the
     * platform's, and every decision the bless tool takes about one comes from
     * the file's own PE headers rather than from here. */
    if (name == (const char *)0)
        return family_by_id(FAMILY_UNKNOWN);
    for (i = 0; i < sizeof(g_memtest_names) / sizeof(g_memtest_names[0]); i++) {
        if (name_contains(name, g_memtest_names[i]))
            return family_by_id(FAMILY_MEMTEST);
    }
    return family_by_id(FAMILY_UNKNOWN);
}

const family_desc *family_by_id(family_id id)
{
    size_t i;

    for (i = 0; i < FAMILY_COUNT; i++) {
        if (g_families[i].id == id)
            return &g_families[i];
    }
    return &g_families[0]; /* FAMILY_UNKNOWN */
}

const family_desc *family_table(size_t *count)
{
    if (count != (void *)0)
        *count = FAMILY_COUNT;
    return g_families;
}

const family_desc *family_detect(const family_probe *p)
{
    if (p == (void *)0)
        return family_by_id(FAMILY_UNKNOWN);

    /* Windows first: its layout shares nothing with the Linux families, so
     * there is no ordering subtlety to get wrong later. */
    if (probe_exists(p, "/sources/install.wim") ||
        probe_exists(p, "/sources/install.esd"))
        return family_by_id(FAMILY_WINDOWS);

    /* SystemRescue before Arch: it is archiso underneath, and its media can
     * carry archiso's loader entries while its base directory is its own. */
    if (probe_exists(p, "/sysresccd") ||
        probe_exists(p, "/sysresccd/version") ||
        loader_entries_mention(p, "archisobasedir=sysresccd"))
        return family_by_id(FAMILY_SYSTEMRESCUE);

    if (probe_exists(p, "/casper") ||
        probe_exists(p, "/casper/vmlinuz"))
        return family_by_id(FAMILY_DEBIAN_CASPER);

    /* Checked after casper because Ubuntu derivatives occasionally carry a
     * /live directory of leftovers while booting from /casper. */
    if (probe_exists(p, "/live/filesystem.squashfs") ||
        probe_exists(p, "/live"))
        return family_by_id(FAMILY_DEBIAN_LIVE);

    if (probe_exists(p, "/LiveOS/squashfs.img") ||
        probe_exists(p, "/LiveOS") ||
        probe_exists(p, "/images/pxeboot/vmlinuz"))
        return family_by_id(FAMILY_FEDORA_DRACUT);

    /* genkernel's kernel and initramfs names, which Gentoo's catalyst kept
     * when the live media moved to dracut. No other family uses them. */
    if (probe_exists(p, "/boot/gentoo.igz") ||
        (probe_exists(p, "/boot/gentoo") && probe_exists(p, "/image.squashfs")))
        return family_by_id(FAMILY_GENTOO);

    if (probe_exists(p, "/arch/boot/x86_64/vmlinuz-linux") ||
        probe_exists(p, "/arch/version") ||
        probe_exists(p, "/arch") ||
        loader_entries_mention(p, "archisobasedir="))
        return family_by_id(FAMILY_ARCH);

    /* memtest last: its fingerprints are the weakest, so anything with a real
     * base directory has already claimed the medium by now. */
    /* The first four are memtest86+ (the GPL one, whose binary is a kernel
     * image under /boot). The last two are what PassMark MemTest86's own USB
     * image actually carries beside its BOOTX64.efi, read off
     * memtest86-usb.img v10.x on 2026-09-13: mt86.png is its splash and
     * blacklist.cfg its DIMM exclusion list, and no other family ships
     * either. Without them that image classifies as unknown, because its ESP
     * is a bare /EFI/BOOT with the loader under the removable-media default
     * name rather than a memtest-shaped one. */
    if (probe_exists(p, "/boot/memtest") ||
        probe_exists(p, "/boot/memtest.bin") ||
        probe_exists(p, "/EFI/BOOT/memtest.efi") ||
        probe_exists(p, "/memtest.efi") ||
        probe_exists(p, "/EFI/BOOT/mt86.png") ||
        probe_exists(p, "/EFI/BOOT/blacklist.cfg"))
        return family_by_id(FAMILY_MEMTEST);

    return family_by_id(FAMILY_UNKNOWN);
}
