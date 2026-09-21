/* falcon-bios: what an ISO on p2 holds (docs/notes/bios-route.md Section 4,
 * and DESIGN Section 6.4 for the same job under UEFI).
 *
 * This is the point of the whole route, in one file. Everything here is
 * falcon-core, unmodified, over a chain of read callbacks whose far end is
 * INT 13h:
 *
 *   bios_disk_read      the BIOS, one interrupt per transfer
 *     -> part_read      the partition, offset by its first LBA
 *       -> fs_mount     p2, identified by the registry rather than named
 *         -> fs_file_read_cb   the .iso file as a stream of bytes
 *           -> medium_open     UDF, ISO9660 and the El Torito boot image
 *             -> bootfs/bootcfg  the ISO's own GRUB, systemd-boot, isolinux
 *
 * The UEFI loader runs the same stack from `medium_open` down, over a
 * callback that reads a file the firmware opened. Nothing below that line
 * can tell the two apart, which is what Section 1 claimed and what stage 3
 * exists to make true here: the registry and the ISO drivers do not fit in
 * stage 2's 33 KB and do not have to.
 */

#include <bios.h>
#include <scan.h>
#include <linux.h>
#include <vcd.h>
#include <bsd.h>
#include <win.h>

#include <fs/fs.h>
#include <media/medium.h>
#include <media/bootfs.h>
#include <media/bootcfg.h>
#include <media/family.h>
#include <media/extmap.h>
#include <media/hookcmd.h>
#include <manifest/manifest.h>
#include <part/part.h>
#include <lib/string.h>

/* The hook, as make embeds it (falcon-hook/build.mk). The same binary the
 * UEFI loader appends, for the same reason: it runs in the initramfs and has
 * no opinion about which firmware started the kernel. */
extern const unsigned char falcon_hook_bin[];
extern const size_t falcon_hook_bin_len;

/* All of it .bss, which stage 2 zeroes and the image does not carry: this is
 * about 600 KB of iterators and scratch and there is one survey at a time.
 * A stack would not hold it either; stage 3's is 64 KB by the map in
 * biosdefs.h. */
static fs_file       g_iso;
static medium        g_medium;
static bootfs_ctx    g_bctx;
static bootcfg_src   g_src;
static bootcfg_result g_cfg;

/* What bootcfg parses config files in. The sign tool's figure for the same
 * job (falcon-cli/src/sign/main.c), since both read the same files. */
static char g_scratch[512u * 1024u];

/* The files a boot opens: the kernel and every initrd at once, because
 * linux_boot reads them in the order it places them and reading one would
 * otherwise close the next. */
/* What the loader adds to every cmdline it boots.
 *
 * This build is serial-first and says so from its first instruction: stage 1
 * brings COM1 up before it reads anything, because a machine with no usable
 * video is the machine a BIOS route exists to reach, and every menu line
 * goes to both screen and serial. A kernel that printed only to a screen
 * that is not there would break that in the one place it matters most.
 *
 * tty0 is last on purpose. The kernel sends messages to every console= it is
 * given and makes the last one /dev/console, so this puts the log on serial
 * and leaves the interactive console on the screen. */
#define CONSOLE_ARGS " console=ttyS0,115200 console=tty0"

/* The disk p2 is on, which the BSD route needs to install its handler and
 * nothing else here does. Set by scan_iso's caller through scan_disk. */
static bios_disk   *g_disk;

static fs_file      g_kernel;
static fs_file      g_initrd[BOOTCFG_INITRD_MAX];
/* One more than the entry's initrds: the last is the hook's archive. */
static linux_object g_obj[BOOTCFG_INITRD_MAX + 1u];
static char         g_cmd[BOOTCFG_CMDLINE_MAX];
static char         g_hooked[BOOTCFG_CMDLINE_MAX + 128u];

