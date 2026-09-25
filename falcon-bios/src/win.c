/* falcon-bios: Windows install media, patched at boot
 * (docs/DESIGN.md Section 6.4, docs/notes/bios-route.md Section 4.3).
 *
 * What the virtual CD serves, against the ISO it serves from, which is the
 * UEFI route's list unchanged because the computation is the same file's
 * (falcon-efi/src/win/winpatch.c) over the same falcon-core:
 *
 *   the Partition Descriptors   longer, to reach the appended blocks
 *   boot.wim's File Entry       longer, its last extent cut to whole blocks
 *                               and a new one at the first appended block
 *   boot.wim's first 208 bytes  the patched WIM header
 *   past the ISO's end          boot.wim's old partial last block, the WIM
 *                               tail (jump binary, its configuration, the
 *                               new metadata, the new lookup table), zeros
 *                               to a whole block, then a copy of the anchor
 *
 * WHAT IS DIFFERENT HERE is where those bytes live and who moves them. Under
 * UEFI they are pool allocations the loader holds and the virtual CD reads
 * directly. Here the four small edits fit the resident handler's own pool in
 * conventional memory, and the appended tail does not: it is tens of
 * megabytes, so it goes above 64 MB and the handler has the BIOS move it
 * with INT 15h AH=87h, which is the one service that has always been able to
 * reach the far side of a megabyte.
 */

#include <bios.h>
#include <win.h>
#include <vcd.h>
#include <screen.h>
#include <heap.h>
#include <linux.h>

#include <cpuid.h>

#include <fs/fs.h>
#include <fs/udf.h>
#include <fs/iso9660.h>
#include <media/extmap.h>
#include <media/xpsetup.h>
#include <pe/pe.h>
#include <wim/wim.h>
#include <lib/string.h>

/* The jump binary, as make embeds it (falcon-winjump/build.mk), in both of
 * its builds. The patch puts it in place of winpeshl.exe, so it has to be
 * the boot image's width: a 32-bit WinPE cannot start a 64-bit program, and
 * Setup then never appears. */
extern const unsigned char falcon_winjump_bin[];
extern const size_t falcon_winjump_bin_len;
extern const unsigned char falcon_winjump32_bin[];
extern const size_t falcon_winjump32_bin_len;

/* The x86 virtual-CD driver, embedded the same way (falcon-bios/build.mk). The
 * 2000/XP route serves it into the ISO's \i386 and injects it into
 * txtsetup.sif, since those Windows have no WinPE and so no winjump. */
extern const unsigned char falcon_vdisk_bin[];
extern const size_t falcon_vdisk_bin_len;

#define BLK 2048u

/* The cfg carries the ISO's extent map as hex, so a WinPE that cannot read the
 * exFAT the ISO lives on (Windows 7 and Vista) can serve it through the driver
 * from raw disk sectors. Sized for the largest map, matching the EFI loader's
 * winpatch.c. */
#define WIN_CFG_MAX \
    (1024u + 2u * (EXTMAP_HDR_LEN + EXTMAP_MAX_EXTENTS * EXTMAP_EXT_LEN))

static fs_file  g_iso;
static udf_vol  g_uv;
static udf_file g_wim;
/* The medium's boot.wim comes off UDF on a bridge disc (Windows 7, Vista x86)
 * and off ISO 9660 on an ISO 9660-only one (Vista SP1 x64). g_i9 says which,
 * so wim_rd and the grow read and edit the right filesystem. */
static int          g_i9;
static iso9660_vol  g_iv;
static iso9660_file g_i9wim;
static iso9660_dirent g_i9dir;
/* bootmgr reads a boot CD through the ISO 9660 primary tree, so when the mount
 * above chose Joliet the primary /sources/boot.wim record is a second one to
 * relocate. g_i9_pri says it is a distinct record. */
static int          g_i9_pri;
static iso9660_vol  g_iv_pri;
static iso9660_dirent g_i9dir_pri;
static uint8_t  g_scratch[WIM_SCRATCH_LEN];
static uint8_t  g_fe[BLK];
static uint8_t  g_pd[2][BLK];
static uint8_t  g_hdr[WIM_HDR_LEN];
static char     g_cfg[WIN_CFG_MAX];
static udf_extent g_ext[UDF_MAX_EXTENTS];
static uint8_t  g_head[WIM_CHUNK_LEN];
static fs_extent    g_fsext[EXTMAP_MAX_EXTENTS];
static extmap_extent g_mext[EXTMAP_MAX_EXTENTS];
static uint8_t  g_map[EXTMAP_HDR_LEN + EXTMAP_MAX_EXTENTS * EXTMAP_EXT_LEN];

