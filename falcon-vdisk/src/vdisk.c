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
 * falcon-winjump; the driver reads those raw disk sectors off the physical
 * disk and serves them as CD sectors. Nothing of this survives the install:
 * the driver runs only in the installer's WinPE, and the Windows it lays
 * down has its own boot with driver-signing enforcement on.
 *
 * SIGNING. 32-bit Windows enforces no kernel-mode signing, so this loads
 * unsigned there. 64-bit Windows does; falcon patches the served boot
 * configuration to put that one WinPE boot into test-signing mode, and the
 * driver is signed with a throwaway certificate (falcon-cli). Under UEFI
 * Secure Boot on, the firmware refuses test-signing, which is the platform
 * opt-out the loader never works around (DESIGN Section 7).
 *
 * Built for both widths (i686 and x86_64) from this one source, as the hook
 * and winjump are; the loader carries the one matching the boot image.
 */

#include <ntddk.h>

#include "vdiskio.h"

#define VDISK_DEVICE  L"\\Device\\FalconVDisk"
#define VDISK_DOSLINK L"\\DosDevices\\FalconVDisk"

static UNICODE_STRING g_dev_name;
static UNICODE_STRING g_dos_link;

/* The backing store the served CD reads from. Until the extent map and the
 * physical disk it names are wired in, this is a control device only: it
 * accepts the geometry an IOCTL sets, so the userland side and the mount can
 * be brought up a piece at a time. */
static struct {
    ULONGLONG iso_bytes;     /* the medium's declared size */
} g_backing;

static NTSTATUS NTAPI vdisk_create_close(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI vdisk_ioctl(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION io = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;

    (void)dev;
    switch (io->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_FALCON_VDISK_SET_GEOMETRY:
        if (io->Parameters.DeviceIoControl.InputBufferLength >=
                sizeof(struct falcon_vdisk_geometry)) {
            const struct falcon_vdisk_geometry *g = irp->AssociatedIrp.SystemBuffer;

            g_backing.iso_bytes = g->iso_bytes;
            st = STATUS_SUCCESS;
        } else {
            st = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    default:
        break;
    }
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static void NTAPI vdisk_unload(PDRIVER_OBJECT drv)
{
    IoDeleteSymbolicLink(&g_dos_link);
    if (drv->DeviceObject != NULL)
        IoDeleteDevice(drv->DeviceObject);
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg);
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    PDEVICE_OBJECT dev = NULL;
    NTSTATUS st;
    int i;

    (void)reg;
    RtlInitUnicodeString(&g_dev_name, VDISK_DEVICE);
    RtlInitUnicodeString(&g_dos_link, VDISK_DOSLINK);

    st = IoCreateDevice(drv, 0, &g_dev_name, FILE_DEVICE_UNKNOWN, 0, FALSE,
                        &dev);
    if (!NT_SUCCESS(st))
        return st;
    st = IoCreateSymbolicLink(&g_dos_link, &g_dev_name);
    if (!NT_SUCCESS(st)) {
        IoDeleteDevice(dev);
        return st;
    }

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        drv->MajorFunction[i] = vdisk_create_close;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = vdisk_ioctl;
    drv->DriverUnload = vdisk_unload;

    DbgPrint("falcon-vdisk: loaded\n");
    return STATUS_SUCCESS;
}
