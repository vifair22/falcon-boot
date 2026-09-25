/* falcon-vdisk: the WinPE virtual-CD kernel driver (DESIGN Section 6.3).
 *
 * WHY THIS EXISTS. Windows install media older than 8 boot a WinPE (2 or 3)
 * whose AttachVirtualDisk cannot attach an ISO, and 2000/XP boot a text-mode
 * setup with no WinPE at all. falcon-winjump's userland ISO attach (the
 * Windows 8+ route) has nothing to call there. So the media the loader
 * serves has to be presented to that Windows as a real optical drive by a
 * kernel driver, which is this: it exposes the ISO as a read-only CD-ROM
 * that CDFS mounts and Setup finds \sources\install.wim on.
 *
 * WHERE THE BYTES COME FROM. WinPE cannot read the exFAT partition the ISO
 * lives on, so the driver does not read the ISO by path. falcon computes the
 * ISO's extents on the physical disk at boot (the same extent map the BIOS
 * virtual CD and falcon-hook use) and hands them to the driver through
 * falcon-winjump; the driver reads those raw disk sectors and serves them as
 * CD sectors. Until that path is wired, the backing is a pool buffer the
 * userland side fills through an IOCTL, which is enough to bring the device
 * and its CDFS mount up. `backing_read` is the seam between the two.
 *
 * Nothing of this survives the install: the driver runs only in the
 * installer's WinPE, and the Windows it lays down has its own boot with
 * driver-signing enforcement on. 32-bit Windows enforces no kernel-mode
 * signing, so this loads unsigned there; 64-bit needs the served boot
 * configuration put into test-signing mode and the driver signed with a
 * throwaway certificate, which Secure Boot on refuses (the platform opt-out,
 * DESIGN Section 7).
 *
 * Built for both widths from this one source, as the hook and winjump are.
 */

#include <ntddk.h>
#include <mountdev.h>

#include "vdiskio.h"

/* DISK_GEOMETRY and the media type live in ntdddisk.h, which this mingw does
 * not ship, so they are here. RemovableMedia is 11 in the MEDIA_TYPE enum. */
typedef enum { FALCON_RemovableMedia = 11 } FALCON_MEDIA_TYPE;
typedef struct {
    LARGE_INTEGER Cylinders;
    FALCON_MEDIA_TYPE MediaType;
    ULONG TracksPerCylinder;
    ULONG SectorsPerTrack;
    ULONG BytesPerSector;
} FALCON_DISK_GEOMETRY;

/* The storage IOCTLs CDFS and the mount path issue, spelled out rather than
 * pulled from ntddstor.h/ntddcdrm.h, which this mingw does not ship. FILE
 * device types: CD-ROM 2, DISK 7, MASS_STORAGE 0x2d. FILE_READ_ACCESS 1. */
#define IOCTL_DISK_GET_DRIVE_GEOMETRY   CTL_CODE(7, 0x0000, 0, 0)
#define IOCTL_CDROM_GET_DRIVE_GEOMETRY  CTL_CODE(2, 0x0013, 0, 1)
#define IOCTL_STORAGE_CHECK_VERIFY      CTL_CODE(0x2d, 0x0200, 0, 1)
#define IOCTL_STORAGE_CHECK_VERIFY2     CTL_CODE(0x2d, 0x0200, 0, 0)
#define IOCTL_CDROM_CHECK_VERIFY        CTL_CODE(2, 0x0200, 0, 1)
#define IOCTL_CDROM_READ_TOC            CTL_CODE(2, 0x0000, 0, 1)
#define IOCTL_STORAGE_GET_DEVICE_NUMBER CTL_CODE(0x2d, 0x0420, 0, 0)
#define IOCTL_STORAGE_GET_HOTPLUG_INFO  CTL_CODE(0x2d, 0x0305, 0, 0)

typedef struct {
    DEVICE_TYPE DeviceType;
    ULONG DeviceNumber;
    ULONG PartitionNumber;
} FALCON_STORAGE_DEVICE_NUMBER;

typedef struct {
    UCHAR Length[2];
    UCHAR FirstTrack;
    UCHAR LastTrack;
    struct {
        UCHAR Reserved;
        UCHAR Control : 4;
        UCHAR Adr : 4;
        UCHAR TrackNumber;
        UCHAR Reserved1;
        UCHAR Address[4];
    } TrackData[2];
} FALCON_CDROM_TOC;

#define VDISK_CTL_DEVICE L"\\Device\\FalconVDisk"
#define VDISK_CTL_LINK   L"\\DosDevices\\FalconVDisk"
#define VDISK_CD_DEVICE  L"\\Device\\FalconCd"

/* Which of the driver's two devices an IRP is for. */
typedef struct {
    int is_cd;
} vdisk_ext;

