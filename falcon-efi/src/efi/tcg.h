/* falcon-boot: the firmware's TPM protocols and its random number protocol,
 * as far as the station's TOTP check uses them (DESIGN Section 13.15).
 *
 * TCG EFI Protocol Specification rev 1.22: EFI_TCG2_PROTOCOL for a TPM 2.0
 * (Section 6), EFI_TCG_PROTOCOL for a TPM 1.2 (Section 7). Only the command
 * pass-through and the calls that say whether a TPM is there are typed; the
 * rest are placeholders, since falcon measures nothing and logs nothing.
 *
 * EFI_TCG2_BOOT_SERVICE_CAPABILITY is laid out with natural alignment, 36
 * bytes, as EDK2 declares it (MdePkg Tcg2Protocol.h) and so every firmware
 * built from EDK2: measured under OVMF with falcon-efi/tools/tpm-probe/, a
 * packed copy reads TPMPresentFlag as 0 with a TPM attached. Size is how
 * the firmware tells which revision of the structure the caller passed.
 * EFI_TCG_BOOT_SERVICE_CAPABILITY has only byte members after Size and is
 * the same either way.
 */

#ifndef FALCON_EFI_TCG_H
#define FALCON_EFI_TCG_H

#include <efi/types.h>

/* ---- TPM 2.0 ---- */

#define EFI_TCG2_PROTOCOL_GUID \
    { 0x607f766c, 0x7455, 0x42be, \
      { 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f } }

typedef struct {
    UINT8 Major;
    UINT8 Minor;
} EFI_TCG2_VERSION;

typedef struct {
    UINT8            Size;
    EFI_TCG2_VERSION StructureVersion;
    EFI_TCG2_VERSION ProtocolVersion;
    UINT32           HashAlgorithmBitmap;
    UINT32           SupportedEventLogs;
    BOOLEAN          TPMPresentFlag;
    UINT16           MaxCommandSize;
    UINT16           MaxResponseSize;
    UINT32           ManufacturerID;
    UINT32           NumberOfPcrBanks;
    UINT32           ActivePcrBanks;
} EFI_TCG2_BOOT_SERVICE_CAPABILITY;

typedef struct EFI_TCG2_PROTOCOL EFI_TCG2_PROTOCOL;
struct EFI_TCG2_PROTOCOL {
    EFI_STATUS (EFIAPI *GetCapability)(EFI_TCG2_PROTOCOL *This,
                                       EFI_TCG2_BOOT_SERVICE_CAPABILITY *Cap);
    void *GetEventLog;
    void *HashLogExtendEvent;
    EFI_STATUS (EFIAPI *SubmitCommand)(EFI_TCG2_PROTOCOL *This,
                                       UINT32 InputParameterBlockSize,
                                       UINT8 *InputParameterBlock,
                                       UINT32 OutputParameterBlockSize,
                                       UINT8 *OutputParameterBlock);
    void *GetActivePcrBanks;
    void *SetActivePcrBanks;
    void *GetResultOfSetActivePcrBanks;
};

/* ---- TPM 1.2 ---- */

#define EFI_TCG_PROTOCOL_GUID \
    { 0xf541796d, 0xa62e, 0x4954, \
      { 0xa7, 0x75, 0x95, 0x84, 0xf6, 0x1b, 0x9c, 0xdd } }

typedef struct {
    UINT8 Major;
    UINT8 Minor;
    UINT8 RevMajor;
    UINT8 RevMinor;
} EFI_TCG_VERSION;

typedef struct {
    UINT8           Size;
    EFI_TCG_VERSION StructureVersion;
    EFI_TCG_VERSION ProtocolSpecVersion;
    UINT8           HashAlgorithmBitmap;
    BOOLEAN         TPMPresentFlag;
    BOOLEAN         TPMDeactivatedFlag;
} EFI_TCG_BOOT_SERVICE_CAPABILITY;

typedef struct EFI_TCG_PROTOCOL EFI_TCG_PROTOCOL;
struct EFI_TCG_PROTOCOL {
    EFI_STATUS (EFIAPI *StatusCheck)(EFI_TCG_PROTOCOL *This,
                                     EFI_TCG_BOOT_SERVICE_CAPABILITY *Cap,
                                     UINT32 *TCGFeatureFlags,
                                     UINT64 *EventLogLocation,
                                     UINT64 *EventLogLastEntry);
    void *HashAll;
    void *LogEvent;
    EFI_STATUS (EFIAPI *PassThroughToTpm)(EFI_TCG_PROTOCOL *This,
                                          UINT32 TpmInputParameterBlockSize,
                                          UINT8 *TpmInputParameterBlock,
                                          UINT32 TpmOutputParameterBlockSize,
                                          UINT8 *TpmOutputParameterBlock);
    void *HashLogExtendEvent;
};

/* ---- EFI_RNG_PROTOCOL (UEFI spec 37.5) ---- */

#define EFI_RNG_PROTOCOL_GUID \
    { 0x3152bca5, 0xeade, 0x433d, \
      { 0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44 } }

typedef struct EFI_RNG_PROTOCOL EFI_RNG_PROTOCOL;
struct EFI_RNG_PROTOCOL {
    void *GetInfo;
    /* A NULL algorithm is the driver's default. */
    EFI_STATUS (EFIAPI *GetRNG)(EFI_RNG_PROTOCOL *This, EFI_GUID *Algorithm,
                                UINTN ValueLength, UINT8 *Value);
};

#endif /* FALCON_EFI_TCG_H */
