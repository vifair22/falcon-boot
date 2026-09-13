/* shim-plumbing probe: what does shim actually hand the next stage?
 *
 * Staged under the name the shim being tested launches by default
 * (grubx64.efi for Fedora/Debian, grub.efi for openSUSE) so shim starts it
 * as its second stage. It answers, empirically, the questions DESIGN.md
 * Section 7.6 asks before we build on any of them:
 *
 *   - is SHIM_LOCK installed (branch C floor)?
 *   - is SHIM_IMAGE_LOADER installed (branch B)?
 *   - are the Security/Security2 architectural protocols present, and is the
 *     installed one shim's or the firmware's (branch A)?
 *   - is gBS->LoadImage/StartImage the firmware's, or has shim patched the
 *     table in place?
 *
 * That last one is the interesting one and it needs an A/B: run the probe
 * directly as BOOTX64.EFI to record the firmware's own LoadImage pointer,
 * then run it under shim and compare. When SHIM_IMAGE_LOADER is present the
 * probe can decide by itself, because a hooked gBS->LoadImage is literally
 * the same shim_load_image the protocol exposes.
 *
 * Output goes to ConOut, which OVMF also mirrors to the serial port, so a
 * headless QEMU run captures everything in the serial log.
 */

#include "probe-efi.h"

/* shim refuses to start a second stage that has no .sbat section: pe.c
 * verify_sbat_section() returns EFI_SECURITY_VIOLATION when the section is
 * missing and in_protocol is false, which is exactly shim's own load path.
 * Images loaded later through shim's image-loader protocol set in_protocol,
 * so for those SBAT is optional. A freestanding mingw PE has no such section
 * unless we place one, and without it the probe is refused under Secure Boot
 * no matter how correctly it is signed -- the failure looks identical to a
 * bad signature, which is why it is worth stating here.
 *
 * Format is the SBAT CSV of shim's SBAT.md: the mandatory sbat,1 version row
 * first, then one row per component. The generation number (field 2) is what
 * a future revocation bumps. mingw's linker places an attribute-named section
 * in sequence with the rest, so no post-link objcopy is needed.
 */
__attribute__((section(".sbat"), used))
static const char sbat_csv[] =
    "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n"
    "airies-boot.spike,1,airies-boot,shim-spike-probe,0,https://git.airies.net/vifair22/airies-boot\n";

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;

static EFI_GUID GUID_SHIM_LOCK =
    {0x605dab50, 0xe046, 0x4300, {0xab, 0xb6, 0x3d, 0xd8, 0x10, 0xdd, 0x8b, 0x23}};
static EFI_GUID GUID_SHIM_IMAGE_LOADER =
    {0x1f492041, 0xfadb, 0x4e59, {0x9e, 0x57, 0x7c, 0xaf, 0xe7, 0x3a, 0x55, 0xab}};
static EFI_GUID GUID_SHIM_LOADED_IMAGE =
    {0x6e6baeb8, 0x7108, 0x4179, {0x94, 0x9d, 0xa3, 0x49, 0x34, 0x15, 0xec, 0x97}};
static EFI_GUID GUID_SECURITY_ARCH =
    {0xa46423e3, 0x4617, 0x49f1, {0xb9, 0xff, 0xd1, 0xbf, 0xa9, 0x11, 0x58, 0x39}};
static EFI_GUID GUID_SECURITY2_ARCH =
    {0x94ab2f58, 0x1438, 0x4ef1, {0x91, 0x52, 0x18, 0x94, 0x1a, 0x3a, 0x0e, 0x68}};