/* What Windows asks of a processor before it runs. PAE every Windows of the
 * WIM era needs. NX and SSE2 only Windows 8 and later need: without them
 * bootmgr or winload resets with nothing on screen (measured on the harness's
 * Pentium III, which has neither), but Vista and 7 run without either, so
 * `modern` gates that half. A Pentium III boots Vista or 7 x86 and not Windows
 * 10 x86. Returns the missing feature, or NULL when the processor will do. */
static const char *win_cpu_lacks(int modern)
{
    unsigned a, b, c, d;

    if (__get_cpuid(1u, &a, &b, &c, &d) == 0)
        return "CPUID";
    if ((d & (1u << 6)) == 0)
        return "PAE";
    if (!modern)
        return (const char *)0;
    if ((d & (1u << 26)) == 0)
        return "SSE2";
    if (__get_cpuid(0x80000000u, &a, &b, &c, &d) == 0 || a < 0x80000001u ||
        __get_cpuid(0x80000001u, &a, &b, &c, &d) == 0 ||
        (d & (1u << 20)) == 0)
        return "NX";
    return (const char *)0;
}

static int wim_rd(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    if (g_i9)
        return iso9660_file_read(&g_i9wim, off, len, buf) == ISO9660_OK ? 0 : -1;
    return udf_file_read(&g_wim, off, len, buf) == UDF_OK ? 0 : -1;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Case-insensitive match of an ISO 9660 name (the driver strips the version
 * suffix; a Joliet name is UCS-2 but ASCII for these) to lower-case ASCII. */
static int i9_name_eq(const iso9660_dirent *e, const char *s)
{
    uint16_t i;

    for (i = 0; s[i] != '\0'; i++) {
        uint16_t c = e->name[i];

        if (c >= 'A' && c <= 'Z')
            c = (uint16_t)(c + 32u);
        if (c != (uint16_t)(unsigned char)s[i])
            return 0;
    }
    return e->name_len == i;
}

static int i9_child(iso9660_dir *dir, const char *name, iso9660_dirent *out)
{
    while (iso9660_dir_next(dir, out) == 1)
        if (i9_name_eq(out, name))
            return 0;
    return -1;
}

/* /sources/boot.wim on the ISO 9660 tree, into *out. */
static int i9_bootwim(iso9660_vol *v, iso9660_dirent *out)
{
    iso9660_dir dir;
    iso9660_dirent e;

    if (iso9660_dir_open_root(v, &dir) != ISO9660_OK ||
        i9_child(&dir, "sources", &e) != 0 || !e.is_dir ||
        iso9660_dir_open(v, &dir, &e) != ISO9660_OK ||
        i9_child(&dir, "boot.wim", out) != 0)
        return -1;
    return 0;
}

/* Relocates boot.wim's single ISO 9660 extent to an appended run, rewriting a
 * directory record in place to name the new extent (new_lba, new_len). bootmgr's
 * CDFS reads a single extent but not a multi-extent file, so the grown
 * boot.wim cannot stay where it lies with a second extent tacked on; it moves
 * whole to space past the ISO's end. The record is the same length either way
 * (only its extent and data-length fields change), so its own directory block
 * is edited and served over the overlay. Called once per tree that names the
 * file, since bootmgr reads through the primary tree and not the Joliet one. */
static int i9_relocate(const iso9660_dirent *rec, uint32_t new_lba,
                       uint32_t new_len)
{
    uint8_t *sec = g_pd[0];
    uint16_t off = rec->rec_off;

    if (fs_file_read(&g_iso, (uint64_t)rec->rec_lba * BLK, BLK, sec) != FS_OK)
        return -1;
    put_le32(sec + off + 2, new_lba);          /* extent LBA, both-endian */
    put_be32(sec + off + 6, new_lba);
    put_le32(sec + off + 10, new_len);         /* data length, both-endian */
    put_be32(sec + off + 14, new_len);
    return vcd_overlay(&g_iso, (uint64_t)rec->rec_lba * BLK, sec, BLK);
}

static size_t put_dec(char *out, uint64_t v)
{
    char d[24];
    size_t n = 0;
    size_t i;

    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    while (v > 0 && n < sizeof d) {
        d[n++] = (char)('0' + (unsigned)(v % 10u));
        v /= 10u;
    }
    for (i = 0; i < n; i++)
        out[i] = d[n - 1u - i];
    return n;
}

/* Builds the ISO's extent map into g_map: the p2 GPT entry the winjump driver
 * finds the disk by, and the runs of the ISO's bytes on it. Returns the map's
 * length, or 0 if it could not be built (winjump then has only the VirtDisk
 * route, which is all a Windows 8+ WinPE needs). Mirrors the EFI loader's
 * build_map (falcon-efi/src/direct/direct.c). */
static size_t build_win_map(const bios_disk *d, fs_vol *p2,
                            const gpt_part *p2part, const fs_dirent *iso)
{
    extmap_hdr hdr;
    size_t n = 0, i, mlen = 0;

    memset(&hdr, 0, sizeof hdr);
    memcpy(hdr.part_guid, p2part->unique, sizeof hdr.part_guid);
    hdr.part_lba = p2part->start_lba;
    hdr.block_size = d->sector_size;
    hdr.iso_size = iso->size;

    if (fs_extents(p2, iso, g_fsext, EXTMAP_MAX_EXTENTS, &n) != FS_OK)
        return 0;
    for (i = 0; i < n; i++) {
        g_mext[i].offset = g_fsext[i].vol_off;
        g_mext[i].length = g_fsext[i].len;
    }
    hdr.n_extents = (uint32_t)n;
    if (extmap_encode(&hdr, g_mext, g_map, sizeof g_map, &mlen) != EXTMAP_OK)
        return 0;
    return mlen;
}

/* What falcon-winjump reads to find the medium once Windows is running: the
 * ISO's path on p2, its length, and (when built) its extent map as hex for the
 * driver path. The same file the UEFI route writes. */
static size_t make_cfg(const char *rel, uint64_t size, const uint8_t *map,
                       size_t map_len)
{
    static const char head[] =
        "# falcon-boot winjump configuration, made by the loader at boot\n"
        "iso=";
    static const char mid[] = "\nsize=";
    static const char hexd[] = "0123456789abcdef";
    size_t n;
    size_t i;

    memcpy(g_cfg, head, sizeof head - 1u);
    n = sizeof head - 1u;
    for (i = 0; rel[i] != '\0' && n + 40u < sizeof g_cfg; i++)
        g_cfg[n++] = rel[i];
    memcpy(g_cfg + n, mid, sizeof mid - 1u);
    n += sizeof mid - 1u;
    n += put_dec(g_cfg + n, size);
    g_cfg[n++] = '\n';

    if (map != (const uint8_t *)0 && map_len != 0 &&
        n + 5u + 2u * map_len < sizeof g_cfg) {
        memcpy(g_cfg + n, "map=", sizeof "map=" - 1u);
        n += sizeof "map=" - 1u;
        for (i = 0; i < map_len; i++) {
            g_cfg[n++] = hexd[map[i] >> 4];
            g_cfg[n++] = hexd[map[i] & 0x0fu];
        }
        g_cfg[n++] = '\n';
    }
    return n;
}

int win_boot(bios_disk *d, fs_vol *p2, const gpt_part *p2part,
             const fs_dirent *iso, const char *iso_rel, boot_result *r,
             int *unpatchable)
{
    udf_dirent we;
    wim_info w;
    size_t next = 0;
    size_t npd = 0;
    size_t i;
    size_t tail_cap;
    size_t tail_len = 0;
    size_t cfg_len;
    size_t map_len;
    uint8_t *lookup;
    uint8_t *meta;
    uint8_t *app;
    uint32_t pd[2];
    uint32_t moved = 0;
    uint32_t iso_blocks;
    uint32_t app_blocks;
    uint64_t app_len;
    uint64_t wim_size = 0;
    uint64_t wim_off = 0;
    const unsigned char *jump = falcon_winjump_bin;
    size_t jump_len = falcon_winjump_bin_len;

    if (unpatchable != (int *)0)
        *unpatchable = 1;

    r->stage = "opening the ISO";
    if (iso->size % BLK != 0)
        return -1;
    iso_blocks = (uint32_t)(iso->size / BLK);

    if (fs_file_open(p2, &g_iso, iso) != FS_OK)
        return -1;
    /* boot.wim comes off UDF on a Windows 7 or Vista x86 bridge disc, and off
     * ISO 9660 on an ISO 9660-only one (Vista SP1 x64). Try UDF first, since a
     * bridge disc carries both and its UDF tree is the one Windows reads. */
    r->stage = "reading the ISO's filesystem";
    g_i9 = 0;
    if (udf_mount(&g_uv, fs_file_read_cb, &g_iso) == UDF_OK &&
        udf_lookup(&g_uv, "/sources/boot.wim", &we) == UDF_OK &&
        udf_file_open(&g_uv, &g_wim, &we) == UDF_OK &&
        udf_file_extents(&g_uv, &we, g_ext, UDF_MAX_EXTENTS, &next) == UDF_OK &&
        next != 0 && g_ext[0].len >= WIM_HDR_LEN) {
        wim_size = we.size;
        wim_off = g_ext[0].vol_off;
    } else if (iso9660_mount(&g_iv, fs_file_read_cb, &g_iso) == ISO9660_OK &&
               i9_bootwim(&g_iv, &g_i9dir) == 0 && g_i9dir.parts == 1 &&
               iso9660_file_open(&g_iv, &g_i9wim, &g_i9dir) == ISO9660_OK &&
               g_i9dir.size >= WIM_HDR_LEN) {
        g_i9 = 1;
        wim_size = g_i9dir.size;
        wim_off = (uint64_t)g_i9dir.lba * BLK;
        /* bootmgr reads boot.wim through the primary tree. When the mount chose
         * Joliet its record is a second one to relocate, and it must be found
         * or bootmgr would read the un-relocated original; when the mount chose
         * plain, g_i9dir already is the primary record. */
        if (g_iv.names != ISO9660_NAMES_PLAIN) {
            if (iso9660_mount_primary(&g_iv_pri, fs_file_read_cb, &g_iso) !=
                    ISO9660_OK ||
                i9_bootwim(&g_iv_pri, &g_i9dir_pri) != 0 ||
                g_i9dir_pri.parts != 1)
                return -1;
            g_i9_pri = 1;
        }
    } else {
        return -1;
    }

    r->stage = "reading boot.wim";
    if (wim_open(wim_rd, (void *)0, wim_size, &w) != WIM_OK)
        return -1;

    /* From here the medium is one this route applies to, so a failure is a
     * real failure rather than a cue to chainload. */
    if (unpatchable != (int *)0)
        *unpatchable = 0;

    map_len = build_win_map(d, p2, p2part, iso);
    cfg_len = make_cfg(iso_rel, iso->size, map_len != 0 ? g_map : (uint8_t *)0,
                       map_len);
    /* Sized for the larger of the two builds: which one this boot.wim takes
     * is read out of it after the memory is reserved. */
    tail_cap = wim_patch_tail_max((size_t)w.boot_meta.orig,
                                  (size_t)w.lookup.orig,
                                  falcon_winjump_bin_len >
                                          falcon_winjump32_bin_len
                                      ? falcon_winjump_bin_len
                                      : falcon_winjump32_bin_len,
                                  cfg_len);

    /* Everything the patch needs at once, so a run that is big enough is
     * found once rather than discovered to be too small halfway through. */
    r->stage = "reserving memory for the patch";
    heap_reset();
    if (!heap_open((uint64_t)w.lookup.orig + w.boot_meta.orig +
                   (uint64_t)tail_cap + 4u * (uint64_t)BLK + 0x10000u)) {
        r->advice = "this machine has too little memory above 64 MB to "
                    "patch boot.wim.";
        return -1;
    }
    lookup = heap_alloc(w.lookup.orig);
    meta = heap_alloc(w.boot_meta.orig);
    app = heap_alloc((uint64_t)BLK + tail_cap + 2u * (uint64_t)BLK);
    if (lookup == (uint8_t *)0 || meta == (uint8_t *)0 ||
        app == (uint8_t *)0) {
        r->advice = "there is not enough memory to patch boot.wim.";
        return -1;
    }

    r->stage = "reading boot.wim";
    if (wim_rd((void *)0, w.lookup.offset, w.lookup.orig, lookup) != 0 ||
        wim_read_resource(&w, wim_rd, (void *)0, &w.boot_meta, meta,
                          (size_t)w.boot_meta.orig, g_scratch) != WIM_OK)
        return -1;

    /* Which build of the jump binary: winpeshl.exe's own machine type, since
     * that is the program it replaces. Read from its first chunk, which
     * holds the headers. */
    r->stage = "reading winpeshl.exe's headers";
    {
        wim_reshdr pe_res;
        size_t head_len = 0;
        pe_hdr h;

        if (wim_winpeshl(&w, lookup, meta, (size_t)w.boot_meta.orig,
                         &pe_res) != WIM_OK ||
            wim_read_head(&w, wim_rd, (void *)0, &pe_res, g_head, &head_len,
                          g_scratch) != WIM_OK ||
            pe_parse(g_head, head_len, &h) != PE_OK) {
            r->advice = "boot.wim has no winpeshl.exe this loader can read.";
            return -1;
        }
        if (h.machine == PE_MACHINE_I386 || h.machine == PE_MACHINE_AMD64) {
            /* winpeshl.exe's PE optional-header OS version is the Windows
             * version (offset 40/42, the same for PE32 and PE32+): 6.2 is
             * Windows 8, so 6.2 and up need NX and SSE2, Vista (6.0) and 7
             * (6.1) do not. */
            int modern = 1;
            const char *lacks;

            if ((size_t)h.opt + 44u <= head_len) {
                uint16_t maj = (uint16_t)(g_head[h.opt + 40] |
                                          ((uint16_t)g_head[h.opt + 41] << 8));
                uint16_t min = (uint16_t)(g_head[h.opt + 42] |
                                          ((uint16_t)g_head[h.opt + 43] << 8));

                modern = maj > 6 || (maj == 6 && min >= 2);
                bios_puts("falcon-bios: winpeshl targets Windows ");
                bios_put_dec(maj);
                bios_puts(".");
                bios_put_dec(min);
                bios_puts("\n");
            }
            lacks = win_cpu_lacks(modern);
            if (lacks != (const char *)0) {
                r->stage = "checking the processor";
                bios_puts("falcon-bios: this processor has no ");
                bios_puts(lacks);
                bios_puts("\n");
                /* The sentence names what is missing: it is the thing a
                 * person looking at an old machine can check. */
                r->advice =
                    lacks[0] == 'P'
                        ? "Windows needs PAE; this processor has none."
                    : lacks[0] == 'S'
                        ? "Windows 8 and later need NX and SSE2; this "
                          "processor has no SSE2."
                    : lacks[0] == 'N'
                        ? "Windows 8 and later need NX and SSE2; this "
                          "processor has no NX."
                        : "this processor does not answer CPUID.";
                return -1;
            }
        }
        if (h.machine == PE_MACHINE_I386) {
            jump = falcon_winjump32_bin;
            jump_len = falcon_winjump32_bin_len;
            bios_puts("falcon-bios: boot.wim is 32-bit Windows\n");
        } else if (h.machine == PE_MACHINE_AMD64) {
            /* Asked here, as linux.c asks before a 64-bit kernel: a 64-bit
             * Windows on a processor without long mode stops inside bootmgr
             * with nothing on screen to say why. */
            if (!linux_cpu_is_64()) {
                r->stage = "checking the processor";
                r->advice = "this is 64-bit Windows and this processor has "
                            "no 64-bit mode.";
                return -1;
            }
            bios_puts("falcon-bios: boot.wim is 64-bit Windows\n");
        } else {
            r->advice = "boot.wim is for a processor this loader has no "
                        "jump binary for.";
            return -1;
        }
    }

    /* The tail is built one block into the append buffer: the old partial
     * last block goes in front of it once the File Entry says how long that
     * is. A block of slack at the end holds the zero padding, and one more
     * the anchor. */
    r->stage = "patching boot.wim";
    if (wim_patch_build(&w, lookup, meta, (size_t)w.boot_meta.orig,
                        jump, jump_len, g_cfg,
                        cfg_len, g_hdr, app + BLK, tail_cap,
                        &tail_len) != WIM_OK) {
        r->advice = "boot.wim is not one this loader can patch.";
        return -1;
    }

    r->stage = "growing boot.wim";
    vcd_overlay_reset();
    if (g_i9) {
        /* ISO 9660-only medium (Vista SP1 x64): bootmgr's CDFS reads a single
         * extent but not a multi-extent file, so boot.wim cannot grow in place.
         * It is relocated whole to one new extent past the ISO's end, but its
         * bulk is not copied: those sectors redirect to where the original
         * boot.wim already lies on the disk. Only two things go to memory - the
         * patched header sector at the start, and a tail of the original's
         * partial last block followed by the patch tail. */
        uint32_t body_cds = (uint32_t)(wim_size / BLK);
        uint8_t *hdrsec = g_pd[1];   /* not g_pd[0]: i9_relocate reads records
                                      * into that one */

        moved = (uint32_t)(wim_size - (uint64_t)body_cds * BLK);
        memmove(app + moved, app + BLK, tail_len);
        if (moved != 0 &&
            iso9660_file_read(&g_i9wim, (uint64_t)body_cds * BLK, moved, app)
                != ISO9660_OK)
            goto no_room;
        app_len = ((uint64_t)moved + tail_len + BLK - 1u) / BLK * BLK;
        memset(app + moved + tail_len, 0, (size_t)(app_len - moved - tail_len));
        app_blocks = (uint32_t)(app_len / BLK);
        if (iso9660_file_read(&g_i9wim, 0, BLK, hdrsec) != ISO9660_OK)
            goto no_room;
        memcpy(hdrsec, g_hdr, WIM_HDR_LEN);
        if (i9_relocate(&g_i9dir, iso_blocks, (uint32_t)(wim_size + tail_len))
                != 0 ||
            (g_i9_pri &&
             i9_relocate(&g_i9dir_pri, iso_blocks,
                         (uint32_t)(wim_size + tail_len)) != 0) ||
            vcd_overlay_redirect(g_i9dir.lba, body_cds) != 0 ||
            vcd_overlay_at(iso_blocks, hdrsec) != 0 ||
            vcd_overlay_append(heap_phys(app), app_blocks) != 0)
            goto no_room;
    } else {
        /* UDF: the File Entry gains the appended tail as its last extent, with
         * boot.wim's old partial last block moved in front of it, and the
         * anchor and partition descriptors follow the now-longer volume. */
        if (fs_file_read(&g_iso, ((uint64_t)g_uv.part_start + we.icb_lbn) * BLK,
                         BLK, g_fe) != FS_OK ||
            udf_fe_append(g_fe, we.icb_lbn, tail_len,
                          iso_blocks - g_uv.part_start, &moved) != UDF_OK)
            goto no_room;
        memmove(app + moved, app + BLK, tail_len);
        if (moved != 0 &&
            udf_file_read(&g_wim, we.size - moved, moved, app) != UDF_OK)
            goto no_room;
        app_len = ((uint64_t)moved + tail_len + BLK - 1u) / BLK * BLK;
        memset(app + moved + tail_len, 0, (size_t)(app_len - moved - tail_len));
        app_blocks = (uint32_t)(app_len / BLK) + 1u;
        if (fs_file_read(&g_iso, (uint64_t)256u * BLK, BLK,
                         app + app_len) != FS_OK ||
            udf_avdp_at(app + app_len, iso_blocks + app_blocks - 1u) != UDF_OK)
            goto no_room;
        if (udf_pd_blocks(&g_uv, pd, &npd) != UDF_OK)
            goto no_room;
        for (i = 0; i < npd; i++) {
            if (fs_file_read(&g_iso, (uint64_t)pd[i] * BLK, BLK,
                             g_pd[i]) != FS_OK ||
                udf_pd_set_length(g_pd[i], pd[i],
                                  iso_blocks + app_blocks - g_uv.part_start) !=
                    UDF_OK ||
                vcd_overlay(&g_iso, (uint64_t)pd[i] * BLK, g_pd[i], BLK) != 0)
                goto no_room;
        }
        if (vcd_overlay(&g_iso, ((uint64_t)g_uv.part_start + we.icb_lbn) * BLK,
                        g_fe, BLK) != 0 ||
            vcd_overlay(&g_iso, wim_off, g_hdr, WIM_HDR_LEN) != 0 ||
            vcd_overlay_append(heap_phys(app), app_blocks) != 0)
            goto no_room;
    }

    bios_puts("falcon-bios: boot.wim patched in memory, ");
    bios_put_dec((uint64_t)app_blocks * BLK / 1024u);
    bios_puts(" KiB served\n");
    screen_out("booting with boot.wim patched in memory\n\n");
    r->stage = "booting";
    return vcd_chainload(d, p2, p2part, iso, &g_iso, r);

no_room:
    vcd_overlay_reset();
    return -1;
}

/* Windows 2000/XP have no WinPE, so the loader cannot patch a running PE; it
 * injects falcon-vdisk into text-mode setup instead (DESIGN Section 6.3,
 * windows-route.md 7.4). The served ISO is edited in memory the way the
 * boot.wim relocation edits an ISO 9660-only medium, but the targets are
 * i386/txtsetup.sif and a new i386/_FALCON.SYS:
 *   - txtsetup.sif grows by three lines (txtsetup_inject) and is served whole
 *     from memory past the ISO's end, its directory record rewritten to the
 *     new extent;
 *   - _FALCON.SYS (the embedded driver) is served past that, its record added
 *     in i386's last-block slack, and i386's own record grown so a strict
 *     reader (setupldr) walks past the old end and sees it;
 *   - the PVD volume space size is grown to cover the appended blocks.
 * setupldr reads all of this through the ISO 9660 primary (8.3) tree, which is
 * why the mount and every lookup use it. *not_xp stays 1 when the medium is
 * not 2000/XP media, so the caller boots it as it is. Returns only when it did
 * not boot, with *r filled in.
 *
 * This wires the driver in; giving its DriverEntry the stick's extents at
 * boot-start with no winjump (piece 3, windows-route.md 7.4) is separate, so
 * for now setup loads the driver and then cannot reach its boot device. */
int win_xp_boot(bios_disk *d, fs_vol *p2, const gpt_part *p2part,
                const fs_dirent *iso, boot_result *r, int *not_xp)
{
    static uint8_t pvd[BLK], i386rec[BLK], dirblk[BLK];
    iso9660_dir dir;
    iso9660_dirent i386, sif;
    iso9660_file f;
    uint8_t *raw, *app;
    size_t sif_len, grown_len = 0, sys_len = falcon_vdisk_bin_len;
    uint32_t iso_blocks, sif_start, sys_start, total;
    uint32_t sif_blocks, sys_blocks, app_blocks, blk_lba, new_i386_len;
    /* 33 + strlen("_FALCON.SYS;1") = 46, already even. */
    const uint32_t reclen = 46u;

    if (not_xp != (int *)0)
        *not_xp = 1;

    r->stage = "opening the ISO";
    if (iso->size % BLK != 0)
        return -1;
    iso_blocks = (uint32_t)(iso->size / BLK);
    if (fs_file_open(p2, &g_iso, iso) != FS_OK)
        return -1;

    /* setupldr reads the primary (8.3) tree, so mount and look up there. */
    r->stage = "reading the ISO's filesystem";
    if (iso9660_mount_primary(&g_iv_pri, fs_file_read_cb, &g_iso) != ISO9660_OK)
        return -1;
    if (iso9660_dir_open_root(&g_iv_pri, &dir) != ISO9660_OK ||
        i9_child(&dir, "i386", &i386) != 0 || !i386.is_dir)
        return -1;
    if (iso9660_dir_open(&g_iv_pri, &dir, &i386) != ISO9660_OK ||
        i9_child(&dir, "txtsetup.sif", &sif) != 0)
        return -1;

    /* From here it is 2000/XP media, so a failure is a real failure. */
    if (not_xp != (int *)0)
        *not_xp = 0;
    if (sif.parts != 1 || sif.size < 64u)
        return -1;
    sif_len = (size_t)sif.size;

    /* Two heap_alloc regions (the sif read buffer and the append region), each
     * rounded up to a page, plus room for the sif to grow and the driver: a
     * generous slack covers both roundings so a barely-fitting request does not
     * fail the second alloc. */
    r->stage = "reserving memory for the driver injection";
    heap_reset();
    if (!heap_open((uint64_t)sif_len * 2u + (uint64_t)sys_len + 0x8000u)) {
        r->advice = "this machine has too little memory above 64 MB to inject "
                    "the driver.";
        return -1;
    }
    raw = heap_alloc(sif_len);
    /* The append region: the grown sif block-padded, then the driver
     * block-padded, served as one run past the ISO's end. */
    app = heap_alloc((uint64_t)sif_len + (uint64_t)sys_len +
                     0x1000u + 3u * (uint64_t)BLK);
    if (raw == (uint8_t *)0 || app == (uint8_t *)0) {
        r->advice = "there is not enough memory to inject the driver.";
        return -1;
    }

    r->stage = "reading txtsetup.sif";
    if (iso9660_file_open(&g_iv_pri, &f, &sif) != ISO9660_OK ||
        iso9660_file_read(&f, 0, sif_len, raw) != ISO9660_OK)
        return -1;

    r->stage = "injecting the driver into txtsetup.sif";
    if (txtsetup_inject((const char *)raw, sif_len, "falconcd", "_FALCON.SYS",
                        "Falcon Virtual CD", (char *)app,
                        sif_len + 0x1000u, &grown_len) != XPSETUP_OK) {
        r->advice = "txtsetup.sif is not one this loader can edit.";
        return -1;
    }

    sif_blocks = (uint32_t)((grown_len + BLK - 1u) / BLK);
    sys_blocks = (uint32_t)((sys_len + BLK - 1u) / BLK);
    sif_start = iso_blocks;
    sys_start = iso_blocks + sif_blocks;
    app_blocks = sif_blocks + sys_blocks;
    total = iso_blocks + app_blocks;

    /* Pad the sif to a whole block, lay the driver after it, pad that too. */
    memset(app + grown_len, 0, (size_t)sif_blocks * BLK - grown_len);
    memcpy(app + (size_t)sif_blocks * BLK, falcon_vdisk_bin, sys_len);
    memset(app + (size_t)sif_blocks * BLK + sys_len, 0,
           (size_t)sys_blocks * BLK - sys_len);

    r->stage = "growing the ISO in memory";
    vcd_overlay_reset();

    /* The appended sif and driver, served as CD sectors past the ISO's end. */
    if (vcd_overlay_append(heap_phys(app), app_blocks) != 0)
        goto no_room;

    /* PVD (LBA 16) volume space size -> the new total, both-endian at 80/84. */
    if (fs_file_read(&g_iso, 16u * (uint64_t)BLK, BLK, pvd) != FS_OK)
        goto no_room;
    put_le32(pvd + 80, total);
    put_be32(pvd + 84, total);
    if (vcd_overlay_at(16u, pvd) != 0)
        goto no_room;

    /* txtsetup.sif's record -> the appended copy (extent and length). */
    if (i9_relocate(&sif, sif_start, (uint32_t)grown_len) != 0)
        goto no_room;

    /* i386's own record data length grows by the .sys record, so a strict
     * reader walks past the old end to _FALCON.SYS. Its record is in the root
     * directory (a different block from the sif's record and the .sys append
     * target). */
    new_i386_len = (uint32_t)i386.size + reclen;
    if (fs_file_read(&g_iso, (uint64_t)i386.rec_lba * BLK, BLK, i386rec) !=
            FS_OK)
        goto no_room;
    put_le32(i386rec + i386.rec_off + 10, new_i386_len);
    put_be32(i386rec + i386.rec_off + 14, new_i386_len);
    if (vcd_overlay_at(i386.rec_lba, i386rec) != 0)
        goto no_room;

    /* _FALCON.SYS's record, in i386's last-block slack. */
    if (iso9660_append_record(&g_iv_pri, i386.lba, (uint32_t)i386.size,
                              "_FALCON.SYS;1", sys_start, (uint32_t)sys_len,
                              dirblk, &blk_lba) != ISO9660_OK) {
        r->advice = "the ISO's i386 directory has no room for the driver's "
                    "record.";
        goto no_room;
    }
    /* Two edits landing in one block would clobber each other through separate
     * overlays. Not seen on XP SP3 (records at 136, 24, 16; the .sys at 155),
     * and merging them is not worth guessing until a medium needs it. */
    if (blk_lba == sif.rec_lba || blk_lba == i386.rec_lba || blk_lba == 16u) {
        r->advice = "this ISO's i386 layout is one this loader does not edit.";
        goto no_room;
    }
    if (vcd_overlay_at(blk_lba, dirblk) != 0)
        goto no_room;

    bios_puts("falcon-bios: 2000/XP driver injected, ");
    bios_put_dec((uint64_t)app_blocks * BLK / 1024u);
    bios_puts(" KiB served\n");
    screen_out("booting Windows setup with the virtual-CD driver injected\n\n");
    r->stage = "booting";
    return vcd_chainload(d, p2, p2part, iso, &g_iso, r);

no_room:
    vcd_overlay_reset();
    return -1;
}
