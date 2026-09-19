/* TRACE BUILD ONLY (branch trace/esxi): logs the boot services an ISO's
 * payload calls, to the console (serial in the rig), so a payload that
 * fails inside our chain and works on a bare CD can be compared call by
 * call. Never for master. */

#include <iso/bstrace.h>

#include <lib/string.h>
#include <efi/loadedimage.h>

typedef EFI_STATUS (EFIAPI *load_fn)(BOOLEAN, EFI_HANDLE,
                                     EFI_DEVICE_PATH_PROTOCOL *, void *,
                                     UINTN, EFI_HANDLE *);
typedef EFI_STATUS (EFIAPI *start_fn)(EFI_HANDLE, UINTN *, CHAR16 **);
typedef EFI_STATUS (EFIAPI *connect_fn)(EFI_HANDLE, EFI_HANDLE *,
                                        EFI_DEVICE_PATH_PROTOCOL *, BOOLEAN);
typedef EFI_STATUS (EFIAPI *locdp_fn)(EFI_GUID *, EFI_DEVICE_PATH_PROTOCOL **,
                                      EFI_HANDLE *);
typedef EFI_STATUS (EFIAPI *open_fn)(EFI_HANDLE, EFI_GUID *, void **,
                                     EFI_HANDLE, EFI_HANDLE, UINT32);
typedef EFI_STATUS (EFIAPI *locp_fn)(EFI_GUID *, void *, void **);
typedef EFI_STATUS (EFIAPI *crc_fn)(void *, UINTN, UINT32 *);
typedef EFI_STATUS (EFIAPI *disc_fn)(EFI_HANDLE, EFI_HANDLE, EFI_HANDLE);

typedef struct {
    CHAR16 *(EFIAPI *node)(void *, BOOLEAN, BOOLEAN);
    CHAR16 *(EFIAPI *path)(EFI_DEVICE_PATH_PROTOCOL *, BOOLEAN, BOOLEAN);
} dp_to_text;

static EFI_SYSTEM_TABLE *g_st;
static load_fn g_load;
static start_fn g_start;
static connect_fn g_connect;
static locdp_fn g_locdp;
static open_fn g_open;
static locp_fn g_locp;
static disc_fn g_disc;
static int g_busy;
static int g_on;

static void out(const CHAR16 *s)
{
    g_st->ConOut->OutputString(g_st->ConOut, (CHAR16 *)(uintptr_t)s);
}

static void hex(UINT64 v)
{
    CHAR16 b[19];
    int i;

    b[0] = L'0';
    b[1] = L'x';
    for (i = 0; i < 16; i++) {
        unsigned d = (unsigned)((v >> (60 - 4 * i)) & 0xfu);

        b[2 + i] = (CHAR16)(d < 10 ? L'0' + d : L'a' + d - 10);
    }
    b[18] = 0;
    out(b);
}

static void guid(const EFI_GUID *g)
{
    out(L"{");
    hex(g == (const EFI_GUID *)0 ? 0 : *(const UINT32 *)(const void *)g);
    out(L"}");
}

static void dpath(EFI_DEVICE_PATH_PROTOCOL *p)
{
    EFI_GUID tg = { 0x8b843e20, 0x8132, 0x4852,
                    { 0x90, 0xcc, 0x55, 0x1a, 0x4e, 0x4a, 0x7f, 0x1c } };
    dp_to_text *t = (dp_to_text *)0;
    CHAR16 *s;

    if (p == (EFI_DEVICE_PATH_PROTOCOL *)0) {
        out(L"(null)");
        return;
    }
    if (g_locp(&tg, (void *)0, (void **)&t) != EFI_SUCCESS ||
        t == (dp_to_text *)0) {
        out(L"(no dp-to-text)");
        return;
    }
    s = t->path(p, FALSE, FALSE);
    if (s == (CHAR16 *)0) {
        out(L"(dp-to-text failed)");
        return;
    }
    out(s);
    g_st->BootServices->FreePool(s);
}