static EFI_GUID GUID_LOADED_IMAGE =
    {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static EFI_GUID GUID_GLOBAL_VAR =
    {0x8be4df61, 0x93ca, 0x11d2, {0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c}};
static EFI_GUID GUID_SHIM_VAR =
    {0x605dab50, 0xe046, 0x4300, {0xab, 0xb6, 0x3d, 0xd8, 0x10, 0xdd, 0x8b, 0x23}};
static EFI_GUID GUID_MOK_VAR =
    {0xc451ed2b, 0x9694, 0x45d3, {0xba, 0xba, 0xed, 0x9f, 0x89, 0x88, 0xa3, 0x89}};
static EFI_GUID GUID_DEVICE_PATH =
    {0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

static void put(CHAR16 *s)
{
    ST->ConOut->OutputString(ST->ConOut, s);
}

static void put_hex(UINT64 v)
{
    static const CHAR16 digits[] = u"0123456789abcdef";
    CHAR16 buf[19];
    int i;

    buf[0] = u'0';
    buf[1] = u'x';
    for (i = 0; i < 16; i++)
        buf[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xf];
    buf[18] = 0;
    put(buf);
}

static void put_dec(UINT64 v)
{
    CHAR16 buf[21];
    int i = 20;

    buf[20] = 0;
    if (v == 0)
        buf[--i] = u'0';
    while (v && i > 0) {
        buf[--i] = (CHAR16)(u'0' + (v % 10));
        v /= 10;
    }
    put(&buf[i]);
}

static void line(void) { put(u"\r\n"); }

/* Report LocateProtocol for one GUID and hand back the interface. */
static void *report_protocol(CHAR16 *name, EFI_GUID *guid)
{
    void *iface = 0;
    EFI_STATUS st = BS->LocateProtocol(guid, 0, &iface);

    put(u"  ");
    put(name);
    put(u": ");
    if (!EFI_ERROR(st) && iface) {
        put(u"PRESENT  iface=");
        put_hex((UINT64)iface);
    } else {
        put(u"absent   status=");
        put_hex(st);
        iface = 0;
    }
    line();
    return iface;
}

/* Report a UEFI variable's first byte, which is all these flags carry. */
static void report_var(CHAR16 *name, CHAR16 *varname, EFI_GUID *guid)
{
    UINT8 val = 0;
    UINTN size = sizeof(val);
    UINT32 attr = 0;
    EFI_STATUS st;

    st = ST->RuntimeServices->GetVariable(varname, guid, &attr, &size, &val);
    put(u"  ");
    put(name);
    put(u": ");
    if (EFI_ERROR(st)) {
        put(u"unset    status=");
        put_hex(st);
    } else {
        put(u"value=");
        put_dec(val);
        put(u"  attr=");
        put_hex(attr);
    }
    line();
}

/* Function pointers cannot portably go through a void* parameter, so
 * addresses printed for boot-service entries take this integer path. */
static void report_addr(CHAR16 *name, UINT64 v)
{
    put(u"  ");
    put(name);
    put(u": ");
    put_hex(v);
    line();
}

static void report_ptr(CHAR16 *name, void *p)
{
    put(u"  ");
    put(name);
    put(u": ");
    put_hex((UINT64)p);
    line();
}


static void copy_bytes(UINT8 *dst, const UINT8 *src, UINTN n)
{
    UINTN i;

    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

static UINTN ucs2_len(const CHAR16 *s)
{
    UINTN n = 0;

    while (s[n])
        n++;
    return n;
}

/* Total bytes of a device path including its end node. */
static UINTN dp_size(EFI_DEVICE_PATH_PROTOCOL *dp)
{
    UINTN total = 0;

    while (dp->Type != END_DEVICE_PATH_TYPE) {
        UINTN len = DP_LEN(dp);

        if (len < 4)
            return 0;
        total += len;
        dp = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)dp + len);
    }
    return total + DP_LEN(dp);
}

/* Build <device path of the volume we booted from>/<path>, so LoadImage can
 * be handed a real file rather than a source buffer. Going through a device
 * path is deliberate: it is the call shape the loader will use, and it is the
 * one whose verification behaviour we are trying to pin down. */
static EFI_DEVICE_PATH_PROTOCOL *dp_for_file(EFI_HANDLE device, CHAR16 *path)
{
    EFI_DEVICE_PATH_PROTOCOL *dev = 0, *out, *node;
    UINTN devlen, namebytes, nodelen, total;
    UINT8 *p;

    if (EFI_ERROR(BS->HandleProtocol(device, &GUID_DEVICE_PATH, (void **)&dev))
        || !dev)
        return 0;

    devlen = dp_size(dev);
    if (devlen < 4)
        return 0;
    devlen -= 4; /* drop the end node; we append our own */

    namebytes = (ucs2_len(path) + 1) * sizeof(CHAR16);
    nodelen = 4 + namebytes;
    total = devlen + nodelen + 4;

    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, total, (void **)&out)) || !out)
        return 0;

    p = (UINT8 *)out;
    copy_bytes(p, (const UINT8 *)dev, devlen);
    p += devlen;

    node = (EFI_DEVICE_PATH_PROTOCOL *)p;
    node->Type = MEDIA_DEVICE_PATH;
    node->SubType = MEDIA_FILEPATH_DP;
    node->Length[0] = (UINT8)(nodelen & 0xff);
    node->Length[1] = (UINT8)((nodelen >> 8) & 0xff);
    copy_bytes(p + 4, (const UINT8 *)path, namebytes);
    p += nodelen;

    node = (EFI_DEVICE_PATH_PROTOCOL *)p;
    node->Type = END_DEVICE_PATH_TYPE;
    node->SubType = END_ENTIRE_SUBTYPE;
    node->Length[0] = 4;
    node->Length[1] = 0;

    return out;
}

/* The whole point of the spike: hand gBS->LoadImage a file and report what
 * comes back. Under Secure Boot with shim's hook in place, an EFI_SUCCESS
 * means shim verified it against db/MOK; EFI_SECURITY_VIOLATION (0x1a) means
 * it refused. The image is unloaded again immediately -- we never start it. */
