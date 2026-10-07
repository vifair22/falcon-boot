/* The TPM rig's probe (falcon-efi/tools/run-tpm-rig.sh): an EFI application
 * that says what the firmware offers for a TPM, on the console and so on
 * the rig's serial, one "FALCON-TPM ..." line per fact.
 *
 * It answers the question the TOTP check (DESIGN Section 12.15) cannot be
 * tested without: does this OVMF install EFI_TCG2_PROTOCOL
 * for a TPM 2.0, EFI_TCG_PROTOCOL for a TPM 1.2, and does it measure the
 * boot, which is what a PCR0 that is not all zeroes shows. It reads PCR0
 * the way the loader will talk to the TPM, through the protocol's command
 * pass-through (SubmitCommand, PassThroughToTpm), so a probe that passes is
 * also a proof that the transport the loader relies on works under the rig.
 *
 * It also saves the firmware's event log, whole, as \falcon-evlog.bin on the
 * volume it was loaded from: the TOTP cases enroll from it on the host, as
 * mkstick enrolls from securityfs on a machine (DESIGN Section 12.15). Run
 * by the station, the log holds the station's own measurement and then the
 * probe's.
 *
 * And, when the station saved one (every boot with a TPM), its FalconEvLog
 * record as \falcon-evsave.bin, which the totp cases enroll from as mkstick
 * would from efivarfs.
 *
 * Every PCR is read, not only PCR0, and every line it prints is also saved
 * as \falcon-tpm.txt. On real hardware that pair is the point: a log can
 * only be checked against the PCRs as they stood when it was taken, which
 * Linux cannot report later (the station and the UKI have extended PCR 4
 * by then), and a firmware console is gone once the probe returns. The
 * T430, whose coreboot and edk2 keep separate logs, is where this was
 * needed.
 *
 * The protocol layouts are transcribed here rather than taken from
 * falcon-efi/src/efi/tcg.h, so this file stays what it was written as:
 * test scaffolding with no dependency on the loader it checks.
 */

#include <efi/uefi.h>
#include <efi/loadedimage.h>
#include <efi/sfs.h>

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

/* ---- TCG EFI Protocol Specification (TPM 2.0), rev 1.22 Section 6 ---- */

typedef struct {
    UINT8  Major;
    UINT8  Minor;
} TCG2_VERSION;

/* Natural alignment, 36 bytes: EDK2's layout (MdePkg Tcg2Protocol.h), and
 * so every firmware built from it. Packed, the firmware takes the smaller
 * Size for the 1.0 structure and the fields land a byte out, which is how
 * this was found (TPMPresentFlag read as 0 with a TPM attached). Size is
 * how the firmware tells which revision the caller passed. */
typedef struct {
    UINT8        Size;
    TCG2_VERSION StructureVersion;
    TCG2_VERSION ProtocolVersion;
    UINT32       HashAlgorithmBitmap;
    UINT32       SupportedEventLogs;
    BOOLEAN      TPMPresentFlag;
    UINT16       MaxCommandSize;
    UINT16       MaxResponseSize;
    UINT32       ManufacturerID;
    UINT32       NumberOfPcrBanks;
    UINT32       ActivePcrBanks;
} TCG2_BOOT_SERVICE_CAPABILITY;

typedef struct TCG2_PROTOCOL TCG2_PROTOCOL;
struct TCG2_PROTOCOL {
    EFI_STATUS (EFIAPI *GetCapability)(TCG2_PROTOCOL *This,
                                       TCG2_BOOT_SERVICE_CAPABILITY *Cap);
    EFI_STATUS (EFIAPI *GetEventLog)(TCG2_PROTOCOL *This, UINT32 Format,
                                     UINT64 *Location, UINT64 *LastEntry,
                                     BOOLEAN *Truncated);
    void *HashLogExtendEvent;
    EFI_STATUS (EFIAPI *SubmitCommand)(TCG2_PROTOCOL *This, UINT32 InSize,
                                       UINT8 *In, UINT32 OutSize, UINT8 *Out);
    EFI_STATUS (EFIAPI *GetActivePcrBanks)(TCG2_PROTOCOL *This,
                                           UINT32 *Banks);
    void *SetActivePcrBanks;
    void *GetResultOfSetActivePcrBanks;
};

