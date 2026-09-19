/* Direct kernel boot of an ISO on p2. See falcon-efi/src/direct/direct.h. */

#include <direct/direct.h>

#include <efi/devpath.h>
#include <efi/loadedimage.h>
#include <efi/loadfile2.h>
#include <loader/loadops.h>
#include <loader/credfs.h>
#include <console/menu.h>

#include <pe/pe.h>
#include <media/bootfs.h>
#include <media/extmap.h>
#include <media/hookcmd.h>
#include <media/grubcfg.h>
#include <iso/iso.h>
#include <lib/string.h>

/* The hook, as make embeds it (falcon-hook/build.mk). */
extern const unsigned char falcon_hook_bin[];
extern const size_t falcon_hook_bin_len;

/* What bootcfg may read a configuration file into: the sign tool's figure
 * (falcon-cli/src/sign/main.c), since both read the same files. */
#define CFG_SCRATCH ((UINTN)512u * 1024u)

/* Everything here is file scope rather than stack: the handles and iterators
 * are kilobytes each, the result table tens of kilobytes. One direct boot
 * runs at a time. */
static fs_file g_iso;
static fs_vol g_isovol;
static bootfs_ctx g_bctx;
static bootcfg_result g_cfg;
static fs_file g_part;
static fs_extent g_ext[EXTMAP_MAX_EXTENTS];
static extmap_extent g_mext[EXTMAP_MAX_EXTENTS];
static UINT8 g_map[EXTMAP_HDR_LEN + EXTMAP_MAX_EXTENTS * EXTMAP_EXT_LEN];
static char g_cmd[BOOTCFG_CMDLINE_MAX];
static char g_hooked[BOOTCFG_CMDLINE_MAX + 128u];
static CHAR16 g_opts[BOOTCFG_CMDLINE_MAX + 160u];

/* The ISO's own shim, for DIRECT_VIA_ISO_SHIM (direct.h), and the names the
 * two files have on the synthetic filesystem it is loaded from. The kernel
 * sits beside it because shim reads its second stage from the device it was
 * itself loaded from. */
#define ISO_SHIM_PATH "/EFI/BOOT/BOOTX64.EFI"
static const CHAR16 g_shim_name[] = L"shimx64.efi";
static const CHAR16 g_shim_load[] = L"\\shimx64.efi";
static const CHAR16 g_kernel_name[] = L"vmlinuz";
static const CHAR16 g_kernel_arg[] = L"\\vmlinuz ";

/* The LoadFile2 provider and the one initrd it serves. The device path is the
 * single vendor node the kernel's stub looks for, then the end node. */
typedef struct {
    VENDOR_DEVICE_PATH       vendor;
    EFI_DEVICE_PATH_PROTOCOL end;
} initrd_path;

static struct {
    EFI_LOAD_FILE2_PROTOCOL lf2;
    initrd_path             path;
    const UINT8            *data;
    UINTN                   size;
    EFI_HANDLE              handle;
} g_initrd;

static EFI_STATUS EFIAPI initrd_load(EFI_LOAD_FILE2_PROTOCOL *self,
                                     EFI_DEVICE_PATH_PROTOCOL *path,
                                     BOOLEAN boot_policy, UINTN *size,
                                     void *buf)
{
    (void)path;
    if (self != &g_initrd.lf2 || size == (UINTN *)0)
        return EFI_INVALID_PARAMETER;
    /* LoadFile2 never serves a boot option (spec 13.2). */
    if (boot_policy)
        return EFI_UNSUPPORTED;
    if (buf == (void *)0 || *size < g_initrd.size) {
        *size = g_initrd.size;
        return EFI_BUFFER_TOO_SMALL;
    }
    memcpy(buf, g_initrd.data, g_initrd.size);
    *size = g_initrd.size;
    return EFI_SUCCESS;
}

