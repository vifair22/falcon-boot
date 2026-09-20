/* BSD install media served as its own memory disk. See bsd.h. */

#include <bsd/bsd.h>

#include <iso/iso.h>
#include <iso/overlay.h>
#include <console/menu.h>

#include <fs/fs.h>
#include <media/bsdmd.h>
#include <lib/string.h>

/* The file whose opening comments the settings are written over. The ISO's
 * own /boot/loader.conf is read after this one and would win, but it is a
 * few hundred bytes with no comment block to spend, and nothing in these
 * media sets md_image_* there. */
#define LOADER_CONF "/boot/defaults/loader.conf"

/* Enough of loader.conf to find the end of its opening comment block; 442
 * bytes on FreeBSD, pfSense and OPNsense alike. */
#define CONF_HEAD 1024u

/* What the route needs free, above the image's own size, in a single run
 * below 4 GB: FreeBSD's loader stages the kernel and its modules in the same
 * region as the memory disk, starting at a 64 MB area it then grows, with
 * 8 MB of slop at the end for the kernel's earliest allocations
 * (stand/efi/loader/copy.c). 192 MB covers that with room for the module set
 * a firewall distribution loads. */
#define STAGING_SLOP (192ull * 1024ull * 1024ull)

/* The ceiling the same file imposes: the staged image has to live under
 * 4 GB, because that is as far as the amd64 kernel's first page tables
 * reach. Measured 2026-09-19: OPNsense 25.1's 2.2 GB DVD fails here with
 * "efi_check_space: Unable to expand staging area" on a machine with 8 GB. */
#define STAGING_MAX 0x100000000ull

static fs_file g_iso;
static fs_vol  g_isovol;
static fs_dir  g_dir;

/* The ISO's own bytes, for bsdmd's walk of its directory records. Reads go
 * through the p2 file handle this module opened, not the virtual CD, which
 * does not exist yet when the edits are computed. */
static int iso_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    return fs_file_read(&g_iso, off, len, buf) == FS_OK ? 0 : -1;
}

/* Can FreeBSD's loader stage an image this size?
 *
 * Asked by doing what it will do: allocate that many contiguous pages below
 * 4 GB and give them straight back. An estimate from the memory map would
 * have to model the firmware's own fragmentation; an allocation IS the
 * question, and the pages are held for the length of one call. */
static BOOLEAN staging_fits(EFI_BOOT_SERVICES *bs, UINT64 size)
{
    EFI_PHYSICAL_ADDRESS addr = STAGING_MAX - 1u;
    UINT64 want = size + STAGING_SLOP;
    UINTN pages;

    if (want >= STAGING_MAX)
        return FALSE;
    pages = (UINTN)((want + 0xFFFu) / 0x1000u);
    if (bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &addr) !=
        EFI_SUCCESS)
        return FALSE;
    (void)bs->FreePages(addr, pages);
    return TRUE;
}

/* The medium published with the two edits, and something started against it.
 *
 * served is what runs with the device up: NULL for the medium's own loader,
 * which is what iso_boot_overlaid does by itself, and the blessed launch
 * otherwise. Everything before that point is the same work either way, and
 * saying it twice is how the two would drift.
 */