#define TCG2_EVENT_LOG_FORMAT_TCG_1_2 0x1u
#define TCG2_EVENT_LOG_FORMAT_TCG_2   0x2u

/* ---- TCG EFI Protocol Specification (TPM 1.2), rev 1.22 Section 7 ---- */

typedef struct {
    UINT8 Major;
    UINT8 Minor;
    UINT8 RevMajor;
    UINT8 RevMinor;
} TCG_VERSION;

typedef struct {
    UINT8       Size;
    TCG_VERSION StructureVersion;
    TCG_VERSION ProtocolSpecVersion;
    UINT8       HashAlgorithmBitmap;
    BOOLEAN     TPMPresentFlag;
    BOOLEAN     TPMDeactivatedFlag;
} TCG_BOOT_SERVICE_CAPABILITY;

typedef struct TCG_PROTOCOL TCG_PROTOCOL;
struct TCG_PROTOCOL {
    EFI_STATUS (EFIAPI *StatusCheck)(TCG_PROTOCOL *This,
                                     TCG_BOOT_SERVICE_CAPABILITY *Cap,
                                     UINT32 *Features, UINT64 *LogLocation,
                                     UINT64 *LogLastEntry);
    void *HashAll;
    void *LogEvent;
    EFI_STATUS (EFIAPI *PassThroughToTpm)(TCG_PROTOCOL *This, UINT32 InSize,
                                          UINT8 *In, UINT32 OutSize,
                                          UINT8 *Out);
    void *HashLogExtendEvent;
};

/* PC Client platforms have 24 PCRs, on 1.2 and 2.0 alike. */
#define PCR_COUNT 24u

/* ---- output ---- */

static EFI_SYSTEM_TABLE *g_st;
static EFI_HANDLE g_image;

/* What was said, as ASCII, for \falcon-tpm.txt. Every line is ASCII by
 * construction; anything else would be a bug here, so it is kept as '?'
 * rather than encoded. 24 PCRs of two banks and the header lines stay far
 * below the size; a transcript that fills it is cut, not overrun. */
static UINT8 g_txt[16384];
static UINTN g_txt_len;

static void say(const CHAR16 *s)
{
    const CHAR16 *c;

    g_st->ConOut->OutputString(g_st->ConOut, (CHAR16 *)(uintptr_t)s);
    for (c = s; *c != 0 && g_txt_len < sizeof(g_txt); c++)
        if (*c != L'\r')
            g_txt[g_txt_len++] = *c < 0x80u ? (UINT8)*c : (UINT8)'?';
}

static void say_dec(unsigned v)
{
    CHAR16 buf[11];
    unsigned i = sizeof(buf) / sizeof(buf[0]) - 1u;

    buf[i] = 0;
    do {
        buf[--i] = (CHAR16)(L'0' + v % 10u);
        v /= 10u;
    } while (v != 0u && i > 0u);
    say(buf + i);
}

static void say_hex(UINT64 v, unsigned digits)
{
    CHAR16 buf[17];
    unsigned i;

    if (digits > 16u)
        digits = 16u;
    for (i = 0; i < digits; i++) {
        unsigned n = (unsigned)(v >> (4u * (digits - 1u - i))) & 0xFu;
        buf[i] = (CHAR16)(n < 10u ? L'0' + n : L'a' + n - 10u);
    }
    buf[digits] = 0;
    say(buf);
}

static void say_bytes(const UINT8 *p, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++)
        say_hex(p[i], 2);
}

static int all_zero(const UINT8 *p, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (p[i] != 0u)
            return 0;
    return 1;
}

/* SetMem is not typed in efi/tables.h, and one loop is less than a
 * prototype for it. */
static void zero(void *p, unsigned n)
{
    UINT8 *b = (UINT8 *)p;
    unsigned i;

    for (i = 0; i < n; i++)
        b[i] = 0;
}

