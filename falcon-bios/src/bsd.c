/* falcon-bios: BSD media served as its own memory disk
 * (docs/DESIGN.md Section 6.4, docs/notes/bsd-route.md, bios-route.md 4.5).
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

#include <bios.h>
#include <bsd.h>
#include <vcd.h>
#include <e820.h>

#include <fs/fs.h>
#include <media/bsdmd.h>
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

/* What the route needs free above the image's own size, in one run below
 * 4 GB. FreeBSD's loader stages the kernel and its modules in the same
 * region as the memory disk and grows it, with slop for the kernel's
 * earliest allocations; the figure and the ceiling are the UEFI route's,
 * measured on the same media (falcon-efi/src/bsd/bsd.c). */
#define STAGING_SLOP (192ull * 1024ull * 1024ull)
#define STAGING_MAX  0x100000000ull

static fs_file g_iso;
static fs_vol  g_isovol;
static fs_dir  g_dir;
static fs_file g_conf;
static char    g_head[CONF_HEAD];
static char    g_cfg[BSDMD_CFG_MAX];
static char    g_path[BSDMD_PATH_MAX];

static void put(const char *s) { bios_screen_puts(s); }

/* bsdmd_record reads the medium; this is the same file, already open. */
static int iso_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    return fs_file_read(&g_iso, off, len, buf) == FS_OK ? 0 : -1;
}

/* Is there one run of usable memory below 4 GB that the whole image and the
 * loader's staging area both fit in. Asked here rather than discovered by
 * the loader, because what the loader does when it does not fit is stop with
 * a message about a staging area, which names nothing a reader can act on. */
static int staging_fits(uint64_t iso_size)
{
    uint64_t want = iso_size + STAGING_SLOP;
    unsigned n = e820_read();
    unsigned i;

    for (i = 0; i < n; i++) {
        const e820_entry *e = e820_at(i);
        uint64_t end;

        if (e == (const e820_entry *)0 || e->type != E820_USABLE)
            continue;
        end = e->base + e->len;
        if (end > STAGING_MAX)
            end = STAGING_MAX;
        if (end > e->base && end - e->base >= want)
            return 1;
    }
    return 0;
}

int bsd_boot(bios_disk *d, fs_vol *p2, const gpt_part *p2part,
             const fs_dirent *iso, int *not_bsd)
{
    bsdmd_record_edit rec;
    fs_dirent conf;
    fs_extent conf_ext;
    size_t next = 0;
    uint32_t cfg_len = 0;

    if (not_bsd != (int *)0)
        *not_bsd = 1;

    if (fs_file_open(p2, &g_iso, iso) != FS_OK) {
        put("  the image would not open\n");
        return -1;
    }
    /* Named, not identified: a medium this route applies to is an ISO9660
     * one and the registry would just as happily claim its UDF tree, which
     * is not where loader.conf is looked up from. */
    if (fs_mount(&g_isovol, &fs_ops_iso9660, fs_file_read_cb,
                 &g_iso) != FS_OK)
        return -1;

    /* Where the settings go, and how much room there is for them. The file
     * has to be one contiguous run so a single override can replace its
     * head, which every medium met writes it as. */
    if (fs_lookup(&g_isovol, LOADER_CONF, &g_dir, &conf) != FS_OK ||
        conf.is_dir || conf.size < CONF_HEAD ||
        fs_extents(&g_isovol, &conf, &conf_ext, 1, &next) != FS_OK ||
        next != 1 || conf_ext.len != conf.size)
        return -1;
    if (fs_file_open(&g_isovol, &g_conf, &conf) != FS_OK ||
        fs_file_read(&g_conf, 0, CONF_HEAD, g_head) != FS_OK)
        return -1;

    if (bsdmd_record(iso->size, iso_read, (void *)0, &rec, g_path,
                     sizeof g_path) != BSDMD_OK) {
        put("  this ISO has no free bytes in its root or /boot directory"
            " for the record this route adds\n");
        return -1;
    }

    /* From here the medium is one this route applies to, so a failure is a
     * real failure rather than a cue to chainload. */
    if (not_bsd != (int *)0)
        *not_bsd = 0;

    if (!staging_fits(iso->size)) {
        put("  this machine has too little memory below 4 GB to hold the"
            " image, which is where the loader has to put it. Press c to"
            " boot the ISO's own loader instead.\n");
        return -1;
    }

    if (bsdmd_config(g_path, BSD_CONSOLE, g_head, sizeof g_head,
                     g_cfg, sizeof g_cfg,
                     &cfg_len) != BSDMD_OK) {
        put("  this ISO's loader.conf has no comment block long enough to"
            " hold the memory-disk settings\n");
        return -1;
    }

    vcd_overlay_reset();
    if (vcd_overlay(&g_iso, rec.off, rec.bytes, rec.len) != 0 ||
        vcd_overlay(&g_iso, conf_ext.vol_off, g_cfg, cfg_len) != 0) {
        put("  the medium's edits do not fit the overlay\n");
        vcd_overlay_reset();
        return -1;
    }

    put("  serving the medium as its own memory disk at ");
    put(g_path);
    put("\n");
    return vcd_chainload(d, p2, p2part, iso, &g_iso);
}