static EFI_STATUS initrd_publish(EFI_BOOT_SERVICES *bs, const UINT8 *data,
                                 UINTN size)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID lf2_guid = EFI_LOAD_FILE2_PROTOCOL_GUID;
    EFI_GUID media = LINUX_EFI_INITRD_MEDIA_GUID;

    g_initrd.lf2.LoadFile = initrd_load;
    g_initrd.path.vendor.Header.Type = MEDIA_DEVICE_PATH;
    g_initrd.path.vendor.Header.SubType = MEDIA_VENDOR_DP;
    dp_set_node_len(&g_initrd.path.vendor.Header,
                    (UINT16)sizeof(g_initrd.path.vendor));
    g_initrd.path.vendor.Guid = media;
    dp_set_end(&g_initrd.path.end);
    g_initrd.data = data;
    g_initrd.size = size;
    g_initrd.handle = (EFI_HANDLE)0;
    /* EFI_ALREADY_STARTED here means another provider already answers for
     * the initrd (a firmware or an earlier loader that did not clean up), and
     * the kernel would take that one's bytes; refusing is the only way to
     * know which initrd boots. */
    return bs->InstallMultipleProtocolInterfaces(&g_initrd.handle,
                                                 &dp_guid, &g_initrd.path,
                                                 &lf2_guid, &g_initrd.lf2,
                                                 (void *)0);
}

static void initrd_withdraw(EFI_BOOT_SERVICES *bs)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID lf2_guid = EFI_LOAD_FILE2_PROTOCOL_GUID;

    if (g_initrd.handle == (EFI_HANDLE)0)
        return;
    (void)bs->UninstallMultipleProtocolInterfaces(g_initrd.handle,
                                                  &dp_guid, &g_initrd.path,
                                                  &lf2_guid, &g_initrd.lf2,
                                                  (void *)0);
    g_initrd.handle = (EFI_HANDLE)0;
}

/* Page allocations for the kernel and the initrd: tens of megabytes each,
 * which is what pages are for. */
typedef struct {
    EFI_PHYSICAL_ADDRESS addr;
    UINTN                pages;
} pages;

static UINT8 *pages_get(EFI_BOOT_SERVICES *bs, pages *p, UINT64 size)
{
    p->pages = (UINTN)((size + 4095u) / 4096u);
    p->addr = 0;
    if (p->pages == 0)
        p->pages = 1;
    if (bs->AllocatePages(AllocateAnyPages, EfiLoaderData, p->pages,
                          &p->addr) != EFI_SUCCESS) {
        p->pages = 0;
        return (UINT8 *)0;
    }
    return (UINT8 *)(uintptr_t)p->addr;
}

static void pages_put(EFI_BOOT_SERVICES *bs, pages *p)
{
    if (p->pages != 0)
        (void)bs->FreePages(p->addr, p->pages);
    p->pages = 0;
}

/* The ISO's filesystem: ISO9660 first, whose Rock Ridge names are the ones a
 * Linux configuration spells, then UDF for an image that has nothing else. */
static EFI_STATUS mount_iso(void)
{
    if (fs_mount(&g_isovol, &fs_ops_iso9660, fs_file_read_cb, &g_iso) == FS_OK)
        return EFI_SUCCESS;
    if (fs_mount(&g_isovol, &fs_ops_udf, fs_file_read_cb, &g_iso) == FS_OK)
        return EFI_SUCCESS;
    return EFI_UNSUPPORTED;
}

/* The file at path read whole into pages. */
static EFI_STATUS read_whole(EFI_BOOT_SERVICES *bs, const char *path,
                             pages *p, UINT8 **out, UINT64 *size)
{
    UINT8 *buf;

    if (bootfs_open(&g_bctx, path, &g_part, size) != FS_OK)
        return EFI_NOT_FOUND;
    buf = pages_get(bs, p, *size);
    if (buf == (UINT8 *)0)
        return EFI_OUT_OF_RESOURCES;
    if (*size != 0 && fs_file_read(&g_part, 0, *size, buf) != FS_OK) {
        pages_put(bs, p);
        return EFI_DEVICE_ERROR;
    }
    *out = buf;
    return EFI_SUCCESS;
}

static UINT64 align4(UINT64 v)
{
    return (v + 3u) & ~(UINT64)3u;
}

/* The entry's initrds, each padded to 4 bytes as a boot loader concatenating
 * them does, then the hook's archive with the extent map in it. The kernel's unpacker takes the
 * concatenation as one initramfs, later archives overlaying earlier ones. */