static UINT32 be32(const UINT8 *p)
{
    return (UINT32)p[0] << 24 | (UINT32)p[1] << 16 | (UINT32)p[2] << 8 | p[3];
}

static UINT16 be16(const UINT8 *p)
{
    return (UINT16)((unsigned)p[0] << 8 | p[1]);
}

static UINT32 le32(const UINT8 *p)
{
    return (UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16 |
           (UINT32)p[3] << 24;
}

/* ---- the event log, saved ---- */

typedef EFI_STATUS (EFIAPI *file_write_fn)(EFI_FILE_PROTOCOL *This,
                                           UINTN *BufferSize, void *Buffer);
typedef EFI_STATUS (EFIAPI *file_delete_fn)(EFI_FILE_PROTOCOL *This);

/* Write and Delete are placeholders in efi/sfs.h, which only reads; a slot
 * is copied into a typed pointer byte by byte, since C has no cast from an
 * object pointer to a function pointer. */
static void slot_to_fn(void *slot, void *fn, unsigned size)
{
    UINT8 *d = (UINT8 *)fn;
    const UINT8 *src = (const UINT8 *)&slot;
    unsigned i;

    for (i = 0; i < size; i++)
        d[i] = src[i];
}

#define FILE_MODE_RW_CREATE 0x8000000000000003ULL

/* Writes data to name on the volume the probe was loaded from; the what
 * names it in the line that says how it went. */
static void save_file(const CHAR16 *name, const CHAR16 *what,
                      const UINT8 *data, UINTN len)
{
    EFI_GUID lig = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID sfg = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = (EFI_LOADED_IMAGE_PROTOCOL *)0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    EFI_FILE_PROTOCOL *root = (EFI_FILE_PROTOCOL *)0;
    EFI_FILE_PROTOCOL *f = (EFI_FILE_PROTOCOL *)0;
    file_write_fn wr;
    file_delete_fn del;
    UINTN n = len;
    EFI_STATUS s;

    say(L"FALCON-TPM ");
    say(what);
    say(L" ");
    if (g_st->BootServices->HandleProtocol(g_image, &lig, (void **)&li) !=
            EFI_SUCCESS ||
        g_st->BootServices->HandleProtocol(li->DeviceHandle, &sfg,
                                           (void **)&fs) != EFI_SUCCESS ||
        fs->OpenVolume(fs, &root) != EFI_SUCCESS) {
        say(L"not saved: no writable volume\r\n");
        return;
    }
    /* Open with create keeps an older, longer file's tail past what this
     * run writes, so an existing file is deleted first (Delete also closes
     * the handle, whatever it returns). */
    if (root->Open(root, &f, (CHAR16 *)(uintptr_t)name,
                   FILE_MODE_RW_CREATE, 0) == EFI_SUCCESS) {
        slot_to_fn(f->Delete, &del, sizeof del);
        (void)del(f);
    }
    if (root->Open(root, &f, (CHAR16 *)(uintptr_t)name, FILE_MODE_RW_CREATE,
                   0) != EFI_SUCCESS) {
        root->Close(root);
        say(L"not saved: cannot create the file\r\n");
        return;
    }
    slot_to_fn(f->Write, &wr, sizeof wr);
    s = wr(f, &n, (void *)(uintptr_t)data);
    f->Close(f);
    root->Close(root);
    if (s != EFI_SUCCESS || n != len) {
        say(L"not saved: write status 0x");
        say_hex(s, 16);
        say(L"\r\n");
        return;
    }
    say(L"saved 0x");
    say_hex(len, 8);
    say(L" bytes\r\n");
}

static void save_log(const UINT8 *log, UINTN len)
{
    save_file(L"\\falcon-evlog.bin", L"evlog", log, len);
}

/* The crypto-agile log's length: the header event, then TCG_PCR_EVENT2s up
 * to and including the one at last, whose digests are sized by the
 * header's algorithm table. */
static UINTN tcg2_log_len(const UINT8 *log, UINT64 last)
{
    UINT32 algs[8][2];
    UINT32 nalg, i;
    UINTN off;
    const UINT8 *d = log + 32;

    if (le32(log + 28) < 28u)
        return 0;
    nalg = le32(d + 24);
    if (nalg == 0 || nalg > 8u)
        return 0;
    for (i = 0; i < nalg; i++) {
        algs[i][0] = (UINT32)d[28 + 4 * i] | (UINT32)d[29 + 4 * i] << 8;
        algs[i][1] = (UINT32)d[30 + 4 * i] | (UINT32)d[31 + 4 * i] << 8;
    }
    off = 32u + le32(log + 28);
    for (;;) {
        UINTN at = off + 12u;
        UINT32 count = le32(log + off + 8);
        UINT32 k, j;

        if (count > 8u)
            return 0;
        for (k = 0; k < count; k++) {
            UINT32 alg = (UINT32)log[at] | (UINT32)log[at + 1] << 8;
            UINT32 size = 0;

            for (j = 0; j < nalg; j++)
                if (algs[j][0] == alg)
                    size = algs[j][1];
            if (size == 0)
                return 0;
            at += 2u + size;
        }
        at += 4u + le32(log + at);
        if ((UINT64)(uintptr_t)(log + off) >= last)
            return at;
        off = at;
        if (off > 0x400000u)
            return 0;
    }
}

/* ---- TPM 2.0 ---- */

/* TPM2_PCR_Read of one PCR in one bank (TPM 2.0 Part 3 Section 22.4): no
 * sessions, one TPMS_PCR_SELECTION with a 3-byte select naming it. One
 * PCR a call, since a read answers at most eight digests. */
static void tcg2_pcr(TCG2_PROTOCOL *t, UINT16 alg, const CHAR16 *name,
                     unsigned idx)
{
    UINT8 cmd[20] = {
        0x80, 0x01,             /* TPM_ST_NO_SESSIONS */
        0x00, 0x00, 0x00, 0x14, /* size */
        0x00, 0x00, 0x01, 0x7e, /* TPM_CC_PCR_Read */
        0x00, 0x00, 0x00, 0x01, /* one selection */
        0x00, 0x00,             /* hash, below */
        0x03, 0x00, 0x00, 0x00  /* sizeofSelect, the PCR's bit below */
    };
    UINT8 rsp[256];
    EFI_STATUS s;
    UINT32 rc;
    UINT16 dlen;

    cmd[14] = (UINT8)(alg >> 8);
    cmd[15] = (UINT8)alg;
    cmd[17u + idx / 8u] = (UINT8)(1u << (idx % 8u));
    s = t->SubmitCommand(t, sizeof(cmd), cmd, sizeof(rsp), rsp);
    say(L"FALCON-TPM TCG2 PCR");
    say_dec(idx);
    say(L" ");
    say(name);
    say(L" ");
    if (s != EFI_SUCCESS) {
        say(L"SubmitCommand status 0x");
        say_hex(s, 16);
        say(L"\r\n");
        return;
    }
    rc = be32(rsp + 6);
    if (rc != 0u) {
        say(L"rc 0x");
        say_hex(rc, 8);
        say(L"\r\n");
        return;
    }
    /* header 10, pcrUpdateCounter 4, TPML_PCR_SELECTION 4+2+1+3,
     * TPML_DIGEST count 4, then TPM2B size and the digest. */
    if (be32(rsp + 24) != 1u) {
        say(L"not allocated\r\n");
        return;
    }
    dlen = be16(rsp + 28);
    if (dlen > 64u)
        dlen = 64u;
    say_bytes(rsp + 30, dlen);
    say(all_zero(rsp + 30, dlen) ? L" zero\r\n" : L" measured\r\n");
}

static void probe_tcg2(EFI_BOOT_SERVICES *bs)
{
    EFI_GUID g = { 0x607f766c, 0x7455, 0x42be,
                   { 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f } };
    TCG2_PROTOCOL *t = (TCG2_PROTOCOL *)0;
    TCG2_BOOT_SERVICE_CAPABILITY cap;
    UINT64 loc = 0, last = 0;
    BOOLEAN trunc = 0;
    EFI_STATUS s;
    unsigned i;

    if (bs->LocateProtocol(&g, (void *)0, (void **)&t) != EFI_SUCCESS ||
        t == (TCG2_PROTOCOL *)0) {
        say(L"FALCON-TPM TCG2 absent\r\n");
        return;
    }
    say(L"FALCON-TPM TCG2 found\r\n");
    zero(&cap, sizeof(cap));
    cap.Size = (UINT8)sizeof(cap);
    s = t->GetCapability(t, &cap);
    if (s != EFI_SUCCESS) {
        say(L"FALCON-TPM TCG2 GetCapability status 0x");
        say_hex(s, 16);
        say(L"\r\n");
        return;
    }
    say(L"FALCON-TPM TCG2 present ");
    say_hex(cap.TPMPresentFlag, 2);
    say(L" proto ");
    say_hex(cap.ProtocolVersion.Major, 2);
    say(L".");
    say_hex(cap.ProtocolVersion.Minor, 2);
    say(L" manufacturer 0x");
    say_hex(cap.ManufacturerID, 8);
    say(L" hashes 0x");
    say_hex(cap.HashAlgorithmBitmap, 8);
    say(L" banks 0x");
    say_hex(cap.ActivePcrBanks, 8);
    say(L" logs 0x");
    say_hex(cap.SupportedEventLogs, 8);
    say(L"\r\n");
    if (!cap.TPMPresentFlag)
        return;

    if (cap.SupportedEventLogs & TCG2_EVENT_LOG_FORMAT_TCG_2) {
        s = t->GetEventLog(t, TCG2_EVENT_LOG_FORMAT_TCG_2, &loc, &last,
                           &trunc);
        say(L"FALCON-TPM TCG2 eventlog ");
        if (s != EFI_SUCCESS) {
            say(L"status 0x");
            say_hex(s, 16);
        } else {
            say(L"at 0x");
            say_hex(loc, 16);
            say(L" last 0x");
            say_hex(last, 16);
            say(trunc ? L" truncated" : L" whole");
        }
        say(L"\r\n");
        if (s == EFI_SUCCESS && loc != 0 && last >= loc) {
            UINTN len = tcg2_log_len((const UINT8 *)(uintptr_t)loc, last);

            if (len != 0)
                save_log((const UINT8 *)(uintptr_t)loc, len);
            else
                say(L"FALCON-TPM evlog not saved: it does not parse\r\n");
        }
    }
    /* TPM_ALG_SHA1 0x0004 is bitmap bit 0, TPM_ALG_SHA256 0x000b bit 1. */
    for (i = 0; i < PCR_COUNT; i++) {
        if (cap.ActivePcrBanks & 0x1u)
            tcg2_pcr(t, 0x0004, L"sha1", i);
        if (cap.ActivePcrBanks & 0x2u)
            tcg2_pcr(t, 0x000b, L"sha256", i);
    }
}

/* ---- TPM 1.2 ---- */

/* TPM_PCRRead (TPM 1.2 Part 3 Section 16.2) of one PCR. */
static void tcg_pcr(TCG_PROTOCOL *t, unsigned idx)
{
    UINT8 cmd[14] = {
        0x00, 0xc1,             /* TPM_TAG_RQU_COMMAND */
        0x00, 0x00, 0x00, 0x0e, /* size */
        0x00, 0x00, 0x00, 0x15, /* TPM_ORD_PcrRead */
        0x00, 0x00, 0x00, 0x00  /* the PCR, below */
    };
    UINT8 rsp[64];
    EFI_STATUS s;

    cmd[13] = (UINT8)idx;
    s = t->PassThroughToTpm(t, sizeof(cmd), cmd, sizeof(rsp), rsp);
    say(L"FALCON-TPM TCG PCR");
    say_dec(idx);
    say(L" sha1 ");
    if (s != EFI_SUCCESS) {
        say(L"PassThroughToTpm status 0x");
        say_hex(s, 16);
    } else if (be32(rsp + 6) != 0u) {
        say(L"rc 0x");
        say_hex(be32(rsp + 6), 8);
    } else {
        say_bytes(rsp + 10, 20);
        say(all_zero(rsp + 10, 20) ? L" zero" : L" measured");
    }
    say(L"\r\n");
}

static void probe_tcg(EFI_BOOT_SERVICES *bs)
{
    EFI_GUID g = { 0xf541796d, 0xa62e, 0x4954,
                   { 0xa7, 0x75, 0x95, 0x84, 0xf6, 0x1b, 0x9c, 0xdd } };
    TCG_PROTOCOL *t = (TCG_PROTOCOL *)0;
    TCG_BOOT_SERVICE_CAPABILITY cap;
    UINT32 features = 0;
    UINT64 loc = 0, last = 0;
    EFI_STATUS s;
    unsigned i;

    if (bs->LocateProtocol(&g, (void *)0, (void **)&t) != EFI_SUCCESS ||
        t == (TCG_PROTOCOL *)0) {
        say(L"FALCON-TPM TCG absent\r\n");
        return;
    }
    say(L"FALCON-TPM TCG found\r\n");
    zero(&cap, sizeof(cap));
    cap.Size = (UINT8)sizeof(cap);
    s = t->StatusCheck(t, &cap, &features, &loc, &last);
    if (s != EFI_SUCCESS) {
        say(L"FALCON-TPM TCG StatusCheck status 0x");
        say_hex(s, 16);
        say(L"\r\n");
        return;
    }
    say(L"FALCON-TPM TCG present ");
    say_hex(cap.TPMPresentFlag, 2);
    say(L" deactivated ");
    say_hex(cap.TPMDeactivatedFlag, 2);
    say(L" spec ");
    say_hex(cap.ProtocolSpecVersion.Major, 2);
    say(L".");
    say_hex(cap.ProtocolSpecVersion.Minor, 2);
    say(L" eventlog at 0x");
    say_hex(loc, 16);
    say(L" last 0x");
    say_hex(last, 16);
    say(L"\r\n");
    if (!cap.TPMPresentFlag)
        return;
    /* A 1.2 log is TCG_PCR_EVENTs: 32 bytes of header and the event data;
     * last is where the last one starts. */
    if (loc != 0 && last >= loc)
        save_log((const UINT8 *)(uintptr_t)loc,
                 (UINTN)(last - loc) + 32u +
                     le32((const UINT8 *)(uintptr_t)last + 28));

    for (i = 0; i < PCR_COUNT; i++)
        tcg_pcr(t, i);
}

/* The station's saved record (falcon-core/src/tpm/evsave.h), under
 * falcon's vendor GUID, copied out as it is. */
static void save_evsave(void)
{
    static UINT8 buf[65536];
    EFI_GUID vendor = { 0x50614485, 0x081b, 0x4b7c,
                        { 0x8b, 0x54, 0x2a, 0xbe, 0xff, 0xdf, 0x2f, 0xb3 } };
    UINTN n = sizeof buf;

    if (g_st->RuntimeServices->GetVariable((CHAR16 *)(uintptr_t)L"FalconEvLog",
                                           &vendor, (UINT32 *)0, &n,
                                           buf) != EFI_SUCCESS) {
        say(L"FALCON-TPM evsave none\r\n");
        return;
    }
    save_file(L"\\falcon-evsave.bin", L"evsave", buf, n);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    g_st = st;
    g_image = image_handle;

    say(L"\r\nFALCON-TPM-PROBE\r\n");
    probe_tcg2(st->BootServices);
    probe_tcg(st->BootServices);
    save_evsave();
    save_file(L"\\falcon-tpm.txt", L"report", g_txt, g_txt_len);
    say(L"FALCON-TPM-PROBE-DONE\r\n");
    st->BootServices->Stall(1000000);
    return EFI_SUCCESS;
}
