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

#include <fs/fs.h>
#include <media/medium.h>
#include <media/bootfs.h>
#include <media/bootcfg.h>
#include <media/family.h>
#include <lib/string.h>

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

int scan_iso(fs_vol *p2, const fs_dirent *ent)
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
    return 0;
}
