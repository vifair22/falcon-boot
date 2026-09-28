/* falcon-boot: EFI table structures (UEFI spec 4.2-4.6).
 *
 * The boot-services and runtime-services tables are transcribed in FULL and
 * in spec order, because every field is a pointer and one missing placeholder
 * silently shifts every call that follows it. Members we call carry real
 * prototypes; the rest are typed void* placeholders named after the spec so
 * upgrading one to a prototype later is a local edit.
 */

#ifndef FALCON_UEFI_TABLES_H
#define FALCON_UEFI_TABLES_H

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

/* GetMemoryMap's entries (spec 7.2). Read only to say how much memory is
 * free when an argon2id keyslot asks for more than AllocatePages gives
 * (pos/pos.c). Entries are DescriptorSize apart, which the firmware may make
 * larger than this struct, so they are never indexed as an array. */
#define EfiConventionalMemory ((EFI_MEMORY_TYPE)7)

typedef struct {
    UINT32               Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    UINT64               VirtualStart;
    UINT64               NumberOfPages;
    UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;

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
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize,
                                      EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                      UINTN *MapKey,
                                      UINTN *DescriptorSize,
                                      UINT32 *DescriptorVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType,
                                      UINTN Size,
                                      void **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);

    /* Event and timer */
    /* Typed for the two events the loader creates: a notification at
     * ExitBootServices, which is the last moment code of ours runs on a
     * boot that does not come back (loader/credfs.c), and the menu's
     * countdown timer (console/menu.c). */
    EFI_STATUS (EFIAPI *CreateEvent)(UINT32 Type, EFI_TPL NotifyTpl,
                                     EFI_EVENT_NOTIFY NotifyFunction,
                                     void *NotifyContext, EFI_EVENT *Event);
    EFI_STATUS (EFIAPI *SetTimer)(EFI_EVENT Event, EFI_TIMER_DELAY Type,
                                  UINT64 TriggerTime);
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN NumberOfEvents,
                                      EFI_EVENT *Event,
                                      UINTN *Index);
    void *SignalEvent;
    EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT Event);
    void *CheckEvent;

    /* Protocol handler */
    /* InterfaceType 0 is EFI_NATIVE_INTERFACE, the only one there is. */
    EFI_STATUS (EFIAPI *InstallProtocolInterface)(EFI_HANDLE *Handle,
                                                  EFI_GUID *Protocol,
                                                  UINT32 InterfaceType,
                                                  void *Interface);
    void *ReinstallProtocolInterface;
    EFI_STATUS (EFIAPI *UninstallProtocolInterface)(EFI_HANDLE Handle,
                                                    EFI_GUID *Protocol,
                                                    void *Interface);
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle,
                                        EFI_GUID *Protocol,
                                        void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;

    /* Image */
    /* The verifying load path of DESIGN Sections 2, 5, 6 and 7. We never
     * install a security override, so what boots is the platform's decision
     * on both shapes this call takes here, and both are device-path loads:
     *
     *   - ISO path (Section 6): the device path names the ISO's firmware-
     *     mounted ESP, which also gives the payload a
     *     LoadedImage->DeviceHandle so a distro bootloader can find its own
     *     configuration.
     *   - UKI path (Sections 5, 5.1): the device path names our synthetic
     *     credential filesystem (falcon-efi/src/loader/credfs.c), which serves
     *     the UKI bytes read off the encrypted p3 plus the .extra.d credential;
     *     sd-stub needs that DeviceHandle to find its companion files. Shim
     *     verifies what it reads off the synthetic device against db/MOK/SBAT
     *     exactly as it would off a real one.
     *
     * A SourceBuffer load (DevicePath NULL) survives only as pos.c's
     * loudly-warned fallback when the credfs publish fails; it is still
     * shim-verified, but sd-stub credentials cannot work there. */
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

/* Variable attributes (spec 8.2). Only the two this loader ever sets are
 * named; the rest are added when something needs one. */
#define EFI_VARIABLE_NON_VOLATILE       0x00000001u
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x00000002u
#define EFI_VARIABLE_RUNTIME_ACCESS     0x00000004u

/* UEFI spec 8.5.1.
 *
 * "Reboot into setup" uses COLD, which is what sd-boot's reboot_into_firmware
 * does. Warm reads better on paper - the indication is already in non-volatile
 * storage, so a full power cycle buys nothing - and it works under OVMF. It
 * did not work on a LENOVO 21EX005RUS, where OsIndicationsSupported advertises
 * bit 0 and the machine came back to the loader instead of to setup. The spec
 * leaves what a warm reset actually skips up to the platform, so "enough of a
 * reset that BDS re-reads OsIndications" is not something it can be relied on
 * to mean. Cold is the one every implementation agrees about. */
typedef enum {
    EfiResetCold,
    EfiResetWarm,
    EfiResetShutdown,
    EfiResetPlatformSpecific
} EFI_RESET_TYPE;

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

    /* Variables. GetVariable is UX-only for the platform's own variables
     * (DESIGN Section 7.3): SecureBoot / SetupMode / MokSBState drive labels
     * and messages, never enforcement.
     *
     * SetVariable writes two, and both are worth naming because the list
     * must stay short and deliberate.
     *
     * FalconNextBoot is ours, and its whole lifecycle belongs to this loader
     * (DESIGN Section 6.3.1 and the `loader` module of Section 9).
     *
     * OsIndications is the platform's, and is the one exception: setting
     * EFI_OS_INDICATIONS_BOOT_TO_FW_UI asks the firmware to show its own
     * settings UI on the next boot (Section 7.4). It is written only on an
     * explicit menu selection, it changes no enforcement state, and the
     * firmware clears it itself. Read-modify-write, because the other bits
     * are somebody else's pending capsule updates.
     *
     * No variable gates a load, and nothing here decides what may run. */
    EFI_STATUS (EFIAPI *GetVariable)(CHAR16 *VariableName,
                                     EFI_GUID *VendorGuid,
                                     UINT32 *Attributes,
                                     UINTN *DataSize,
                                     void *Data);
    void *GetNextVariableName;
    EFI_STATUS (EFIAPI *SetVariable)(CHAR16 *VariableName,
                                     EFI_GUID *VendorGuid,
                                     UINT32 Attributes,
                                     UINTN DataSize,
                                     void *Data);

    /* Miscellaneous */
    void *GetNextHighMonotonicCount;
    /* Real prototype because we call it: the "reboot to firmware setup" row
     * sets OsIndications and then resets, and a reset that went through a
     * void* would be a call with an unchecked signature. */
    void (EFIAPI *ResetSystem)(EFI_RESET_TYPE ResetType,
                               EFI_STATUS ResetStatus,
                               UINTN DataSize,
                               void *ResetData);

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

#endif /* FALCON_UEFI_TABLES_H */