static EFI_STATUS build_initrd(EFI_BOOT_SERVICES *bs, const bootcfg_entry *e,
                               UINTN map_len, pages *p, UINT8 **out,
                               UINT64 *size)
{
    UINT64 isz[BOOTCFG_INITRD_MAX];
    UINT64 total = 0, off = 0;
    size_t csize = hookcmd_archive_size(falcon_hook_bin_len, map_len);
    size_t wrote = 0;
    UINT8 *buf;
    UINTN i;

    if (csize == 0)
        return EFI_LOAD_ERROR;

    for (i = 0; i < e->n_initrd; i++) {
        if (bootfs_open(&g_bctx, e->initrd[i], &g_part, &isz[i]) != FS_OK)
            return EFI_NOT_FOUND;
        total += align4(isz[i]);
    }
    total += csize;

    buf = pages_get(bs, p, total);
    if (buf == (UINT8 *)0)
        return EFI_OUT_OF_RESOURCES;
    for (i = 0; i < e->n_initrd; i++) {
        if (bootfs_open(&g_bctx, e->initrd[i], &g_part, &isz[i]) != FS_OK ||
            (isz[i] != 0 &&
             fs_file_read(&g_part, 0, isz[i], buf + off) != FS_OK)) {
            pages_put(bs, p);
            return EFI_DEVICE_ERROR;
        }
        off += isz[i];
        while ((off & 3u) != 0)
            buf[off++] = 0;
    }
    if (hookcmd_archive(falcon_hook_bin, falcon_hook_bin_len, g_map, map_len,
                        buf + off, (size_t)(total - off), &wrote) !=
        HOOKCMD_OK) {
        pages_put(bs, p);
        return EFI_LOAD_ERROR;
    }
    *out = buf;
    *size = off + wrote;
    return EFI_SUCCESS;
}

/* The Hard Drive node of p2's device path: the partition's unique GUID and
 * its first LBA, which is how the hook finds the same partition from Linux. */
static EFI_STATUS part_node(EFI_BOOT_SERVICES *bs, EFI_HANDLE h,
                            extmap_hdr *hdr)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *dp;
    const UINT8 *p;

    if (bs->HandleProtocol(h, &dp_guid, (void **)&dp) != EFI_SUCCESS)
        return EFI_NOT_FOUND;
    p = (const UINT8 *)dp;
    for (;;) {
        const EFI_DEVICE_PATH_PROTOCOL *n = (const EFI_DEVICE_PATH_PROTOCOL *)p;
        UINT16 len = dp_node_len(n);

        if (dp_is_end(n) || len < sizeof(*n))
            return EFI_NOT_FOUND;
        if (n->Type == MEDIA_DEVICE_PATH && n->SubType == MEDIA_HARDDRIVE_DP &&
            len >= HD_DP_LEN) {
            if (p[HD_DP_OFF_SIGTYPE] != HD_DP_SIGTYPE_GUID)
                return EFI_UNSUPPORTED;
            memcpy(&hdr->part_lba, p + HD_DP_OFF_START, sizeof hdr->part_lba);
            memcpy(hdr->part_guid, p + HD_DP_OFF_SIG, sizeof hdr->part_guid);
            return EFI_SUCCESS;
        }
        p += len;
    }
}

/* Where the ISO sits on p2, encoded for the hook. */
static EFI_STATUS build_map(EFI_BOOT_SERVICES *bs, fs_vol *p2,
                            EFI_HANDLE p2_handle, UINT32 block_size,
                            const fs_dirent *iso, UINTN *len,
                            direct_result *r)
{
    extmap_hdr hdr;
    size_t n = 0, i, mlen = 0;
    int rc;

    memset(&hdr, 0, sizeof hdr);
    r->stage = L"finding p2's GPT entry";
    if (EFI_ERROR(part_node(bs, p2_handle, &hdr))) {
        r->advice = L"the ISO's partition has no GPT entry this loader can "
                    L"name to Linux.";
        return EFI_UNSUPPORTED;
    }
    hdr.block_size = block_size;
    hdr.iso_size = iso->size;

    r->stage = L"mapping the ISO's extents on p2";
    rc = fs_extents(p2, iso, g_ext, EXTMAP_MAX_EXTENTS, &n);
    if (rc == FS_ERR_NOSPACE) {
        r->advice = L"the ISO is in too many pieces on the stick. Copy it "
                    L"again, onto a stick with more free space.";
        return EFI_BUFFER_TOO_SMALL;
    }
    if (rc != FS_OK)
        return EFI_DEVICE_ERROR;
    for (i = 0; i < n; i++) {
        g_mext[i].offset = g_ext[i].vol_off;
        g_mext[i].length = g_ext[i].len;
    }
    hdr.n_extents = (uint32_t)n;
    if (extmap_encode(&hdr, g_mext, g_map, sizeof g_map, &mlen) != EXTMAP_OK)
        return EFI_LOAD_ERROR;
    *len = (UINTN)mlen;
    return EFI_SUCCESS;
}

