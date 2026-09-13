/* Self-contained minimal UEFI declarations for the shim-plumbing probe.
 *
 * Deliberately NOT sharing the headers under src/efi. This probe is a
 * throwaway spike artifact and src/efi is under active development by the
 * loader work; a local copy keeps the two from breaking each other, and
 * keeps the probe buildable from a checkout where src/efi has moved on.
 *
 * Only the handful of boot/runtime service entries the probe calls carry
 * real prototypes. Everything else is a void* placeholder named after the
 * UEFI spec field so the table offsets stay correct -- one missing
 * placeholder silently shifts every call after it.
 */

#ifndef SHIM_SPIKE_PROBE_EFI_H
#define SHIM_SPIKE_PROBE_EFI_H

#define EFIAPI __attribute__((ms_abi))

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef signed long long   INT64;
typedef UINT64             UINTN;
typedef unsigned short     CHAR16;
typedef unsigned char      BOOLEAN;
typedef void              *EFI_HANDLE;
typedef void              *EFI_EVENT;
typedef UINT64             EFI_STATUS;

#define EFI_SUCCESS        ((EFI_STATUS)0)
#define EFI_ERROR(s)       (((INT64)(s)) < 0)
#define EFI_NOT_FOUND      ((EFI_STATUS)0x800000000000000eULL)
#define EFI_BUFFER_TOO_SMALL ((EFI_STATUS)0x8000000000000005ULL)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      CHAR16 *String);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    void *ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (EFIAPI *ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
                                       EFI_INPUT_KEY *Key);
    EFI_EVENT WaitForKey;
};

/* Device paths (spec 10.2), only enough to build \EFI\BOOT\<name> on the
 * volume we were loaded from. Length is a two-byte little-endian field that
 * is deliberately unaligned in the spec, hence the UINT8[2]. */
typedef struct {
    UINT8 Type;
    UINT8 SubType;
    UINT8 Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

#define MEDIA_DEVICE_PATH      0x04
#define MEDIA_FILEPATH_DP      0x04
#define END_DEVICE_PATH_TYPE   0x7f
#define END_ENTIRE_SUBTYPE     0xff

#define DP_LEN(dp) ((UINTN)((dp)->Length[0] | ((dp)->Length[1] << 8)))

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_LOAD)(BOOLEAN BootPolicy,
                                            EFI_HANDLE ParentImageHandle,
                                            EFI_DEVICE_PATH_PROTOCOL *DevicePath,
                                            void *SourceBuffer,
                                            UINTN SourceSize,
                                            EFI_HANDLE *ImageHandle);

typedef UINT32 EFI_MEMORY_TYPE;
#define EfiLoaderData ((EFI_MEMORY_TYPE)2)

typedef UINT32 EFI_LOCATE_SEARCH_TYPE;
#define ByProtocol ((EFI_LOCATE_SEARCH_TYPE)2)

typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;

    void *RaiseTPL;
    void *RestoreTPL;

    void *AllocatePages;
    void *FreePages;
    void *GetMemoryMap;
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE, UINTN, void **);
    EFI_STATUS (EFIAPI *FreePool)(void *);

    void *CreateEvent;
    void *SetTimer;
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN, EFI_EVENT *, UINTN *);
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;

    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;

    EFI_IMAGE_LOAD LoadImage;
    void *StartImage;
    void *Exit;
    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE ImageHandle);
    void *ExitBootServices;

    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN, UINT64, UINTN, CHAR16 *);

    void *ConnectController;
    void *DisconnectController;

    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;

    void *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE, EFI_GUID *,
                                            void *, UINTN *, EFI_HANDLE **);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, void *, void **);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;

    void *CalculateCrc32;

    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
};

typedef struct {
    EFI_TABLE_HEADER Hdr;

    void *GetTime;
    void *SetTime;
    void *GetWakeupTime;
    void *SetWakeupTime;

    void *SetVirtualAddressMap;
    void *ConvertPointer;

    EFI_STATUS (EFIAPI *GetVariable)(CHAR16 *VariableName, EFI_GUID *VendorGuid,
                                     UINT32 *Attributes, UINTN *DataSize,
                                     void *Data);
    void *GetNextVariableName;
    void *SetVariable;

    void *GetNextHighMonotonicCount;
    void *ResetSystem;

    void *UpdateCapsule;
    void *QueryCapsuleCapabilities;

    void *QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

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

/* EFI_LOADED_IMAGE_PROTOCOL, truncated after the fields the probe reads. */
typedef struct {
    UINT32      Revision;
    EFI_HANDLE  ParentHandle;
    void       *SystemTable;
    EFI_HANDLE  DeviceHandle;
    void       *FilePath;
    void       *Reserved;
    UINT32      LoadOptionsSize;
    void       *LoadOptions;
    void       *ImageBase;
    UINT64      ImageSize;
    UINT32      ImageCodeType;
    UINT32      ImageDataType;
    void       *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* SHIM_IMAGE_LOADER, shim loader-proto.h. Same four calls as boot services,
 * so the probe only needs the pointers, not callable prototypes. */
typedef struct {
    EFI_IMAGE_LOAD LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
} SHIM_IMAGE_LOADER;

#endif /* SHIM_SPIKE_PROBE_EFI_H */