static void range(EFI_HANDLE h)
{
    EFI_GUID lig = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = (EFI_LOADED_IMAGE_PROTOCOL *)0;

    if (h == (EFI_HANDLE)0 ||
        g_st->BootServices->HandleProtocol(h, &lig, (void **)&li) !=
            EFI_SUCCESS || li == (EFI_LOADED_IMAGE_PROTOCOL *)0) {
        out(L" [no image]");
        return;
    }
    out(L" base=");
    hex((UINT64)(uintptr_t)li->ImageBase);
    out(L" size=");
    hex(li->ImageSize);
}

static EFI_STATUS EFIAPI t_load(BOOLEAN pol, EFI_HANDLE parent,
                                EFI_DEVICE_PATH_PROTOCOL *dp, void *src,
                                UINTN len, EFI_HANDLE *img)
{
    EFI_STATUS s;

    if (g_busy)
        return g_load(pol, parent, dp, src, len, img);
    g_busy = 1;
    out(L"[bs] LoadImage pol=");
    hex(pol);
    out(L" parent=");
    hex((UINT64)(uintptr_t)parent);
    out(L" src=");
    hex((UINT64)(uintptr_t)src);
    out(L" len=");
    hex(len);
    out(L"\r\n     dp=");
    dpath(dp);
    out(L"\r\n");
    g_busy = 0;
    s = g_load(pol, parent, dp, src, len, img);
    g_busy = 1;
    out(L"[bs] LoadImage -> ");
    hex(s);
    out(L" img=");
    hex(img != (EFI_HANDLE *)0 ? (UINT64)(uintptr_t)*img : 0);
    if (s == EFI_SUCCESS && img != (EFI_HANDLE *)0)
        range(*img);
    out(L"\r\n");
    g_busy = 0;
    return s;
}

static EFI_STATUS EFIAPI t_start(EFI_HANDLE h, UINTN *n, CHAR16 **d)
{
    EFI_STATUS s;

    if (!g_busy) {
        out(L"[bs] StartImage ");
        hex((UINT64)(uintptr_t)h);
        out(L"\r\n");
    }
    s = g_start(h, n, d);
    if (!g_busy) {
        out(L"[bs] StartImage -> ");
        hex(s);
        out(L"\r\n");
    }
    return s;
}

static EFI_STATUS EFIAPI t_connect(EFI_HANDLE c, EFI_HANDLE *drv,
                                   EFI_DEVICE_PATH_PROTOCOL *rem, BOOLEAN rec)
{
    EFI_STATUS s;

    if (!g_busy) {
        out(L"[bs] ConnectController enter ");
        hex((UINT64)(uintptr_t)c);
        out(L" drv[0]=");
        hex(drv != (EFI_HANDLE *)0 ? (UINT64)(uintptr_t)*drv : 0);
        out(L"\r\n");
    }
    s = g_connect(c, drv, rem, rec);

    if (!g_busy) {
        g_busy = 1;
        out(L"[bs] ConnectController ");
        hex((UINT64)(uintptr_t)c);
        out(L" drv=");
        hex(drv != (EFI_HANDLE *)0 ? (UINT64)(uintptr_t)*drv : 0);
        out(L" rec=");
        hex(rec);
        out(L" -> ");
        hex(s);
        out(L"\r\n");
        g_busy = 0;
    }
    return s;
}

static EFI_STATUS EFIAPI t_locdp(EFI_GUID *g, EFI_DEVICE_PATH_PROTOCOL **dp,
                                 EFI_HANDLE *dev)
{
    EFI_DEVICE_PATH_PROTOCOL *in = (dp != (EFI_DEVICE_PATH_PROTOCOL **)0)
                                       ? *dp : (EFI_DEVICE_PATH_PROTOCOL *)0;
    EFI_STATUS s = g_locdp(g, dp, dev);

    if (!g_busy) {
        g_busy = 1;
        out(L"[bs] LocateDevicePath ");
        guid(g);
        out(L" -> ");
        hex(s);
        out(L" dev=");
        hex(dev != (EFI_HANDLE *)0 ? (UINT64)(uintptr_t)*dev : 0);
        out(L"\r\n     in=");
        dpath(in);
        out(L"\r\n");
        g_busy = 0;
    }
    return s;
}

