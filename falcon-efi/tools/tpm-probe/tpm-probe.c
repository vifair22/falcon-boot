/* The TPM rig's probe (falcon-efi/tools/run-tpm-rig.sh): an EFI application
 * that says what the firmware offers for a TPM, on the console and so on
 * the rig's serial, one "FALCON-TPM ..." line per fact.
 *
 * It answers the question the TOTP work (docs/notes/argon2-totp.md Section
 * 2) cannot be tested without: does this OVMF install EFI_TCG2_PROTOCOL
 * for a TPM 2.0, EFI_TCG_PROTOCOL for a TPM 1.2, and does it measure the
 * boot, which is what a PCR0 that is not all zeroes shows. It reads PCR0
 * the way the loader will talk to the TPM, through the protocol's command
 * pass-through (SubmitCommand, PassThroughToTpm), so a probe that passes is
 * also a proof that the transport the loader relies on works under the rig.
 *
 * The protocol layouts are transcribed here rather than put in
 * falcon-efi/src/efi/: the loader's own copies will come with the loader's
 * TPM code, and this file is test scaffolding that never ships.
 */

#include <efi/uefi.h>

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

/* ---- output ---- */

static EFI_SYSTEM_TABLE *g_st;

static void say(const CHAR16 *s)
{
    g_st->ConOut->OutputString(g_st->ConOut, (CHAR16 *)(uintptr_t)s);
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

/* ---- TPM 2.0 ---- */

/* TPM2_PCR_Read of PCR0 in one bank (TPM 2.0 Part 3 Section 22.4): no
 * sessions, one TPMS_PCR_SELECTION with a 3-byte select naming PCR 0. */
static void tcg2_pcr0(TCG2_PROTOCOL *t, UINT16 alg, const CHAR16 *name)
{
    UINT8 cmd[20] = {
        0x80, 0x01,             /* TPM_ST_NO_SESSIONS */
        0x00, 0x00, 0x00, 0x14, /* size */
        0x00, 0x00, 0x01, 0x7e, /* TPM_CC_PCR_Read */
        0x00, 0x00, 0x00, 0x01, /* one selection */
        0x00, 0x00,             /* hash, below */
        0x03, 0x01, 0x00, 0x00  /* sizeofSelect, PCR 0 */
    };
    UINT8 rsp[256];
    EFI_STATUS s;
    UINT32 rc;
    UINT16 dlen;

    cmd[14] = (UINT8)(alg >> 8);
    cmd[15] = (UINT8)alg;
    s = t->SubmitCommand(t, sizeof(cmd), cmd, sizeof(rsp), rsp);
    say(L"FALCON-TPM TCG2 PCR0 ");
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
    }
    /* TPM_ALG_SHA1 0x0004 is bitmap bit 0, TPM_ALG_SHA256 0x000b bit 1. */
    if (cap.ActivePcrBanks & 0x1u)
        tcg2_pcr0(t, 0x0004, L"sha1");
    if (cap.ActivePcrBanks & 0x2u)
        tcg2_pcr0(t, 0x000b, L"sha256");
}

/* ---- TPM 1.2 ---- */

static void probe_tcg(EFI_BOOT_SERVICES *bs)
{
    EFI_GUID g = { 0xf541796d, 0xa62e, 0x4954,
                   { 0xa7, 0x75, 0x95, 0x84, 0xf6, 0x1b, 0x9c, 0xdd } };
    TCG_PROTOCOL *t = (TCG_PROTOCOL *)0;
    TCG_BOOT_SERVICE_CAPABILITY cap;
    UINT32 features = 0;
    UINT64 loc = 0, last = 0;
    /* TPM_PCRRead (TPM 1.2 Part 3 Section 16.2) of PCR 0. */
    UINT8 cmd[14] = {
        0x00, 0xc1,             /* TPM_TAG_RQU_COMMAND */
        0x00, 0x00, 0x00, 0x0e, /* size */
        0x00, 0x00, 0x00, 0x15, /* TPM_ORD_PcrRead */
        0x00, 0x00, 0x00, 0x00  /* PCR 0 */
    };
    UINT8 rsp[64];
    EFI_STATUS s;

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

    s = t->PassThroughToTpm(t, sizeof(cmd), cmd, sizeof(rsp), rsp);
    say(L"FALCON-TPM TCG PCR0 sha1 ");
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

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    (void)image_handle;
    g_st = st;

    say(L"\r\nFALCON-TPM-PROBE\r\n");
    probe_tcg2(st->BootServices);
    probe_tcg(st->BootServices);
    say(L"FALCON-TPM-PROBE-DONE\r\n");
    st->BootServices->Stall(1000000);
    return EFI_SUCCESS;
}
