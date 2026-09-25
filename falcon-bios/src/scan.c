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
#include <screen.h>

#include <fs/fs.h>
#include <media/medium.h>
#include <media/bootfs.h>
#include <media/bootcfg.h>
#include <media/family.h>
#include <media/extmap.h>
#include <media/hookcmd.h>
#include <part/part.h>
#include <lib/string.h>

/* The hook, as make embeds it (falcon-hook/build.mk), in both widths. The
 * x86-64 one is the binary the UEFI loader appends, for the same reason: it
 * runs in the initramfs and has no opinion about which firmware started the
 * kernel. It does have to be the kernel's width, and this loader boots
 * 32-bit kernels too (bios-route.md Section 6h), so the i686 build rides
 * along and the kernel's own header decides which one goes. */
extern const unsigned char falcon_hook_bin[];
extern const size_t falcon_hook_bin_len;
extern const unsigned char falcon_hook32_bin[];
extern const size_t falcon_hook32_bin_len;

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

/* Serial only: what the screen does not say, for a log. */
static void log_str(const char *a, const char *b)
{
    bios_puts("falcon-bios:   ");
    bios_puts(a);
    bios_puts(b);
    bios_puts("\n");
}

/* The title the handoff names, copied out of the entry being booted. */
static const char *g_title;

/* The line the UEFI loader prints as it starts a kernel, printed here at the
 * same moment: every read done, nothing of this loader left to run. */
static void dir_handoff(void)
{
    screen_out("starting ");
    screen_out(g_title);
    screen_out("\n");
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

    if (fs_extents(p2, iso, g_ext, EXTMAP_MAX_EXTENTS, &n) != FS_OK)
        return 0;
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
                      uint32_t block_size, boot_result *r)
{
    linux_object kernel;
    char *cmd = g_cmd;
    uint64_t size;
    size_t map_len = 0;
    size_t wrote = 0;
    unsigned i;
    int is_64 = 1;

    r->stage = "reading the kernel";
    if (e->kernel_unresolved) {
        r->advice = "the entry names its kernel through a value only GRUB "
                    "could compute.";
        r->fallthrough = 1;
        return -1;
    }
    if (bootfs_open(&g_bctx, e->kernel, &g_kernel, &size) != FS_OK) {
        boot_result_advise(r, "the entry's kernel is not on the medium: ",
                           e->kernel);
        r->fallthrough = 1;
        return -1;
    }
    kernel.read = fs_file_read_cb;
    kernel.ctx = &g_kernel;
    kernel.size = size;
    /* A header that will not read leaves the 64-bit hook in place, and
     * linux_boot refuses the kernel with the reason a moment later. */
    (void)linux_probe(&kernel, &is_64);
    log_str("kernel width: ", is_64 ? "64-bit" : "32-bit");

    r->stage = "reading the initrd";
    for (i = 0; i < e->n_initrd && i < BOOTCFG_INITRD_MAX; i++) {
        if (bootfs_open(&g_bctx, e->initrd[i], &g_initrd[i], &size) != FS_OK) {
            boot_result_advise(r, "the entry's initrd is not on the medium: ",
                               e->initrd[i]);
            return -1;
        }
        g_obj[i].read = fs_file_read_cb;
        g_obj[i].ctx = &g_initrd[i];
        g_obj[i].size = size;
    }

    r->stage = "building the cmdline";
    if (bootcfg_render(e, (const char *)0, g_cmd, sizeof g_cmd) != BOOTCFG_OK)
        return -1;

    /* The hook, and the map that tells it where the ISO is. Without them the
     * kernel starts and its initramfs has no medium to find, which is what
     * this route does when p2's own GPT entry is not to hand. */
    if (p2part != (const gpt_part *)0)
        map_len = build_map(p2, p2part, iso, block_size);

    if (map_len > 0 &&
        hookcmd_archive(is_64 ? falcon_hook_bin : falcon_hook32_bin,
                        is_64 ? falcon_hook_bin_len : falcon_hook32_bin_len,
                        g_map, map_len, g_archive, sizeof g_archive,
                        &wrote) == HOOKCMD_OK &&
        hookcmd_build(g_cmd, g_hooked, sizeof g_hooked) == HOOKCMD_OK) {
        g_archive_src.at = g_archive;
        g_archive_src.len = wrote;
        g_obj[i].read = mem_read;
        g_obj[i].ctx = &g_archive_src;
        g_obj[i].size = wrote;
        i++;
        cmd = g_hooked;
        bios_puts("falcon-bios:   falcon-hook appended, ");
        bios_put_dec(wrote);
        bios_puts(" bytes\n");
    } else {
        screen_out("warning: falcon-hook cannot be told where this ISO is on "
                   "the stick,\n         so the kernel has to find its "
                   "medium by itself.\n\n");
    }

    if (append(cmd, cmd == g_cmd ? sizeof g_cmd : sizeof g_hooked,
               CONSOLE_ARGS) != 0)
        return -1;
    log_str("cmdline: ", cmd);

    r->stage = "starting the kernel";
    g_title = e->title;
    (void)linux_boot(&kernel, g_obj, i, cmd, dir_handoff);
    boot_result_advise(r, linux_why(), (const char *)0);
    r->fallthrough = linux_not_taken();
    return -1;
}

