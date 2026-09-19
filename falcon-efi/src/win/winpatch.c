/* Windows install media, patched at boot. See falcon-efi/src/win/winpatch.h.
 *
 * What the virtual CD serves, against the ISO it serves from:
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
 * All of it is computed here, into memory the loader holds until the payload
 * returns; the ISO on p2 is only read.
 */

#include <win/winpatch.h>

#include <iso/iso.h>
#include <console/menu.h>

#include <fs/udf.h>
#include <wim/wim.h>
#include <lib/string.h>

/* The jump binary, as make embeds it (falcon-winjump/build.mk). */
extern const unsigned char falcon_winjump_bin[];
extern const size_t falcon_winjump_bin_len;

#define BLK 2048u

static fs_file g_iso;
static udf_vol g_uv;
static udf_file g_wim;
static uint8_t g_scratch[WIM_SCRATCH_LEN];
static uint8_t g_fe[BLK];
static uint8_t g_pd[2][BLK];
static uint8_t g_hdr[WIM_HDR_LEN];
static char g_cfg[512];

static int wim_rd(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    return udf_file_read(&g_wim, off, len, buf) == UDF_OK ? 0 : -1;
}

static void *pool(EFI_BOOT_SERVICES *bs, UINTN n)
{
    void *p = (void *)0;

    if (bs->AllocatePool(EfiLoaderData, n, &p) != EFI_SUCCESS)
        return (void *)0;
    return p;
}