/* The extent map, and the cpio archive the hook and the map travel in. */
static fs_extent     g_ext[EXTMAP_MAX_EXTENTS];
static extmap_extent g_mext[EXTMAP_MAX_EXTENTS];
static uint8_t       g_map[EXTMAP_HDR_LEN + EXTMAP_MAX_EXTENTS * EXTMAP_EXT_LEN];
static uint8_t       g_archive[256u * 1024u];

/* A linux_object over memory, for the archive, which is built rather than
 * read. */
typedef struct {
    const uint8_t *at;
    uint64_t       len;
} mem_src;

static mem_src g_archive_src;

static int mem_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const mem_src *m = ctx;

    if (off > m->len || len > m->len - off)
        return -1;
    memcpy(buf, m->at + off, (size_t)len);
    return 0;
}

static void put(const char *s)         { bios_screen_puts(s); }

static void put_dec(uint64_t v)
{
    char buf[21];
    char one[2];
    size_t n = 0;

    one[1] = '\0';
    if (v == 0) {
        put("0");
        return;
    }
    while (v > 0 && n < sizeof buf) {
        buf[n++] = (char)('0' + (unsigned)(v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        one[0] = buf[--n];
        put(one);
    }
}

/* Appends to a NUL-terminated buffer. 0 on success, nonzero when it would
 * not fit; nothing is written in that case, because half a kernel argument
 * is worse than none. */
static int append(char *buf, size_t cap, const char *add)
{
    size_t n = 0;
    size_t k = 0;

    while (buf[n] != '\0')
        n++;
    while (add[k] != '\0')
        k++;
    if (n + k + 1u > cap)
        return -1;
    memcpy(buf + n, add, k + 1u);
    return 0;
}

static const char *kind_name(medium_vol_kind k)
{
    switch (k) {
    case MEDIUM_VOL_ISO9660: return "ISO9660";
    case MEDIUM_VOL_UDF:     return "UDF";
    case MEDIUM_VOL_BOOT:    return "El Torito boot image";
    case MEDIUM_VOL_PART:    return "partition";
    default:                 return "whole image";
    }
}

/* Where the ISO sits on p2, encoded for the hook (DESIGN Section 6.4).
 *
 * The UEFI loader reads the partition's GUID and first LBA out of the Hard
 * Drive node of its device path; here they come straight off the GPT entry
 * falcon-core already parsed, which is the same two numbers by a shorter
 * road. Returns the encoded length, or 0. */
static size_t build_map(fs_vol *p2, const gpt_part *part, const fs_dirent *iso,
                        uint32_t block_size)
{
    extmap_hdr hdr;
    size_t n = 0;
    size_t i;
    size_t mlen = 0;

    memset(&hdr, 0, sizeof hdr);
    memcpy(hdr.part_guid, part->unique, sizeof hdr.part_guid);
    hdr.part_lba = part->start_lba;
    hdr.block_size = block_size;
    hdr.iso_size = iso->size;

    if (fs_extents(p2, iso, g_ext, EXTMAP_MAX_EXTENTS, &n) != FS_OK) {
        put("  the ISO is in more pieces on p2 than the hook can be told"
            " about\n");
        return 0;
    }
    for (i = 0; i < n; i++) {
        g_mext[i].offset = g_ext[i].vol_off;
        g_mext[i].length = g_ext[i].len;
    }
    hdr.n_extents = (uint32_t)n;
    if (extmap_encode(&hdr, g_mext, g_map, sizeof g_map, &mlen) != EXTMAP_OK)
        return 0;
    return mlen;
}

/* Open what the entry names and hand it to the boot protocol.
 *
 * The cmdline is rendered without an ISO path: finding the medium is
 * falcon-hook's job (DESIGN Section 12.4), and it is told where the ISO is
 * by the extent map in its own cpio, appended after the entry's initrds.
 * A map that cannot be built (too many fragments, no GPT entry) boots the
 * entry without the hook rather than not at all, and says so; the kernel
 * then finds its medium by its own means or not. */
static int boot_entry(const bootcfg_entry *e, fs_vol *p2,
                      const gpt_part *p2part, const fs_dirent *iso,
                      uint32_t block_size)
{
    linux_object kernel;
    char *cmd = g_cmd;
    uint64_t size;
    size_t map_len = 0;
    size_t wrote = 0;
    unsigned i;

    if (e->kernel_unresolved) {
        put("  the entry names its kernel through a value only GRUB could"
            " compute\n");
        return -1;
    }
    if (bootfs_open(&g_bctx, e->kernel, &g_kernel, &size) != FS_OK) {
        put("  the kernel is not on the medium: ");
        put(e->kernel);
        put("\n");
        return -1;
    }
    kernel.read = fs_file_read_cb;
    kernel.ctx = &g_kernel;
    kernel.size = size;

    for (i = 0; i < e->n_initrd && i < BOOTCFG_INITRD_MAX; i++) {
        if (bootfs_open(&g_bctx, e->initrd[i], &g_initrd[i], &size) != FS_OK) {
            put("  an initrd is not on the medium: ");
            put(e->initrd[i]);
            put("\n");
            return -1;
        }
        g_obj[i].read = fs_file_read_cb;
        g_obj[i].ctx = &g_initrd[i];
        g_obj[i].size = size;
    }

    if (bootcfg_render(e, (const char *)0, g_cmd, sizeof g_cmd) != BOOTCFG_OK) {
        put("  the cmdline does not fit\n");
        return -1;
    }

    /* The hook, and the map that tells it where the ISO is. Without them the
     * kernel starts and its initramfs has no medium to find, which is what
     * this route does when p2's own GPT entry is not to hand. */
    if (p2part != (const gpt_part *)0)
        map_len = build_map(p2, p2part, iso, block_size);

    if (map_len > 0 &&
        hookcmd_archive(falcon_hook_bin, falcon_hook_bin_len, g_map, map_len,
                        g_archive, sizeof g_archive, &wrote) == HOOKCMD_OK &&
        hookcmd_build(g_cmd, g_hooked, sizeof g_hooked) == HOOKCMD_OK) {
        g_archive_src.at = g_archive;
        g_archive_src.len = wrote;
        g_obj[i].read = mem_read;
        g_obj[i].ctx = &g_archive_src;
        g_obj[i].size = wrote;
        i++;
        cmd = g_hooked;
        put("  falcon-hook appended, ");
        put_dec(wrote);
        put(" bytes\n");
    } else {
        put("  no hook: this kernel will have to find its own medium\n");
    }

    if (append(cmd, cmd == g_cmd ? sizeof g_cmd : sizeof g_hooked,
               CONSOLE_ARGS) != 0) {
        put("  the cmdline does not fit\n");
        return -1;
    }

    put("  starting the kernel\n");
    return linux_boot(&kernel, g_obj, i, cmd, (void (*)(void))0);
}

int scan_iso(fs_vol *p2, const gpt_part *p2part, const fs_dirent *ent,
             const uint16_t *path, uint32_t block_size)
{
    unsigned i;
    unsigned n;
    fs_vol *cfgvol;
    family_probe probe;
    const family_desc *fam;
    int rc;

    if (fs_file_open(p2, &g_iso, ent) != FS_OK) {
        put("  the image would not open\n");
        return -1;
    }

    if (medium_open(&g_medium, fs_file_read_cb, &g_iso, ent->size) != 0) {
        put("  nothing on the medium mounted\n");
        return -1;
    }

    n = medium_count(&g_medium);
    put("  ");
    put_dec(n);
    put(" volume(s):");
    for (i = 0; i < n; i++) {
        put(i == 0 ? " " : ", ");
        put(kind_name(medium_kind_at(&g_medium, i)));
    }
    put("\n");

    /* The medium's own filesystem, which is what family_detect wants: the
     * boot image is a FAT volume holding a loader, and fingerprinting that
     * would name the loader rather than the distribution. */
    cfgvol = medium_count(&g_medium) > 0 ? medium_vol_at(&g_medium, 0) : (fs_vol *)0;
    if (cfgvol == (fs_vol *)0) {
        put("  no filesystem to read a configuration from\n");
        return -1;
    }

    bootfs_probe(&g_bctx, cfgvol, &probe);
    fam = family_detect(&probe);
    put("  family: ");
    put(fam != (const family_desc *)0 ? fam->name : "unrecognised");
    put("\n");

    /* Two families boot by routes that have nothing to do with a Linux boot
     * entry, so they are dispatched on the fingerprint rather than on the
     * configuration, exactly as the UEFI loader does. Each reports that the
     * medium is not one it applies to, which is a cue to carry on here
     * rather than a failure. */
    if (fam != (const family_desc *)0 && p2part != (const gpt_part *)0) {
        if (fam->id == FAMILY_BSD) {
            int not_bsd = 1;

            bios_screen_mode("BSD");
            (void)bsd_boot(g_disk, p2, p2part, ent, &not_bsd);
            if (!not_bsd)
                return -1;
        } else if (fam->id == FAMILY_WINDOWS) {
            int unpatchable = 1;
            char rel[MANIFEST_PATH_MAX];

            /* The whole path from p2's root, not the file's name: winjump
             * looks for exactly this under every drive letter, and an ISO
             * in a folder is not at the root of any of them. Converted by
             * the rule the UEFI loader and the manifest use, so a path it
             * cannot carry is refused here rather than turned into one
             * winjump will never find. */
            if (manifest_path_from_ucs2(path, rel, sizeof rel) !=
                MANIFEST_OK) {
                put("  the ISO's path is not plain ASCII, so Windows could"
                    " not find it again;\n  booting it unpatched\n");
                return scan_chain(g_disk, p2, p2part, ent);
            }
            bios_screen_mode("WIN");
            (void)win_boot(g_disk, p2, p2part, ent, rel, &unpatchable);
            if (!unpatchable)
                return -1;
            /* Windows media with nothing to patch still boots: its own
             * bootloader off the served medium. */
            return scan_chain(g_disk, p2, p2part, ent);
        }
    }

    bootfs_src(&g_bctx, cfgvol, &g_src);
    rc = bootcfg_read(&g_src, BOOTCFG_CD, g_scratch, sizeof g_scratch, &g_cfg);
    if (rc != BOOTCFG_OK || g_cfg.pick >= BOOTCFG_ENTRIES_MAX) {
        put("  no boot entry this loader would take\n");
        return -1;
    }

    put("  config: ");
    put(g_cfg.source);
    put("\n  entry:  ");
    put(g_cfg.entry[g_cfg.pick].title);
    put("\n  kernel: ");
    put(g_cfg.entry[g_cfg.pick].kernel);
    put("\n");
    for (i = 0; i < g_cfg.entry[g_cfg.pick].n_initrd; i++) {
        put("  initrd: ");
        put(g_cfg.entry[g_cfg.pick].initrd[i]);
        put("\n");
    }
    put("  cmdline: ");
    put(g_cfg.entry[g_cfg.pick].cmdline);
    put("\n");

    bios_screen_mode("DIR");
    return boot_entry(&g_cfg.entry[g_cfg.pick], p2, p2part, ent, block_size);
}

int scan_chain(bios_disk *d, fs_vol *p2, const gpt_part *p2part,
               const fs_dirent *ent)
{
    if (p2part == (const gpt_part *)0) {
        put("  p2 has no GPT entry, so the medium cannot be served\n");
        return -1;
    }
    if (fs_file_open(p2, &g_iso, ent) != FS_OK) {
        put("  the image would not open\n");
        return -1;
    }
    bios_screen_mode("CHN");
    return vcd_chainload(d, p2, p2part, ent, &g_iso);
}

void scan_disk(bios_disk *d)
{
    g_disk = d;
}