EFI_STATUS direct_map_set(EFI_SYSTEM_TABLE *st, fs_vol *p2,
                          EFI_HANDLE p2_handle, UINT32 block_size,
                          const fs_dirent *iso, const UINT8 **map,
                          UINTN *len)
{
    direct_result r;
    UINTN n = 0;
    EFI_STATUS status;

    if (map != (const UINT8 **)0)
        *map = (const UINT8 *)0;
    if (len != (UINTN *)0)
        *len = 0;
    r.advice = (const CHAR16 *)0;
    status = build_map(st->BootServices, p2, p2_handle, block_size, iso, &n,
                       &r);
    if (EFI_ERROR(status))
        return status;
    if (map != (const UINT8 **)0)
        *map = g_map;
    if (len != (UINTN *)0)
        *len = n;
    return loader_isomap_set(st, g_map, n);
}

/* Reads the configuration and settles the cmdline. */
static EFI_STATUS pick_entry(direct_result *r, const bootcfg_entry **out,
                             char *scratch)
{
    bootcfg_src src;
    const bootcfg_entry *e;
    UINTN i;

    r->stage = L"reading the ISO's boot configuration";
    bootfs_src(&g_bctx, &g_isovol, &src);
    if (bootcfg_read(&src, BOOTCFG_CD, scratch, CFG_SCRATCH, &g_cfg) !=
            BOOTCFG_OK ||
        g_cfg.pick >= g_cfg.n) {
        r->no_entry = 1;
        return EFI_NOT_FOUND;
    }
    e = &g_cfg.entry[g_cfg.pick];
    for (i = 0; i + 1 < sizeof r->title && e->title[i] != '\0'; i++)
        r->title[i] = e->title[i];
    r->title[i] = '\0';

    r->stage = L"building the cmdline";
    if (bootcfg_render(e, (const char *)0, g_cmd, sizeof g_cmd) != BOOTCFG_OK ||
        hookcmd_build(g_cmd, g_hooked, sizeof g_hooked) != HOOKCMD_OK)
        return EFI_BUFFER_TOO_SMALL;
    for (i = 0; g_hooked[i] != '\0'; i++)
        g_opts[i] = (CHAR16)(unsigned char)g_hooked[i];
    g_opts[i] = 0;
    *out = e;
    return EFI_SUCCESS;
}