static EFI_STATUS EFIAPI t_open(EFI_HANDLE h, EFI_GUID *g, void **i,
                                EFI_HANDLE a, EFI_HANDLE c, UINT32 attr)
{
    EFI_STATUS s = g_open(h, g, i, a, c, attr);

    if (!g_busy && (s != EFI_SUCCESS || (attr & 0x30u) != 0)) {
        g_busy = 1;
        out(L"[bs] OpenProtocol ");
        hex((UINT64)(uintptr_t)h);
        out(L" ");
        guid(g);
        out(L" attr=");
        hex(attr);
        out(L" -> ");
        hex(s);
        out(L"\r\n");
        g_busy = 0;
    }
    return s;
}

static EFI_STATUS EFIAPI t_disc(EFI_HANDLE c, EFI_HANDLE d, EFI_HANDLE ch)
{
    EFI_STATUS s;

    if (!g_busy) {
        out(L"[bs] DisconnectController ");
        hex((UINT64)(uintptr_t)c);
        out(L" drv=");
        hex((UINT64)(uintptr_t)d);
        out(L" child=");
        hex((UINT64)(uintptr_t)ch);
        out(L"\r\n");
    }
    s = g_disc(c, d, ch);
    if (!g_busy) {
        out(L"[bs] DisconnectController -> ");
        hex(s);
        out(L"\r\n");
    }
    return s;
}

static EFI_STATUS EFIAPI t_locp(EFI_GUID *g, void *r, void **i)
{
    EFI_STATUS s = g_locp(g, r, i);

    if (!g_busy && s != EFI_SUCCESS) {
        g_busy = 1;
        out(L"[bs] LocateProtocol ");
        guid(g);
        out(L" -> ");
        hex(s);
        out(L"\r\n");
        g_busy = 0;
    }
    return s;
}

static void reseal(EFI_BOOT_SERVICES *bs)
{
    crc_fn crc = (crc_fn)bs->CalculateCrc32;
    UINT32 c = 0;

    bs->Hdr.CRC32 = 0;
    (void)crc(bs, bs->Hdr.HeaderSize, &c);
    bs->Hdr.CRC32 = c;
}

void bstrace_on(EFI_SYSTEM_TABLE *st)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;

    if (g_on)
        return;
    g_st = st;
    g_load = (load_fn)bs->LoadImage;
    g_start = (start_fn)bs->StartImage;
    g_connect = (connect_fn)bs->ConnectController;
    g_locdp = (locdp_fn)bs->LocateDevicePath;
    g_open = (open_fn)bs->OpenProtocol;
    g_locp = (locp_fn)bs->LocateProtocol;
    g_disc = (disc_fn)bs->DisconnectController;
    out(L"[bs] trace on; gBS->LoadImage=");
    hex((UINT64)(uintptr_t)g_load);
    {
        EFI_GUID sg = { 0x1f492041, 0xfadb, 0x4e59,
                        { 0x9e, 0x57, 0x7c, 0xaf, 0xe7, 0x3a, 0x55, 0xab } };
        void **sil = (void **)0;

        if (g_locp(&sg, (void *)0, (void **)&sil) == EFI_SUCCESS &&
            sil != (void **)0) {
            out(L" shim-loader LoadImage=");
            hex((UINT64)(uintptr_t)sil[0]);
        } else {
            out(L" (no shim image loader)");
        }
    }
    out(L"\r\n");
    bs->LoadImage = t_load;
    bs->StartImage = t_start;
    bs->ConnectController = t_connect;
    bs->LocateDevicePath = (void *)t_locdp;
    bs->OpenProtocol = (void *)t_open;
    bs->LocateProtocol = t_locp;
    bs->DisconnectController = t_disc;
    reseal(bs);
    g_on = 1;
}

