/* airies-boot: EFI table structures (UEFI spec 4.2-4.6).
 *
 * The boot-services and runtime-services tables are transcribed in FULL and
 * in spec order, because every field is a pointer and one missing placeholder
 * silently shifts every call that follows it. Members we call carry real
 * prototypes; the rest are typed void* placeholders named after the spec so
 * upgrading one to a prototype later is a local edit.
 */

#ifndef AIRIES_UEFI_TABLES_H
#define AIRIES_UEFI_TABLES_H

#include <efi/types.h>
#include <efi/textio.h>
#include <efi/devpath.h>

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ---- Boot services (spec 4.4) ---- */

/* AllocatePool memory types (spec 7.2; only what we pass). */
typedef UINT32 EFI_MEMORY_TYPE;
#define EfiLoaderData ((EFI_MEMORY_TYPE)2)

/* AllocatePages allocation types (spec 7.2). Only AllocateAnyPages is passed;
 * the other two are named so a reader can see the enum is complete and that
 * the value 0 we use is the one that means "anywhere". */
typedef UINT32 EFI_ALLOCATE_TYPE;
#define AllocateAnyPages   ((EFI_ALLOCATE_TYPE)0)
#define AllocateMaxAddress ((EFI_ALLOCATE_TYPE)1)
#define AllocateAddress    ((EFI_ALLOCATE_TYPE)2)

/* A physical address is 64-bit on every architecture the spec defines it for,
 * and stays UINT64 rather than a pointer type: AllocatePages writes the
 * allocated address through this, and on a 32-bit build it would still be a
 * 64-bit slot. */
typedef UINT64 EFI_PHYSICAL_ADDRESS;

#define EFI_PAGE_SIZE 4096u

/* LocateHandleBuffer search types (spec 7.3). */
typedef UINT32 EFI_LOCATE_SEARCH_TYPE;
#define AllHandles       ((EFI_LOCATE_SEARCH_TYPE)0)
#define ByRegisterNotify ((EFI_LOCATE_SEARCH_TYPE)1)
#define ByProtocol       ((EFI_LOCATE_SEARCH_TYPE)2)

typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;

struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;

    /* Task priority */
    void *RaiseTPL;
    void *RestoreTPL;

    /* Memory */
    /* Page allocation, for the one buffer that cannot come from the pool:
     * the personal-OS UKI read out of the encrypted p3 (DESIGN Section 5).
     * A UKI is tens of megabytes and is handed to LoadImage as a source
     * buffer, so it wants page granularity and page alignment rather than
     * the pool's arbitrary alignment. */
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE Type,
                                       EFI_MEMORY_TYPE MemoryType,
                                       UINTN Pages,
                                       EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory,
                                   UINTN Pages);
    void *GetMemoryMap;
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType,
                                      UINTN Size,
                                      void **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);

    /* Event and timer */
    void *CreateEvent;
    void *SetTimer;
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN NumberOfEvents,
                                      EFI_EVENT *Event,
                                      UINTN *Index);
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;

    /* Protocol handler */
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle,
                                        EFI_GUID *Protocol,
                                        void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;

    /* Image */
    /* The verifying load path of DESIGN Sections 2, 6 and 7. We never
     * install a security override, so what boots is the platform's decision
     * on both of the two shapes this call takes here:
     *
     *   - ISO path (Section 6): DevicePath set, SourceBuffer NULL. The
     *     firmware reads the image itself, which is also what gives the
     *     payload a LoadedImage->DeviceHandle pointing at the ISO's ESP so a
     *     distro bootloader can find its own configuration.
     *   - UKI path (Section 5): SourceBuffer set, DevicePath NULL, because a
     *     LUKS volume has no firmware-readable device path at all. The image
     *     is still verified: the load goes through the shim-verified path of
     *     Section 7.6, and shim verifies the bytes in the buffer against
     *     db/MOK/SBAT before it will run them.
     *
     * Neither shape is a relaxation, and neither may be "unified" into the
     * other: the ISO path must name a device path to keep its DeviceHandle,
     * and the UKI path has no device path to name. */
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN BootPolicy,
                                   EFI_HANDLE ParentImageHandle,
                                   EFI_DEVICE_PATH_PROTOCOL *DevicePath,
                                   void *SourceBuffer,
                                   UINTN SourceSize,
                                   EFI_HANDLE *ImageHandle);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE ImageHandle,
                                    UINTN *ExitDataSize,
                                    CHAR16 **ExitData);
    void *Exit;
    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE ImageHandle);
    void *ExitBootServices;

    /* Miscellaneous */
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout,
                                          UINT64 WatchdogCode,
                                          UINTN DataSize,
                                          CHAR16 *WatchdogData);

    /* Driver support */
    EFI_STATUS (EFIAPI *ConnectController)(
        EFI_HANDLE ControllerHandle,
        EFI_HANDLE *DriverImageHandle,
        EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath,
        BOOLEAN Recursive);
    EFI_STATUS (EFIAPI *DisconnectController)(EFI_HANDLE ControllerHandle,
                                              EFI_HANDLE DriverImageHandle,
                                              EFI_HANDLE ChildHandle);

    /* Open/close protocol */
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;

    /* Library */
    void *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE SearchType,
                                            EFI_GUID *Protocol,
                                            void *SearchKey,
                                            UINTN *NoHandles,
                                            EFI_HANDLE **Buffer);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol,
                                        void *Registration,
                                        void **Interface);
    /* Variadic {GUID*, interface*} pairs terminated by a NULL GUID. Passing
     * a NULL handle creates one, which is how the virtual CD gets a handle
     * of its own (DESIGN Section 6 stage 2). */
    EFI_STATUS (EFIAPI *InstallMultipleProtocolInterfaces)(EFI_HANDLE *Handle,
                                                           ...);
    EFI_STATUS (EFIAPI *UninstallMultipleProtocolInterfaces)(EFI_HANDLE Handle,
                                                             ...);

    /* CRC */
    void *CalculateCrc32;

    /* Miscellaneous */
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
};

/* ---- Runtime services (spec 4.5) ---- */

typedef struct {
    EFI_TABLE_HEADER Hdr;

    /* Time */
    void *GetTime;
    void *SetTime;
    void *GetWakeupTime;
    void *SetWakeupTime;

    /* Virtual memory */
    void *SetVirtualAddressMap;
    void *ConvertPointer;

    /* Variables. GetVariable is UX-only in this loader (DESIGN Section 7.3):
     * SecureBoot / SetupMode / MokSBState drive labels and messages, never
     * enforcement. */
    EFI_STATUS (EFIAPI *GetVariable)(CHAR16 *VariableName,
                                     EFI_GUID *VendorGuid,
                                     UINT32 *Attributes,
                                     UINTN *DataSize,
                                     void *Data);
    void *GetNextVariableName;
    void *SetVariable;

    /* Miscellaneous */
    void *GetNextHighMonotonicCount;
    void *ResetSystem;

    /* Capsule */
    void *UpdateCapsule;
    void *QueryCapsuleCapabilities;

    /* Variables */
    void *QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

/* ---- System table (spec 4.3) ---- */

typedef struct {
    EFI_GUID Guid;
    void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    UINT32                           FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE         *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* AIRIES_UEFI_TABLES_H */
