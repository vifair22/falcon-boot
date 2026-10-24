/* falcon-bios: BSD media served as its own memory disk
 * (docs/DESIGN.md Section 6.4).
 *
 * The third boot route, and the one that needs neither a Linux boot protocol
 * nor anything of ours running after the handover. FreeBSD's loader can
 * mount root from a memory disk it is told about in loader.conf, and it can
 * read that image off the medium it was started from. So the medium is
 * served with two bytes changed: a directory record naming the ISO as a file
 * inside itself, and the md_image settings written over loader.conf's
 * opening comment block. The loader then loads the whole medium into memory
 * and mounts it.
 *
 * Both edits are media/bsdmd.c's, unchanged, and both are laid over the
 * served medium rather than written to it - under UEFI through
 * iso/overlay.h, here through the resident handler's override table
 * (vcd.c). Nothing touches the stick.
 */

#include <start/bios.h>
#include <bsd/bsd.h>
#include <iso/vcd.h>
#include <mem/e820.h>
#include <linux/linux.h>

#include <fs/fs.h>
#include <media/bsdmd.h>
#include <media/biosboot.h>
#include <lib/string.h>

/* What loader.conf names as the console. A BIOS machine publishes none of
 * its own, so without this FreeBSD's installer draws to the screen alone
 * and a serial log - the only thing an automated run reads - stays empty.
 * Both, not comconsole alone: somebody watching the screen should still
 * see the installer. */
#define BSD_CONSOLE "comconsole,vidconsole"
/* The file whose opening comments the settings are written over. The ISO's
 * own /boot/loader.conf is read after this one and would win, but it is a
 * few hundred bytes with no comment block to spend, and nothing in these
 * media sets md_image_* there. */
#define LOADER_CONF "/boot/defaults/loader.conf"

/* Enough of loader.conf to find the end of its opening comment block; 442
 * bytes on FreeBSD, pfSense and OPNsense alike. */
#define CONF_HEAD 1024u

/* What the route needs free above the image's own size, in one run below the
 * ceiling. FreeBSD's loader stages the kernel and its modules in the same
 * region as the memory disk and grows it, with slop for the kernel's earliest
 * allocations; the figure is the UEFI route's, measured on the same media
 * (falcon-efi/src/bsd/bsd.c).
 *
 * The ceiling is the loader's own reach, and it differs by architecture. The
 * amd64 loader stages below 4 GB. The i386 loader runs under BTX, which maps
 * only the low ~1 GB, so an image the loader copies into RAM there runs off the
 * end of that window and halts BTX. A ~1 GB i386
 * memory disk cannot fit, so it is refused here with a message a reader can
 * act on rather than served and left to crash. */
#define STAGING_SLOP  (192ull * 1024ull * 1024ull)
#define STAGING_AMD64 0x100000000ull   /* 4 GB */
#define STAGING_I386  0x40000000ull    /* 1 GB, the i386 BTX window */

static fs_file g_iso;
static fs_vol  g_isovol;
static fs_dir  g_dir;
static fs_file g_conf;
static char    g_head[CONF_HEAD];
static char    g_cfg[BSDMD_CFG_MAX];
static char    g_path[BSDMD_PATH_MAX];

/* bsdmd_record reads the medium; this is the same file, already open. */
static int iso_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    return fs_file_read(&g_iso, off, len, buf) == FS_OK ? 0 : -1;
}

/* The ISO's kernel is i386, not amd64: read /boot/kernel/kernel's ELF machine.
 * EM_386 is 3, EM_X86_64 is 62, both little-endian at offset 18. The kernel,
 * not /boot/loader, because FreeBSD's loader is not a plain ELF file but the
 * kernel always is. An ISO whose kernel cannot be read is treated as amd64,
 * the permissive ceiling: the crash it would then meet is no worse than
 * today. */
static int kernel_head(uint8_t hdr[20])
{
    fs_dir dir;
    fs_dirent le;
    fs_file lf;

    return fs_lookup(&g_isovol, "/boot/kernel/kernel", &dir, &le) == FS_OK &&
           !le.is_dir && fs_file_open(&g_isovol, &lf, &le) == FS_OK &&
           fs_file_read(&lf, 0, 20, hdr) == FS_OK;
}