/* The medium's volumes and its own filesystem, which is what the family
 * fingerprint and the boot configuration are read from: the boot image is a
 * FAT volume holding a loader, and fingerprinting that would name the loader
 * rather than the distribution. NULL when there is none. */
static fs_vol *open_medium(fs_vol *p2, const fs_dirent *ent, boot_result *r)
{
    unsigned i;
    unsigned n;

    r->stage = "opening the ISO";
    if (fs_file_open(p2, &g_iso, ent) != FS_OK)
        return (fs_vol *)0;

    r->stage = "reading the ISO's filesystem";
    if (medium_open(&g_medium, fs_file_read_cb, &g_iso, ent->size) != 0)
        return (fs_vol *)0;

    n = medium_count(&g_medium);
    bios_puts("falcon-bios:   ");
    bios_put_dec(n);
    bios_puts(" volume(s):");
    for (i = 0; i < n; i++) {
        bios_puts(i == 0 ? " " : ", ");
        bios_puts(kind_name(medium_kind_at(&g_medium, i)));
    }
    bios_puts("\n");
    return n > 0 ? medium_vol_at(&g_medium, 0) : (fs_vol *)0;
}

const family_desc *scan_family(fs_vol *p2, const fs_dirent *ent)
{
    boot_result r;
    family_probe probe;
    const family_desc *fam;
    fs_vol *cfgvol;

    boot_result_init(&r);
    cfgvol = open_medium(p2, ent, &r);
    if (cfgvol == (fs_vol *)0)
        return (const family_desc *)0;
    bootfs_probe(&g_bctx, cfgvol, &probe);
    fam = family_detect(&probe);
    log_str("family: ", fam != (const family_desc *)0 ? fam->name
                                                       : "unrecognised");
    return fam;
}

int scan_direct(fs_vol *p2, const gpt_part *p2part, const fs_dirent *ent,
                uint32_t block_size, boot_result *r)
{
    const bootcfg_entry *e;
    fs_vol *cfgvol;
    size_t i;

    cfgvol = open_medium(p2, ent, r);
    if (cfgvol == (fs_vol *)0) {
        r->fallthrough = 1;
        return -1;
    }

    r->stage = "reading the ISO's boot configuration";
    bootfs_src(&g_bctx, cfgvol, &g_src);
    if (bootcfg_read(&g_src, BOOTCFG_CD, g_scratch, sizeof g_scratch,
                     &g_cfg) != BOOTCFG_OK ||
        g_cfg.pick >= BOOTCFG_ENTRIES_MAX) {
        r->advice = "the ISO has no boot entry this loader can take.";
        r->fallthrough = 1;
        return -1;
    }
    e = &g_cfg.entry[g_cfg.pick];
    for (i = 0; i + 1u < sizeof r->title && e->title[i] != '\0'; i++)
        r->title[i] = e->title[i];
    r->title[i] = '\0';

    log_str("config: ", g_cfg.source);
    log_str("entry:  ", e->title);
    log_str("kernel: ", e->kernel);
    for (i = 0; i < e->n_initrd; i++)
        log_str("initrd: ", e->initrd[i]);

    return boot_entry(e, p2, p2part, ent, block_size, r);
}

int scan_chain(bios_disk *d, fs_vol *p2, const gpt_part *p2part,
               const fs_dirent *ent, boot_result *r)
{
    r->stage = "finding p2's GPT entry";
    if (p2part == (const gpt_part *)0) {
        r->advice = "the ISO's partition has no GPT entry this loader can "
                    "serve it from.";
        return -1;
    }
    r->stage = "opening the ISO";
    if (fs_file_open(p2, &g_iso, ent) != FS_OK)
        return -1;
    return vcd_chainload(d, p2, p2part, ent, &g_iso, r);
}