static EFI_STATUS bsd_run(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                          fs_vol *vol, const fs_dirent *file,
                          iso_served_fn served, void *ctx,
                          const CHAR16 *handoff, bsd_result *result)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    bsdmd_record_edit rec;
    char path[BSDMD_PATH_MAX];
    char head[CONF_HEAD];
    char *cfg = (char *)0;
    UINT8 *recbuf = (UINT8 *)0;
    uint32_t cfg_len = 0;
    fs_dirent conf;
    fs_file conf_file;
    fs_extent conf_ext;
    size_t next = 0;
    uint64_t head_len;
    iso_overlay ovl;
    iso_boot_result ir = { L"starting", EFI_SUCCESS,
                           (const family_desc *)0 };
    EFI_STATUS status;

    result->stage = L"opening the ISO";
    result->status = EFI_SUCCESS;
    result->not_bsd = 1;
    result->advice = (const CHAR16 *)0;
    memset(&ovl, 0, sizeof ovl);

    if (fs_file_open(vol, &g_iso, file) != FS_OK)
        return result->status = EFI_DEVICE_ERROR;

    result->stage = L"reading the ISO's filesystem";
    if (fs_mount(&g_isovol, &fs_ops_iso9660, fs_file_read_cb, &g_iso) !=
        FS_OK)
        return result->status = EFI_UNSUPPORTED;

    /* Where the settings go, and how much room there is for them. The file
     * has to be one contiguous run so a single override can replace its
     * head, which every medium met writes it as. */
    result->stage = L"locating the ISO's loader.conf";
    if (fs_lookup(&g_isovol, LOADER_CONF, &g_dir, &conf) != FS_OK ||
        conf.is_dir || conf.size < CONF_HEAD ||
        fs_extents(&g_isovol, &conf, &conf_ext, 1, &next) != FS_OK ||
        next != 1 || conf_ext.len != conf.size)
        return result->status = EFI_UNSUPPORTED;
    head_len = CONF_HEAD;
    if (fs_file_open(&g_isovol, &conf_file, &conf) != FS_OK ||
        fs_file_read(&conf_file, 0, head_len, head) != FS_OK)
        return result->status = EFI_DEVICE_ERROR;

    /* The record naming the medium as a file inside itself. */
    result->stage = L"placing the memory-disk record";
    if (bsdmd_record(file->size, iso_read, (void *)0, &rec, path,
                     sizeof path) != BSDMD_OK) {
        result->advice = L"this ISO has no free bytes in its root or /boot "
                         L"directory for the record this route adds.";
        return result->status = EFI_UNSUPPORTED;
    }

    /* From here the medium is one this route applies to, so a failure is a
     * real failure rather than a cue to chainload. */
    result->not_bsd = 0;

    result->stage = L"checking memory for the image";
    if (!staging_fits(bs, file->size)) {
        result->advice = L"this machine has too little memory below 4 GB to "
                         L"hold the image, which is where FreeBSD's loader "
                         L"has to put it. Press c to boot the ISO's own "
                         L"loader instead.";
        return result->status = EFI_OUT_OF_RESOURCES;
    }

    result->stage = L"writing the loader settings";
    if (bs->AllocatePool(EfiLoaderData, BSDMD_CFG_MAX, (void **)&cfg) !=
            EFI_SUCCESS ||
        bs->AllocatePool(EfiLoaderData, BSDMD_REC_MAX, (void **)&recbuf) !=
            EFI_SUCCESS) {
        status = EFI_OUT_OF_RESOURCES;
        goto out;
    }
    if (bsdmd_config(path, head, (size_t)head_len, cfg, BSDMD_CFG_MAX,
                     &cfg_len) != BSDMD_OK) {
        result->advice = L"this ISO's loader.conf has no comment block long "
                         L"enough to hold the memory-disk settings.";
        status = EFI_UNSUPPORTED;
        goto out;
    }
    memcpy(recbuf, rec.bytes, rec.len);

    ovl.ovl[0].iso_off = rec.off;
    ovl.ovl[0].len = rec.len;
    ovl.ovl[0].mem = recbuf;
    ovl.ovl[1].iso_off = conf_ext.vol_off;
    ovl.ovl[1].len = cfg_len;
    ovl.ovl[1].mem = (const UINT8 *)cfg;
    ovl.n = 2;

    /* Not "starting": the ISO's loader reads the whole medium out of the
     * virtual CD after this line, and the coverage harness counts media read
     * after "starting" as the kernel's (tools/coverage/run-case.sh). */
    menu_out(st, handoff);
    status = iso_boot_overlaid_served(st, parent_image, vol, file,
                                      ISO_MEDIA_CD, &ovl, served, ctx, &ir);
    result->stage = ir.stage;
    result->status = ir.status;

out:
    if (cfg != (char *)0)
        bs->FreePool(cfg);
    if (recbuf != (UINT8 *)0)
        bs->FreePool(recbuf);
    if (EFI_ERROR(status))
        result->status = status;
    return status;
}

EFI_STATUS bsd_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                    fs_vol *vol, const fs_dirent *file, bsd_result *result)
{
    return bsd_run(st, parent_image, vol, file, (iso_served_fn)0, (void *)0,
                   L"handing the ISO to its own loader, as a memory disk\r\n",
                   result);
}

/* ---- the blessed launch ---- */

typedef struct {
    const bless_hit *hit;
    bless_result    *bres;
} blessed_ctx;

static EFI_STATUS serve_blessed(void *ctx, EFI_SYSTEM_TABLE *st,
                                EFI_HANDLE parent_image,
                                EFI_HANDLE esp_handle,
                                iso_boot_result *ir)
{
    blessed_ctx *c = (blessed_ctx *)ctx;
    EFI_DEVICE_PATH_PROTOCOL *path = (EFI_DEVICE_PATH_PROTOCOL *)0;
    EFI_STATUS status;

    /* The signed loader is named to the platform as the medium's own
     * \\EFI\\BOOT\\BOOTX64.EFI, which is what makes the medium the device
     * it believes it came from: FreeBSD's loader works currdev out from
     * there, and everything it then reads - its Lua, its kernel, the memory
     * disk - is on that medium. Its BYTES are the signed copy off p2, and
     * those are what the platform verifies. Measured 2026-09-19: published
     * on the synthetic filesystem instead, the same loader gets as far as
     * "Failed to find bootable partition" and stops. */
    ir->stage = L"naming the blessed loader as the medium's";
    status = iso_boot_path(st->BootServices, esp_handle, &path);
    if (EFI_ERROR(status))
        return status;
    status = bless_boot_as(st, parent_image, c->hit, path, c->bres);
    st->BootServices->FreePool(path);

    /* The medium's result fields are what iso.c reports about the launch;
     * the caller reads the blessed ones out of *bres. */
    ir->stage = c->bres->stage;
    ir->status = c->bres->status;
    return status;
}

EFI_STATUS bsd_boot_blessed(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                            fs_vol *vol, const fs_dirent *file,
                            const bless_hit *hit, bless_result *bres,
                            bsd_result *result)
{
    blessed_ctx ctx;

    ctx.hit = hit;
    ctx.bres = bres;
    return bsd_run(st, parent_image, vol, file, serve_blessed, &ctx,
                   L"handing the ISO to its blessed loader, as a memory "
                   L"disk\r\n", result);
}
