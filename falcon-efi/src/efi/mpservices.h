/* falcon-boot: EFI_MP_SERVICES_PROTOCOL (PI spec Volume 2, Section 13.4),
 * the members the lane runner (mp/mp.c) calls. SwitchBSP and
 * EnableDisableAP are opaque: the loader never moves the BSP or turns a
 * processor off. */

#ifndef FALCON_UEFI_MPSERVICES_H
#define FALCON_UEFI_MPSERVICES_H

#include <efi/types.h>

#define EFI_MP_SERVICES_PROTOCOL_GUID \
    { 0x3fdda605, 0xa76e, 0x4f46, \
      { 0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3d, 0x08 } }

/* StatusFlag's bits. */
#define PROCESSOR_AS_BSP_BIT        0x00000001u
#define PROCESSOR_ENABLED_BIT       0x00000002u
#define PROCESSOR_HEALTH_STATUS_BIT 0x00000004u

typedef struct {
    UINT32 Package;
    UINT32 Core;
    UINT32 Thread;
} EFI_CPU_PHYSICAL_LOCATION;

/* The structure as every revision lays out its head. A later revision
 * appends extended topology, returned only when ProcessorNumber carries
 * the CPU_V2_EXTENDED_TOPOLOGY bit, which the loader never sets; the
 * padding is room for it all the same. */
typedef struct {
    UINT64                    ProcessorId;
    UINT32                    StatusFlag;
    EFI_CPU_PHYSICAL_LOCATION Location;
    UINT8                     Reserved[64];
} EFI_PROCESSOR_INFORMATION;

typedef void (EFIAPI *EFI_AP_PROCEDURE)(void *ProcedureArgument);

typedef struct EFI_MP_SERVICES_PROTOCOL EFI_MP_SERVICES_PROTOCOL;

struct EFI_MP_SERVICES_PROTOCOL {
    EFI_STATUS (EFIAPI *GetNumberOfProcessors)(EFI_MP_SERVICES_PROTOCOL *This,
                                               UINTN *NumberOfProcessors,
                                               UINTN *NumberOfEnabled);
    EFI_STATUS (EFIAPI *GetProcessorInfo)(EFI_MP_SERVICES_PROTOCOL *This,
                                          UINTN ProcessorNumber,
                                          EFI_PROCESSOR_INFORMATION *Buffer);
    EFI_STATUS (EFIAPI *StartupAllAPs)(EFI_MP_SERVICES_PROTOCOL *This,
                                       EFI_AP_PROCEDURE Procedure,
                                       BOOLEAN SingleThread,
                                       EFI_EVENT WaitEvent,
                                       UINTN TimeoutInMicroSeconds,
                                       void *ProcedureArgument,
                                       UINTN **FailedCpuList);
    void *StartupThisAP;
    void *SwitchBSP;
    void *EnableDisableAP;
    EFI_STATUS (EFIAPI *WhoAmI)(EFI_MP_SERVICES_PROTOCOL *This,
                                UINTN *ProcessorNumber);
};

#endif /* FALCON_UEFI_MPSERVICES_H */