static size_t put_dec(char *out, uint64_t v)
{
    char d[24];
    size_t n = 0, i;

    do {
        d[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0);
    for (i = 0; i < n; i++)
        out[i] = d[n - 1u - i];
    return n;
}

/* falcon-winjump.cfg (falcon-winjump/src/winjump.c reads it). */
static size_t make_cfg(const char *rel, uint64_t size)
{
    static const char head[] =
        "# falcon-boot winjump configuration, made by the loader at boot\n"
        "iso=";
    static const char mid[] = "\nsize=";
    size_t n = 0, i;

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

static void add_ovl(iso_overlay *o, uint64_t off, uint64_t len,
                    const uint8_t *mem)
{
    size_t i = o->n;

    /* Kept ascending by insertion: the set is a handful of entries. */
    while (i > 0 && o->ovl[i - 1u].iso_off > off) {
        o->ovl[i] = o->ovl[i - 1u];
        i--;
    }
    o->ovl[i].iso_off = off;
    o->ovl[i].len = len;
    o->ovl[i].src = (fs_file *)0;
    o->ovl[i].src_off = 0;
    o->ovl[i].mem = mem;
    o->n++;
}

EFI_STATUS win_patched_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent,
                            fs_vol *p2, const fs_dirent *iso,
                            const char *iso_rel, win_result *r)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    iso_boot_result ir = { L"starting", EFI_SUCCESS,
                           (const family_desc *)0 };
    udf_dirent we;
    udf_extent ext[UDF_MAX_EXTENTS];
    size_t next = 0, npd = 0, i, tail_cap, tail_len = 0, cfg_len;
    uint8_t *lookup = (uint8_t *)0, *meta = (uint8_t *)0;
    uint8_t *app = (uint8_t *)0;
    uint32_t pd[2], moved = 0, iso_blocks, app_blocks;
    uint64_t app_len;
    wim_info w;
    iso_overlay ovl;
    EFI_STATUS status = EFI_UNSUPPORTED;

    r->stage = L"opening the ISO";
    r->status = EFI_SUCCESS;
    r->unpatchable = 1;
    r->advice = (const CHAR16 *)0;
    memset(&ovl, 0, sizeof ovl);

    if (iso->size % BLK != 0 || iso->size / BLK > 0xffffffffull)
        return r->status = EFI_UNSUPPORTED;
    iso_blocks = (uint32_t)(iso->size / BLK);
    if (fs_file_open(p2, &g_iso, iso) != FS_OK)
        return r->status = EFI_DEVICE_ERROR;
    r->stage = L"reading the ISO's UDF tree";
    if (udf_mount(&g_uv, fs_file_read_cb, &g_iso) != UDF_OK ||
        udf_lookup(&g_uv, "/sources/boot.wim", &we) != UDF_OK ||
        udf_file_open(&g_uv, &g_wim, &we) != UDF_OK ||
        udf_file_extents(&g_uv, &we, ext, UDF_MAX_EXTENTS, &next) !=
            UDF_OK ||
        next == 0 || ext[0].len < WIM_HDR_LEN)
        return r->status = EFI_UNSUPPORTED;

    r->stage = L"reading boot.wim";
    if (wim_open(wim_rd, (void *)0, we.size, &w) != WIM_OK)
        return r->status = EFI_UNSUPPORTED;
    lookup = pool(bs, (UINTN)w.lookup.orig);
    meta = pool(bs, (UINTN)w.boot_meta.orig);
    if (lookup == (uint8_t *)0 || meta == (uint8_t *)0) {
        status = EFI_OUT_OF_RESOURCES;
        goto out;
    }
    if (wim_rd((void *)0, w.lookup.offset, w.lookup.orig, lookup) != 0 ||
        wim_read_resource(&w, wim_rd, (void *)0, &w.boot_meta, meta,
                          (size_t)w.boot_meta.orig, g_scratch) != WIM_OK) {
        status = EFI_UNSUPPORTED;
        goto out;
    }

    /* The tail is built one block into the append buffer: the old partial
     * last block goes in front of it once the File Entry says how long that
     * is. A block of slack at the end holds the zero padding, and one more
     * the anchor. */
    r->stage = L"patching boot.wim";
    cfg_len = make_cfg(iso_rel, iso->size);
    tail_cap = wim_patch_tail_max((size_t)w.boot_meta.orig,
                                  (size_t)w.lookup.orig,
                                  falcon_winjump_bin_len, cfg_len);
    app = pool(bs, (size_t)BLK + tail_cap + (size_t)2u * BLK);
    if (app == (uint8_t *)0) {
        status = EFI_OUT_OF_RESOURCES;
        goto out;
    }
    if (wim_patch_build(&w, lookup, meta, (size_t)w.boot_meta.orig,
                        falcon_winjump_bin, falcon_winjump_bin_len, g_cfg,
                        cfg_len, g_hdr, app + BLK, tail_cap, &tail_len) !=
        WIM_OK) {
        status = EFI_UNSUPPORTED;
        goto out;
    }

    /* From here on the media is patchable; a failure is a real one. */
    r->unpatchable = 0;
    r->stage = L"rewriting boot.wim's UDF entry";
    if (fs_file_read(&g_iso, ((uint64_t)g_uv.part_start + we.icb_lbn) * BLK,
                     BLK, g_fe) != FS_OK ||
        udf_fe_append(g_fe, we.icb_lbn, tail_len,
                      iso_blocks - g_uv.part_start, &moved) != UDF_OK) {
        status = EFI_UNSUPPORTED;
        goto out;
    }
    memmove(app + moved, app + BLK, tail_len);
    if (moved != 0 &&
        udf_file_read(&g_wim, we.size - moved, moved, app) != UDF_OK) {
        status = EFI_DEVICE_ERROR;
        goto out;
    }
    app_len = ((uint64_t)moved + tail_len + BLK - 1u) / BLK * BLK;
    memset(app + moved + tail_len, 0, (size_t)(app_len - moved - tail_len));
    app_blocks = (uint32_t)(app_len / BLK) + 1u;
    if (fs_file_read(&g_iso, (uint64_t)256u * BLK, BLK, app + app_len) != FS_OK ||
        udf_avdp_at(app + app_len, iso_blocks + app_blocks - 1u) !=
            UDF_OK) {
        status = EFI_UNSUPPORTED;
        goto out;
    }

    r->stage = L"rewriting the UDF partition";
    if (udf_pd_blocks(&g_uv, pd, &npd) != UDF_OK) {
        status = EFI_UNSUPPORTED;
        goto out;
    }
    for (i = 0; i < npd; i++) {
        if (fs_file_read(&g_iso, (uint64_t)pd[i] * BLK, BLK, g_pd[i]) !=
                FS_OK ||
            udf_pd_set_length(g_pd[i], pd[i],
                              iso_blocks + app_blocks - g_uv.part_start) !=
                UDF_OK) {
            status = EFI_UNSUPPORTED;
            goto out;
        }
        add_ovl(&ovl, (uint64_t)pd[i] * BLK, BLK, g_pd[i]);
    }
    add_ovl(&ovl, ((uint64_t)g_uv.part_start + we.icb_lbn) * BLK, BLK, g_fe);
    add_ovl(&ovl, ext[0].vol_off, WIM_HDR_LEN, g_hdr);
    add_ovl(&ovl, iso->size, (uint64_t)app_blocks * BLK, app);
    ovl.extra = (uint64_t)app_blocks * BLK;

    menu_out(st, L"booting with boot.wim patched in memory\r\n\r\n");
    r->stage = L"booting";
    status = iso_boot_overlaid(st, parent, p2, iso, ISO_MEDIA_CD, &ovl, &ir);
    r->stage = ir.stage;
    r->status = ir.status;

out:
    if (app != (uint8_t *)0)
        bs->FreePool(app);
    if (meta != (uint8_t *)0)
        bs->FreePool(meta);
    if (lookup != (uint8_t *)0)
        bs->FreePool(lookup);
    if (EFI_ERROR(status))
        r->status = status;
    return status;
}