static void try_load(CHAR16 *label, EFI_HANDLE self, EFI_HANDLE device,
                     CHAR16 *path)
{
    EFI_DEVICE_PATH_PROTOCOL *dp;
    EFI_HANDLE loaded = 0;
    EFI_STATUS st;

    put(u"  ");
    put(label);
    put(u": ");

    dp = dp_for_file(device, path);
    if (!dp) {
        put(u"device path build failed");
        line();
        return;
    }

    st = BS->LoadImage(0, self, dp, 0, 0, &loaded);
    if (st == EFI_SUCCESS) {
        put(u"LOADED (verified)");
        if (loaded)
            BS->UnloadImage(loaded);
    } else {
        put(u"refused  status=");
        put_hex(st);
    }
    line();
    BS->FreePool(dp);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab);

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    SHIM_IMAGE_LOADER *sil;
    EFI_INPUT_KEY key;
    UINTN idx;

    ST = systab;
    BS = systab->BootServices;

    /* The probe can sit at a shim prompt for a while; do not let the
     * firmware watchdog reboot the box mid-report. */
    BS->SetWatchdogTimer(0, 0, 0, 0);

    line();
    put(u"=== airies-boot shim plumbing probe ===");
    line();

    put(u"firmware: ");
    put(ST->FirmwareVendor ? ST->FirmwareVendor : u"(none)");
    put(u" rev=");
    put_hex(ST->FirmwareRevision);
    line();

    put(u"[image]");
    line();
    if (!EFI_ERROR(BS->HandleProtocol(image, &GUID_LOADED_IMAGE, (void **)&li))
        && li) {
        report_ptr(u"ImageBase          ", li->ImageBase);
        report_ptr(u"ParentHandle       ", li->ParentHandle);
        put(u"  ImageSize          : ");
        put_hex(li->ImageSize);
        line();
    } else {
        put(u"  LoadedImage unavailable");
        line();
    }

    put(u"[variables]");
    line();
    report_var(u"SecureBoot         ", u"SecureBoot", &GUID_GLOBAL_VAR);
    report_var(u"SetupMode          ", u"SetupMode", &GUID_GLOBAL_VAR);
    report_var(u"MokSBState         ", u"MokSBState", &GUID_SHIM_VAR);
    report_var(u"MokSBStateRT       ", u"MokSBStateRT", &GUID_SHIM_VAR);

    put(u"[protocols]");
    line();
    report_protocol(u"SHIM_LOCK          ", &GUID_SHIM_LOCK);
    sil = report_protocol(u"SHIM_IMAGE_LOADER  ", &GUID_SHIM_IMAGE_LOADER);
    report_protocol(u"SHIM_LOADED_IMAGE  ", &GUID_SHIM_LOADED_IMAGE);
    report_protocol(u"EFI_SECURITY_ARCH  ", &GUID_SECURITY_ARCH);
    report_protocol(u"EFI_SECURITY2_ARCH ", &GUID_SECURITY2_ARCH);

    /* Boot-services pointers. Compared against a firmware-only baseline run,
     * a changed LoadImage/StartImage is shim's in-place table patch. */
    put(u"[boot services]");
    line();
    report_ptr(u"BS                 ", BS);
    report_addr(u"BS->LoadImage      ", (UINT64)BS->LoadImage);
    report_ptr(u"BS->StartImage     ", BS->StartImage);
    report_ptr(u"BS->Exit           ", BS->Exit);
    report_ptr(u"BS->ExitBootServices", BS->ExitBootServices);

    if (sil) {
        put(u"[shim image loader]");
        line();
        report_addr(u"SIL->LoadImage     ", (UINT64)sil->LoadImage);
        report_ptr(u"SIL->StartImage    ", sil->StartImage);
        report_ptr(u"SIL->Exit          ", sil->Exit);
        put(u"  gBS->LoadImage is shim's: ");
        put(BS->LoadImage == sil->LoadImage ? u"YES" : u"NO");
        line();
        put(u"  gBS->StartImage is shim's: ");
        put(BS->StartImage == sil->StartImage ? u"YES" : u"NO");
        line();
    }

    /* Payload files staged by run-probe.sh. Names say what they are; the
     * point is which of them gBS->LoadImage accepts. */
    if (li && li->DeviceHandle) {
        put(u"[LoadImage of staged payloads]");
        line();
        try_load(u"mok-signed +sbat  ", image, li->DeviceHandle,
                 u"\\EFI\\BOOT\\pay-mok-sbat.efi");
        try_load(u"mok-signed no sbat", image, li->DeviceHandle,
                 u"\\EFI\\BOOT\\pay-mok-nosbat.efi");
        try_load(u"unsigned   +sbat  ", image, li->DeviceHandle,
                 u"\\EFI\\BOOT\\pay-unsigned.efi");
    }

    put(u"=== probe done, press a key to return to the caller ===");
    line();

    /* Drain, then block on one keystroke so an interactive run is readable.
     * A headless QEMU run just times out here, which is fine: everything
     * above is already in the serial log. */
    while (BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &idx) == EFI_SUCCESS) {
        if (ST->ConIn->ReadKeyStroke(ST->ConIn, &key) == EFI_SUCCESS)
            break;
    }

    return EFI_SUCCESS;
}