static int loader_is_i386(void)
{
    uint8_t hdr[20];

    if (!kernel_head(hdr))
        return 0;
    if (hdr[0] != 0x7Fu || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F')
        return 0;
    return hdr[18] == 3u && hdr[19] == 0u;
}

/* An amd64 kernel, by the rule the menu's {64-BIT ONLY} reads
 * (media/biosboot.h). */
static int kernel_is_amd64(void)
{
    uint8_t hdr[20];

    return kernel_head(hdr) && biosboot_elf_is_64(hdr, sizeof hdr) ==
                                   BIOSBOOT_YES;
}

/* Is there one run of usable memory below the loader's ceiling that the whole
 * image and the loader's staging area both fit in. Asked here rather than
 * discovered by the loader, because what the loader does when it does not fit
 * is stop with a message about a staging area (amd64) or halt BTX (i386),
 * neither of which names anything a reader can act on. */
static int staging_fits(uint64_t iso_size)
{
    uint64_t want = iso_size + STAGING_SLOP;
    uint64_t ceiling = loader_is_i386() ? STAGING_I386 : STAGING_AMD64;
    unsigned n = e820_read();
    unsigned i;

    for (i = 0; i < n; i++) {
        const e820_entry *e = e820_at(i);
        uint64_t end;

        if (e == (const e820_entry *)0 || e->type != E820_USABLE)
            continue;
        end = e->base + e->len;
        if (end > ceiling)
            end = ceiling;
        if (end > e->base && end - e->base >= want)
            return 1;
    }
    return 0;
}

int bsd_boot(bios_disk *d, fs_vol *p2, const part_entry *p2part,
             const fs_dirent *iso, boot_result *r, int *not_bsd)
{
    bsdmd_record_edit rec;
    fs_dirent conf;
    fs_extent conf_ext;
    size_t next = 0;
    uint32_t cfg_len = 0;

    if (not_bsd != (int *)0)
        *not_bsd = 1;

    boot_stage(r, "opening the ISO");
    if (fs_file_open(p2, &g_iso, iso) != FS_OK)
        return -1;
    /* Named, not identified: a medium this route applies to is an ISO9660
     * one and the registry would just as happily claim its UDF tree, which
     * is not where loader.conf is looked up from. */
    boot_stage(r, "reading the ISO's filesystem");
    if (fs_mount(&g_isovol, &fs_ops_iso9660, fs_file_read_cb,
                 &g_iso) != FS_OK)
        return -1;

    /* Where the settings go, and how much room there is for them. The file
     * has to be one contiguous run so a single override can replace its
     * head, which every medium met writes it as. */
    boot_stage(r, "locating the ISO's loader.conf");
    if (fs_lookup(&g_isovol, LOADER_CONF, &g_dir, &conf) != FS_OK ||
        conf.is_dir || conf.size < CONF_HEAD ||
        fs_extents(&g_isovol, &conf, &conf_ext, 1, &next) != FS_OK ||
        next != 1 || conf_ext.len != conf.size)
        return -1;
    if (fs_file_open(&g_isovol, &g_conf, &conf) != FS_OK ||
        fs_file_read(&g_conf, 0, CONF_HEAD, g_head) != FS_OK)
        return -1;

    boot_stage(r, "placing the memory-disk record");
    /* Sector 0: the false cache hit bsdmd.h describes reads the real-mode
     * interrupt table here, harmlessly, as FreeBSD never reads the system
     * area of a medium it mounts. */
    if (bsdmd_record(iso->size, 0, iso_read, (void *)0, &rec, g_path,
                     sizeof g_path) != BSDMD_OK) {
        r->advice = "this ISO has no free bytes in its root or /boot "
                    "directory for the record this route adds.";
        return -1;
    }

    /* From here the medium is one this route applies to, so a failure is a
     * real failure rather than a cue to chainload. */
    if (not_bsd != (int *)0)
        *not_bsd = 0;

    /* Asked here rather than found out, as linux.c asks before a 64-bit
     * kernel: an amd64 FreeBSD kernel on a processor without long mode dies
     * after the memory disk is loaded, with nothing on the screen that says
     * why. The medium's own loader would start the same kernel, so this is
     * a refusal, not a cue to chainload. */
    boot_stage(r, "checking the processor");
    if (kernel_is_amd64() && !linux_cpu_is_64()) {
        r->advice = "this image needs a 64-bit processor, and this one has "
                    "no 64-bit mode.";
        return -1;
    }

    boot_stage(r, "checking memory for the image");
    if (!staging_fits(iso->size)) {
        r->advice = loader_is_i386()
            ? "this ISO is too large for its i386 loader to hold as a memory "
              "disk: that loader runs under BTX and reaches only about 1 GB. "
              "A smaller i386 BSD ISO would boot."
            : "this machine has too little memory below 4 GB to hold the "
              "image, which is where FreeBSD's loader has to put it. Press c "
              "to boot the ISO's own loader instead.";
        return -1;
    }

    boot_stage(r, "writing the loader settings");
    if (bsdmd_config(g_path, BSD_CONSOLE, g_head, sizeof g_head,
                     g_cfg, sizeof g_cfg,
                     &cfg_len) != BSDMD_OK) {
        r->advice = "this ISO's loader.conf has no comment block long "
                    "enough to hold the memory-disk settings.";
        return -1;
    }

    boot_stage(r, "placing the medium's edits");
    vcd_overlay_reset();
    if (vcd_overlay(&g_iso, rec.off, rec.bytes, rec.len) != 0 ||
        vcd_overlay(&g_iso, conf_ext.vol_off, g_cfg, cfg_len) != 0) {
        vcd_overlay_reset();
        return -1;
    }

    bios_puts("falcon-bios: serving the medium as its own memory disk at ");
    bios_puts(g_path);
    bios_puts("\n");
    /* Not "starting", for the reason the UEFI route gives: the ISO's loader
     * reads the whole medium out of the virtual CD after this line, and the
     * coverage harness counts media read after "starting" as the kernel's
     * (tools/coverage/run-case.sh). */
    tui_step(u"handing the ISO to its own loader, as a memory disk");
    return vcd_chainload(d, p2, p2part, iso, &g_iso, r);
}