EFI_STATUS direct_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent, fs_vol *p2,
                       EFI_HANDLE p2_handle, UINT32 block_size,
                       const fs_dirent *iso, direct_via via,
                       direct_result *r)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    const bootcfg_entry *e;
    pages kp = { 0, 0 }, ip = { 0, 0 }, sp = { 0, 0 };
    UINT8 *kernel = (UINT8 *)0, *initrd = (UINT8 *)0, *shim = (UINT8 *)0;
    UINT64 ksize = 0, isize = 0, ssize = 0;
    EFI_DEVICE_PATH_PROTOCOL *shim_path = (EFI_DEVICE_PATH_PROTOCOL *)0;
    BOOLEAN published = FALSE;
    UINTN map_len = 0, exit_size = 0, i;
    CHAR16 *exit_data = (CHAR16 *)0;
    char *scratch = (char *)0;
    EFI_HANDLE child = (EFI_HANDLE)0;
    load_ops ops;
    EFI_STATUS status;

    r->stage = L"opening the ISO";
    r->status = EFI_SUCCESS;
    r->no_entry = 0;
    r->title[0] = '\0';
    r->advice = (const CHAR16 *)0;

    if (fs_file_open(p2, &g_iso, iso) != FS_OK)
        return r->status = EFI_DEVICE_ERROR;
    r->stage = L"reading the ISO's filesystem";
    if (EFI_ERROR(mount_iso())) {
        r->no_entry = 1;
        return r->status = EFI_UNSUPPORTED;
    }

    if (bs->AllocatePool(EfiLoaderData, CFG_SCRATCH, (void **)&scratch) !=
        EFI_SUCCESS)
        return r->status = EFI_OUT_OF_RESOURCES;
    status = pick_entry(r, &e, scratch);
    if (EFI_ERROR(status))
        goto out;

    status = build_map(bs, p2, p2_handle, block_size, iso, &map_len, r);
    if (EFI_ERROR(status))
        goto out;

    r->stage = L"reading the kernel";
    status = read_whole(bs, e->kernel, &kp, &kernel, &ksize);
    if (EFI_ERROR(status))
        goto out;
    r->stage = L"reading the initrd";
    status = build_initrd(bs, e, map_len, &ip, &initrd, &isize);
    if (EFI_ERROR(status))
        goto out;
    if (via == DIRECT_VIA_ISO_SHIM) {
        pe_hdr ph;
        size_t so = 0, sl = 0;

        /* Shim runs a second stage only if it carries SBAT metadata (shim
         * 15.7 and later, under Secure Boot). Fedora's kernels do; Debian's
         * and Ubuntu's do not, because there GRUB is the second stage and
         * checks the kernel through shim's verify protocol instead. A kernel
         * without it is not this route's to boot: the caller chainloads. */
        r->stage = L"checking the kernel for SBAT metadata";
        if (pe_parse(kernel, (size_t)ksize, &ph) != PE_OK ||
            pe_find_section(&ph, kernel, (size_t)ksize, ".sbat", &so, &sl) !=
                PE_OK) {
            r->no_entry = 1;
            status = EFI_UNSUPPORTED;
            goto out;
        }
        r->stage = L"reading the ISO's shim";
        status = read_whole(bs, ISO_SHIM_PATH, &sp, &shim, &ssize);
        if (status == EFI_NOT_FOUND)
            r->no_entry = 1;
        if (EFI_ERROR(status))
            goto out;
    }

    r->stage = L"setting FalconIsoMap";
    status = loader_isomap_set(st, g_map, map_len);
    if (EFI_ERROR(status))
        goto out;
    r->stage = L"publishing the initrd";
    status = initrd_publish(bs, initrd, (UINTN)isize);
    if (EFI_ERROR(status))
        goto out;

    resolve_load_ops(bs, &ops);
    if (via == DIRECT_VIA_ISO_SHIM) {
        /* The ISO's shim, by device path off the synthetic filesystem, with
         * the kernel beside it: shim verifies it against its vendor's
         * certificate and runs it as its second stage, named first in the
         * load options with the cmdline after it. */
        credfs_entry tree[2];

        tree[0].name = g_shim_name;
        tree[0].data = shim;
        tree[0].size = ssize;
        tree[0].parent = CREDFS_ROOT;
        tree[0].is_dir = 0;
        tree[0].secret = 0;
        tree[1].name = g_kernel_name;
        tree[1].data = kernel;
        tree[1].size = ksize;
        tree[1].parent = CREDFS_ROOT;
        tree[1].is_dir = 0;
        tree[1].secret = 0;
        r->stage = L"publishing the ISO's shim and kernel";
        status = credfs_publish(st, tree, 2);
        if (EFI_ERROR(status))
            goto out;
        published = TRUE;
        status = credfs_load_path(bs, g_shim_load, &shim_path);
        if (EFI_ERROR(status))
            goto out;
        r->stage = L"LoadImage of the ISO's shim";
        status = ops.LoadImage(FALSE, parent, shim_path, (void *)0, 0,
                               &child);
    } else {
        r->stage = ops.via_shim ? L"LoadImage of the kernel (shim)"
                                : L"LoadImage of the kernel (firmware)";
        status = ops.LoadImage(FALSE, parent, (EFI_DEVICE_PATH_PROTOCOL *)0,
                               kernel, (UINTN)ksize, &child);
    }
    if (EFI_ERROR(status)) {
        /* A refused image is still loaded (spec 7.4), and only the loader
         * that loaded it may unload it (falcon-efi/src/loader/loadops.h). */
        if (child != (EFI_HANDLE)0)
            ops.UnloadImage(child);
        if (status == EFI_SECURITY_VIOLATION)
            r->advice = L"the platform refused this ISO's kernel. Bless the "
                        L"ISO, or press c on its row to boot its own loader.";
        goto out;
    }

    r->stage = L"setting the cmdline";
    if (bs->HandleProtocol(child, &li_guid, (void **)&li) != EFI_SUCCESS) {
        ops.UnloadImage(child);
        status = EFI_LOAD_ERROR;
        goto out;
    }
    if (via == DIRECT_VIA_ISO_SHIM) {
        /* Shifted right by the second stage's name, in place. */
        UINTN k = 0, len;

        for (len = 0; g_opts[len] != 0; len++)
            ;
        while (g_kernel_arg[k] != 0)
            k++;
        memmove(g_opts + k, g_opts, (len + 1u) * sizeof(CHAR16));
        memcpy(g_opts, g_kernel_arg, k * sizeof(CHAR16));
    }
    for (i = 0; g_opts[i] != 0; i++)
        ;
    li->LoadOptions = g_opts;
    li->LoadOptionsSize = (UINT32)((i + 1u) * sizeof(CHAR16));

    menu_out(st, L"starting ");
    {
        CHAR16 t[BOOTCFG_TITLE_MAX];

        for (i = 0; r->title[i] != '\0'; i++)
            t[i] = (CHAR16)(unsigned char)r->title[i];
        t[i] = 0;
        menu_out(st, t);
    }
    menu_out(st, L"\r\n");

    r->stage = L"StartImage";
    status = ops.StartImage(child, &exit_size, &exit_data);
    if (exit_data != (CHAR16 *)0)
        bs->FreePool(exit_data);
    bs->SetWatchdogTimer(0, 0, 0, (CHAR16 *)0);
    r->status = status;
    status = EFI_SUCCESS;

