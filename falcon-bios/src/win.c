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

#include <fs/fs.h>
#include <fs/udf.h>
#include <wim/wim.h>
#include <lib/string.h>

/* The jump binary, as make embeds it (falcon-winjump/build.mk). */
extern const unsigned char falcon_winjump_bin[];
extern const size_t falcon_winjump_bin_len;

#define BLK 2048u

static fs_file  g_iso;
static udf_vol  g_uv;
static udf_file g_wim;
static uint8_t  g_scratch[WIM_SCRATCH_LEN];
static uint8_t  g_fe[BLK];
static uint8_t  g_pd[2][BLK];
static uint8_t  g_hdr[WIM_HDR_LEN];
static char     g_cfg[512];
static udf_extent g_ext[UDF_MAX_EXTENTS];

static int wim_rd(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    return udf_file_read(&g_wim, off, len, buf) == UDF_OK ? 0 : -1;
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

/* What falcon-winjump reads to find the medium once Windows is running: the
 * ISO's path on p2 and its length. The same file the UEFI route writes. */
static size_t make_cfg(const char *rel, uint64_t size)
{
    static const char head[] =
        "# falcon-boot winjump configuration, made by the loader at boot\n"
        "iso=";
    static const char mid[] = "\nsize=";
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
    uint8_t *lookup;
    uint8_t *meta;
    uint8_t *app;
    uint32_t pd[2];
    uint32_t moved = 0;
    uint32_t iso_blocks;
    uint32_t app_blocks;
    uint64_t app_len;

    if (unpatchable != (int *)0)
        *unpatchable = 1;

    r->stage = "opening the ISO";
    if (iso->size % BLK != 0)
        return -1;
    iso_blocks = (uint32_t)(iso->size / BLK);

    if (fs_file_open(p2, &g_iso, iso) != FS_OK)
        return -1;
    r->stage = "reading the ISO's UDF tree";
    if (udf_mount(&g_uv, fs_file_read_cb, &g_iso) != UDF_OK ||
        udf_lookup(&g_uv, "/sources/boot.wim", &we) != UDF_OK ||
        udf_file_open(&g_uv, &g_wim, &we) != UDF_OK ||
        udf_file_extents(&g_uv, &we, g_ext, UDF_MAX_EXTENTS, &next) !=
            UDF_OK ||
        next == 0 || g_ext[0].len < WIM_HDR_LEN)
        return -1;

    r->stage = "reading boot.wim";
    if (wim_open(wim_rd, (void *)0, we.size, &w) != WIM_OK)
        return -1;

    /* From here the medium is one this route applies to, so a failure is a
     * real failure rather than a cue to chainload. */
    if (unpatchable != (int *)0)
        *unpatchable = 0;

    cfg_len = make_cfg(iso_rel, iso->size);
    tail_cap = wim_patch_tail_max((size_t)w.boot_meta.orig,
                                  (size_t)w.lookup.orig,
                                  falcon_winjump_bin_len, cfg_len);

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

    /* The tail is built one block into the append buffer: the old partial
     * last block goes in front of it once the File Entry says how long that
     * is. A block of slack at the end holds the zero padding, and one more
     * the anchor. */
    r->stage = "patching boot.wim";
    if (wim_patch_build(&w, lookup, meta, (size_t)w.boot_meta.orig,
                        falcon_winjump_bin, falcon_winjump_bin_len, g_cfg,
                        cfg_len, g_hdr, app + BLK, tail_cap,
                        &tail_len) != WIM_OK) {
        r->advice = "boot.wim is not one this loader can patch.";
        return -1;
    }

    r->stage = "rewriting boot.wim's UDF entry";
    if (fs_file_read(&g_iso, ((uint64_t)g_uv.part_start + we.icb_lbn) * BLK,
                     BLK, g_fe) != FS_OK ||
        udf_fe_append(g_fe, we.icb_lbn, tail_len,
                      iso_blocks - g_uv.part_start, &moved) != UDF_OK)
        return -1;
    memmove(app + moved, app + BLK, tail_len);
    if (moved != 0 &&
        udf_file_read(&g_wim, we.size - moved, moved, app) != UDF_OK)
        return -1;
    app_len = ((uint64_t)moved + tail_len + BLK - 1u) / BLK * BLK;
    memset(app + moved + tail_len, 0, (size_t)(app_len - moved - tail_len));
    app_blocks = (uint32_t)(app_len / BLK) + 1u;
    r->stage = "rewriting the UDF partition";
    if (fs_file_read(&g_iso, (uint64_t)256u * BLK, BLK,
                     app + app_len) != FS_OK ||
        udf_avdp_at(app + app_len, iso_blocks + app_blocks - 1u) != UDF_OK)
        return -1;

    if (udf_pd_blocks(&g_uv, pd, &npd) != UDF_OK)
        return -1;
    for (i = 0; i < npd; i++) {
        if (fs_file_read(&g_iso, (uint64_t)pd[i] * BLK, BLK,
                         g_pd[i]) != FS_OK ||
            udf_pd_set_length(g_pd[i], pd[i],
                              iso_blocks + app_blocks - g_uv.part_start) !=
                UDF_OK)
            return -1;
    }

    /* The four small edits go in the handler's own pool; the appended blocks
     * stay where they are and the BIOS moves them. */
    r->stage = "placing the medium's edits";
    vcd_overlay_reset();
    for (i = 0; i < npd; i++) {
        if (vcd_overlay(&g_iso, (uint64_t)pd[i] * BLK, g_pd[i], BLK) != 0)
            goto no_room;
    }
    if (vcd_overlay(&g_iso, ((uint64_t)g_uv.part_start + we.icb_lbn) * BLK,
                    g_fe, BLK) != 0 ||
        vcd_overlay(&g_iso, g_ext[0].vol_off, g_hdr, WIM_HDR_LEN) != 0)
        goto no_room;
    if (vcd_overlay_append(heap_phys(app), app_blocks) != 0)
        goto no_room;

    bios_puts("falcon-bios: boot.wim patched in memory, ");
    bios_put_dec((uint64_t)app_blocks * BLK / 1024u);
    bios_puts(" KiB appended\n");
    screen_out("booting with boot.wim patched in memory\n\n");
    r->stage = "booting";
    return vcd_chainload(d, p2, p2part, iso, &g_iso, r);

no_room:
    vcd_overlay_reset();
    return -1;
}