static UNICODE_STRING g_ctl_name, g_ctl_link, g_cd_name;
static PDEVICE_OBJECT g_ctl_dev, g_cd_dev;

/* The backing store. A pool buffer for now (see the header); the raw-disk
 * read path replaces read_from_pool without touching the callers. */
static PUCHAR    g_pool;
static ULONGLONG g_size;      /* the medium's declared size in bytes */

/* Copies `len` bytes of the medium at `off` into `dst`, zero-filling past the
 * end (a pressed CD answers its padding). The seam the raw-disk path plugs
 * into. */
static void backing_read(ULONGLONG off, ULONG len, PUCHAR dst)
{
    ULONG got = 0;

    if (g_pool != NULL && off < g_size) {
        ULONGLONG avail = g_size - off;

        got = (ULONG)(avail < len ? avail : len);
        RtlCopyMemory(dst, g_pool + off, got);
    }
    if (got < len)
        RtlZeroMemory(dst + got, len - got);
}

static NTSTATUS complete(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static NTSTATUS NTAPI vdisk_create_close(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NTAPI vdisk_read(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION io = IoGetCurrentIrpStackLocation(irp);
    vdisk_ext *ext = dev->DeviceExtension;
    ULONG len = io->Parameters.Read.Length;
    ULONGLONG off = (ULONGLONG)io->Parameters.Read.ByteOffset.QuadPart;
    PUCHAR dst;

    if (!ext->is_cd)
        return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (len == 0)
        return complete(irp, STATUS_SUCCESS, 0);
    /* DO_DIRECT_IO: the caller's pages are described by an MDL. */
    dst = MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority);
    if (dst == NULL)
        return complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    backing_read(off, len, dst);
    return complete(irp, STATUS_SUCCESS, len);
}

static NTSTATUS ctl_ioctl(PIRP irp, PIO_STACK_LOCATION io)
{
    ULONG code = io->Parameters.DeviceIoControl.IoControlCode;
    ULONG in = io->Parameters.DeviceIoControl.InputBufferLength;
    PVOID buf = irp->AssociatedIrp.SystemBuffer;

    if (code == IOCTL_FALCON_VDISK_SET_GEOMETRY) {
        const struct falcon_vdisk_geometry *g = buf;

        if (in < sizeof *g)
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        if (g_pool != NULL)
            ExFreePoolWithTag(g_pool, 'DVaF');
        g_size = g->iso_bytes;
        g_pool = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)g_size, 'DVaF');
        if (g_pool == NULL) {
            g_size = 0;
            return complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        }
        RtlZeroMemory(g_pool, (SIZE_T)g_size);
        return complete(irp, STATUS_SUCCESS, 0);
    }
    if (code == IOCTL_FALCON_VDISK_WRITE) {
        const struct falcon_vdisk_write *w = buf;

        if (in < sizeof *w)
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        if (g_pool == NULL || w->offset + w->length > g_size ||
            in < sizeof *w + w->length)
            return complete(irp, STATUS_INVALID_PARAMETER, 0);
        RtlCopyMemory(g_pool + w->offset, (const UCHAR *)buf + sizeof *w,
                      (SIZE_T)w->length);
        return complete(irp, STATUS_SUCCESS, 0);
    }
    return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS cd_ioctl(PIRP irp, PIO_STACK_LOCATION io)
{
    ULONG code = io->Parameters.DeviceIoControl.IoControlCode;
    ULONG out = io->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = irp->AssociatedIrp.SystemBuffer;

    switch (code) {
    case IOCTL_DISK_GET_DRIVE_GEOMETRY:
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY: {
        FALCON_DISK_GEOMETRY *g = buf;

        if (out < sizeof *g)
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(g, sizeof *g);
        g->BytesPerSector = 2048;
        g->SectorsPerTrack = 1;
        g->TracksPerCylinder = 1;
        g->Cylinders.QuadPart = (LONGLONG)(g_size / 2048);
        g->MediaType = FALCON_RemovableMedia;
        return complete(irp, STATUS_SUCCESS, sizeof *g);
    }
    case IOCTL_STORAGE_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY2:
    case IOCTL_CDROM_CHECK_VERIFY:
        return complete(irp, STATUS_SUCCESS, 0);
    case IOCTL_STORAGE_GET_DEVICE_NUMBER: {
        FALCON_STORAGE_DEVICE_NUMBER *n = buf;

        if (out < sizeof *n)
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        n->DeviceType = FILE_DEVICE_CD_ROM;
        n->DeviceNumber = 0;
        n->PartitionNumber = (ULONG)-1;
        return complete(irp, STATUS_SUCCESS, sizeof *n);
    }
    case IOCTL_CDROM_READ_TOC: {
        FALCON_CDROM_TOC *t = buf;

        if (out < sizeof *t)
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(t, sizeof *t);
        t->FirstTrack = 1;
        t->LastTrack = 1;
        t->Length[0] = (UCHAR)((sizeof *t - 2) >> 8);
        t->Length[1] = (UCHAR)(sizeof *t - 2);
        t->TrackData[0].TrackNumber = 1;
        t->TrackData[0].Control = 0x04;    /* a data track */
        t->TrackData[0].Adr = 0x01;
        return complete(irp, STATUS_SUCCESS, sizeof *t);
    }
    case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME: {
        PMOUNTDEV_NAME name = buf;
        USHORT need = g_cd_name.Length;

        if (out < sizeof(MOUNTDEV_NAME))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        name->NameLength = need;
        if (out < FIELD_OFFSET(MOUNTDEV_NAME, Name) + need)
            return complete(irp, STATUS_BUFFER_OVERFLOW,
                            sizeof(MOUNTDEV_NAME));
        RtlCopyMemory(name->Name, g_cd_name.Buffer, need);
        return complete(irp, STATUS_SUCCESS,
                        FIELD_OFFSET(MOUNTDEV_NAME, Name) + need);
    }
    case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID: {
        PMOUNTDEV_UNIQUE_ID id = buf;
        static const UCHAR uid[] = "falcon-vdisk-0";
        USHORT need = sizeof uid - 1;

        if (out < sizeof(MOUNTDEV_UNIQUE_ID))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        id->UniqueIdLength = need;
        if (out < FIELD_OFFSET(MOUNTDEV_UNIQUE_ID, UniqueId) + need)
            return complete(irp, STATUS_BUFFER_OVERFLOW,
                            sizeof(MOUNTDEV_UNIQUE_ID));
        RtlCopyMemory(id->UniqueId, uid, need);
        return complete(irp, STATUS_SUCCESS,
                        FIELD_OFFSET(MOUNTDEV_UNIQUE_ID, UniqueId) + need);
    }
    default:
        return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS NTAPI vdisk_ioctl(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION io = IoGetCurrentIrpStackLocation(irp);
    vdisk_ext *ext = dev->DeviceExtension;

    return ext->is_cd ? cd_ioctl(irp, io) : ctl_ioctl(irp, io);
}

static void NTAPI vdisk_unload(PDRIVER_OBJECT drv)
{
    (void)drv;
    IoDeleteSymbolicLink(&g_ctl_link);
    if (g_cd_dev != NULL)
        IoDeleteDevice(g_cd_dev);
    if (g_ctl_dev != NULL)
        IoDeleteDevice(g_ctl_dev);
    if (g_pool != NULL)
        ExFreePoolWithTag(g_pool, 'DVaF');
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg);
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    NTSTATUS st;
    int i;

    (void)reg;
    RtlInitUnicodeString(&g_ctl_name, VDISK_CTL_DEVICE);
    RtlInitUnicodeString(&g_ctl_link, VDISK_CTL_LINK);
    RtlInitUnicodeString(&g_cd_name, VDISK_CD_DEVICE);

    st = IoCreateDevice(drv, sizeof(vdisk_ext), &g_ctl_name,
                        FILE_DEVICE_UNKNOWN, 0, FALSE, &g_ctl_dev);
    if (!NT_SUCCESS(st))
        return st;
    ((vdisk_ext *)g_ctl_dev->DeviceExtension)->is_cd = 0;
    st = IoCreateSymbolicLink(&g_ctl_link, &g_ctl_name);
    if (!NT_SUCCESS(st)) {
        IoDeleteDevice(g_ctl_dev);
        return st;
    }

    st = IoCreateDevice(drv, sizeof(vdisk_ext), &g_cd_name, FILE_DEVICE_CD_ROM,
                        FILE_REMOVABLE_MEDIA | FILE_READ_ONLY_DEVICE, FALSE,
                        &g_cd_dev);
    if (!NT_SUCCESS(st)) {
        IoDeleteSymbolicLink(&g_ctl_link);
        IoDeleteDevice(g_ctl_dev);
        return st;
    }
    ((vdisk_ext *)g_cd_dev->DeviceExtension)->is_cd = 1;
    g_cd_dev->Flags |= DO_DIRECT_IO;
    g_cd_dev->Flags &= ~DO_DEVICE_INITIALIZING;

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        drv->MajorFunction[i] = vdisk_create_close;
    drv->MajorFunction[IRP_MJ_READ] = vdisk_read;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = vdisk_ioctl;
    drv->DriverUnload = vdisk_unload;

    DbgPrint("falcon-vdisk: loaded\n");
    return STATUS_SUCCESS;
}