out:
    if (shim_path != (EFI_DEVICE_PATH_PROTOCOL *)0)
        bs->FreePool(shim_path);
    if (published)
        (void)credfs_teardown(bs);
    pages_put(bs, &sp);
    initrd_withdraw(bs);
    loader_isomap_clear(st);
    pages_put(bs, &ip);
    pages_put(bs, &kp);
    if (scratch != (char *)0)
        bs->FreePool(scratch);
    if (EFI_ERROR(status))
        r->status = status;
    return status;
}

/* ---- D3b: the ISO's own GRUB with a configuration of ours ---- */

/* The file GRUB reads, no larger than this: the one piece of the medium
 * that is replaced wholesale, in memory. */
#define GRUBCFG_MAX ((UINTN)256u * 1024u)
#define ISO_BLOCK 2048u

static fs_dir g_lookdir;

EFI_STATUS direct_grub_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent,
                            fs_vol *p2, EFI_HANDLE p2_handle,
                            UINT32 block_size, const fs_dirent *iso,
                            direct_result *r)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    const bootcfg_entry *e;
    pages cp = { 0, 0 }, hp = { 0, 0 };
    UINT8 *cfg = (UINT8 *)0, *hook = (UINT8 *)0;
    UINTN map_len = 0;
    size_t hook_len = 0, hook_cap, next = 0;
    UINT64 hook_blocks;
    char *scratch = (char *)0;
    fs_dirent cfg_ent;
    fs_extent cfg_ext[2];
    iso_overlay ovl;
    iso_boot_result ir = { L"starting", EFI_SUCCESS,
                           (const family_desc *)0 };
    EFI_STATUS status;
    UINTN i;

    r->stage = L"opening the ISO";
    r->status = EFI_SUCCESS;
    r->no_entry = 1;
    r->title[0] = '\0';
    r->advice = (const CHAR16 *)0;
    memset(&ovl, 0, sizeof ovl);

    /* The hook's archive goes after the ISO's last byte, which is a block
     * boundary on every medium the virtual CD serves. */
    if (iso->size % ISO_BLOCK != 0)
        return r->status = EFI_UNSUPPORTED;
    if (fs_file_open(p2, &g_iso, iso) != FS_OK)
        return r->status = EFI_DEVICE_ERROR;
    r->stage = L"reading the ISO's filesystem";
    if (EFI_ERROR(mount_iso()))
        return r->status = EFI_UNSUPPORTED;
    if (bs->AllocatePool(EfiLoaderData, CFG_SCRATCH, (void **)&scratch) !=
        EFI_SUCCESS)
        return r->status = EFI_OUT_OF_RESOURCES;
    status = pick_entry(r, &e, scratch);
    if (EFI_ERROR(status))
        goto out;

    /* The file replaced is the grub.cfg the ISO's GRUB reads: the one the
     * entry came from when it is one, else the first GRUB looks for (the
     * entry's kernel and initrds are the same files whichever format named
     * them). It has to be one contiguous file this driver can place. */
    status = EFI_UNSUPPORTED;
    r->no_entry = 1;
    r->stage = L"locating the ISO's grub.cfg";
    if (e->loop)
        goto out;
    {
        const char *s = g_cfg.source;
        const char *const *gp = bootcfg_grub_cd_paths();
        UINTN n = 0;
        int found;

        while (s[n] != '\0')
            n++;
        found = n >= 9 && memcmp(s + n - 9, "/grub.cfg", 9) == 0 &&
                fs_lookup(&g_isovol, s, &g_lookdir, &cfg_ent) == FS_OK;
        for (; !found && *gp != (const char *)0; gp++)
            found = fs_lookup(&g_isovol, *gp, &g_lookdir, &cfg_ent) == FS_OK;
        if (!found)
            goto out;
    }
    if (cfg_ent.is_dir || cfg_ent.size == 0 || cfg_ent.size > GRUBCFG_MAX ||
        fs_extents(&g_isovol, &cfg_ent, cfg_ext, 2, &next) != FS_OK ||
        next != 1 || cfg_ext[0].len != cfg_ent.size)
        goto out;

    status = build_map(bs, p2, p2_handle, block_size, iso, &map_len, r);
    if (EFI_ERROR(status))
        goto out;

    r->stage = L"building the hook's archive";
    hook_cap = hookcmd_archive_size(falcon_hook_bin_len, map_len);
    hook_blocks = ((UINT64)hook_cap + ISO_BLOCK - 1u) / ISO_BLOCK;
    hook = pages_get(bs, &hp, hook_blocks * ISO_BLOCK);
    cfg = pages_get(bs, &cp, cfg_ent.size);
    if (hook == (UINT8 *)0 || cfg == (UINT8 *)0) {
        status = EFI_OUT_OF_RESOURCES;
        goto out;
    }
    memset(hook, 0, (UINTN)(hook_blocks * ISO_BLOCK));
    if (hookcmd_archive(falcon_hook_bin, falcon_hook_bin_len, g_map, map_len,
                        hook, hook_cap, &hook_len) != HOOKCMD_OK) {
        status = EFI_LOAD_ERROR;
        goto out;
    }

    /* A grub.cfg too short to hold ours (Alpine's is 182 bytes) leaves
     * nothing this route can serve. */
    r->stage = L"writing the grub.cfg";
    if (grubcfg_write(e, g_hooked, iso->size / 512u,
                      ((UINT64)hook_len + 511u) / 512u, (size_t)cfg_ent.size,
                      (char *)cfg, (size_t)cfg_ent.size) != GRUBCFG_OK) {
        status = EFI_UNSUPPORTED;
        goto out;
    }

    ovl.ovl[0].iso_off = cfg_ext[0].vol_off;
    ovl.ovl[0].len = cfg_ent.size;
    ovl.ovl[0].mem = cfg;
    ovl.ovl[1].iso_off = iso->size;
    ovl.ovl[1].len = hook_blocks * ISO_BLOCK;
    ovl.ovl[1].mem = hook;
    ovl.n = 2;
    ovl.extra = hook_blocks * ISO_BLOCK;

    /* From here the ISO has a configuration this route can serve; a
     * failure is a real one. */
    r->no_entry = 0;
    r->stage = L"setting FalconIsoMap";
    status = loader_isomap_set(st, g_map, map_len);
    if (EFI_ERROR(status))
        goto out;

    /* Not "starting": GRUB reads the kernel and initrds after this line,
     * and the coverage harness counts media read after "starting" as the
     * kernel's (tools/coverage/run-case.sh). */
    menu_out(st, L"handing ");
    {
        CHAR16 t[BOOTCFG_TITLE_MAX];

        for (i = 0; r->title[i] != '\0'; i++)
            t[i] = (CHAR16)(unsigned char)r->title[i];
        t[i] = 0;
        menu_out(st, t);
    }
    menu_out(st, L" to the ISO's own GRUB\r\n");
    status = iso_boot_overlaid(st, parent, p2, iso, ISO_MEDIA_CD, &ovl, &ir);
    r->stage = ir.stage;
    r->status = ir.status;

out:
    loader_isomap_clear(st);
    pages_put(bs, &cp);
    pages_put(bs, &hp);
    if (scratch != (char *)0)
        bs->FreePool(scratch);
    if (EFI_ERROR(status))
        r->status = status;
    return status;
}