void bstrace_off(EFI_SYSTEM_TABLE *st)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;

    if (!g_on)
        return;
    bs->LoadImage = g_load;
    bs->StartImage = g_start;
    bs->ConnectController = g_connect;
    bs->LocateDevicePath = (void *)g_locdp;
    bs->OpenProtocol = (void *)g_open;
    bs->LocateProtocol = g_locp;
    bs->DisconnectController = g_disc;
    reseal(bs);
    g_on = 0;
    out(L"[bs] trace off\r\n");
}

/* The loaded-image record of img, and the device path on its DeviceHandle. */
void bstrace_image(EFI_SYSTEM_TABLE *st, EFI_HANDLE img)
{
    EFI_GUID lig = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID dpg = { 0x09576e91, 0x6d3f, 0x11d2,
                     { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
    EFI_LOADED_IMAGE_PROTOCOL *li = (EFI_LOADED_IMAGE_PROTOCOL *)0;
    EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)0;

    g_st = st;
    g_locp = (locp_fn)st->BootServices->LocateProtocol;
    out(L"[img] handle=");
    hex((UINT64)(uintptr_t)img);
    if (st->BootServices->HandleProtocol(img, &lig, (void **)&li) !=
        EFI_SUCCESS || li == (EFI_LOADED_IMAGE_PROTOCOL *)0) {
        out(L" no LoadedImage\r\n");
        return;
    }
    range(img);
    out(L" parent=");
    hex((UINT64)(uintptr_t)li->ParentHandle);
    range(li->ParentHandle);
    out(L" device=");
    hex((UINT64)(uintptr_t)li->DeviceHandle);
    out(L"\r\n      FilePath=");
    dpath((EFI_DEVICE_PATH_PROTOCOL *)li->FilePath);
    out(L"\r\n      device's path=");
    if (li->DeviceHandle != (EFI_HANDLE)0 &&
        st->BootServices->HandleProtocol(li->DeviceHandle, &dpg,
                                         (void **)&dp) == EFI_SUCCESS)
        dpath(dp);
    else
        out(L"(none)");
    {
        EFI_GUID lidp = { 0xbc62157e, 0x3e33, 0x4fec,
                          { 0x99, 0x20, 0x2d, 0x3b, 0x36, 0xd7, 0x50, 0xdf } };
        EFI_DEVICE_PATH_PROTOCOL *ldp = (EFI_DEVICE_PATH_PROTOCOL *)0;
        EFI_STATUS s = st->BootServices->HandleProtocol(img, &lidp,
                                                        (void **)&ldp);

        out(L"\r\n      LoadedImageDevicePath: ");
        hex(s);
        out(L" ");
        if (s == EFI_SUCCESS)
            dpath(ldp);
    }
    out(L"\r\n");
}

/* Every handle carrying Block I/O, with its device path. */
void bstrace_blockio(EFI_SYSTEM_TABLE *st)
{
    EFI_GUID bg = { 0x964e5b21, 0x6459, 0x11d2,
                    { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
    EFI_GUID dpg = { 0x09576e91, 0x6d3f, 0x11d2,
                     { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
    EFI_HANDLE *hs = (EFI_HANDLE *)0;
    UINTN n = 0, i;

    g_st = st;
    g_locp = (locp_fn)st->BootServices->LocateProtocol;
    if (st->BootServices->LocateHandleBuffer(ByProtocol, &bg, (void *)0, &n,
                                             &hs) != EFI_SUCCESS)
        return;
    for (i = 0; i < n; i++) {
        EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)0;

        out(L"[bio] ");
        hex((UINT64)(uintptr_t)hs[i]);
        out(L" ");
        if (st->BootServices->HandleProtocol(hs[i], &dpg, (void **)&dp) ==
            EFI_SUCCESS)
            dpath(dp);
        out(L"\r\n");
    }
    st->BootServices->FreePool(hs);
}
