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
#include <mountmgr.h>

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
#define IOCTL_DISK_CHECK_VERIFY         CTL_CODE(7, 0x0200, 0, 1)
#define IOCTL_CDROM_READ_TOC            CTL_CODE(2, 0x0000, 0, 1)
#define IOCTL_STORAGE_GET_DEVICE_NUMBER CTL_CODE(0x2d, 0x0420, 0, 0)
#define IOCTL_STORAGE_GET_HOTPLUG_INFO  CTL_CODE(0x2d, 0x0305, 0, 0)
/* How the mount path asks a CD its size. UDFS reads its anchor at the disc's
 * last sector, so without a true size it cannot mount and CDFS falls back to
 * the ISO 9660 view, which on a Windows install DVD is a bridge stub with only
 * a readme. These are what give it the size. */
#define IOCTL_DISK_GET_LENGTH_INFO      CTL_CODE(7, 0x0017, 0, 0)
#define IOCTL_STORAGE_READ_CAPACITY     CTL_CODE(0x2d, 0x0450, 0, 1)
#define IOCTL_CDROM_GET_LAST_SESSION    CTL_CODE(2, 0x000E, 0, 1)

typedef struct {
    LARGE_INTEGER Length;
} FALCON_GET_LENGTH_INFORMATION;

typedef struct {
    ULONG Version;
    ULONG Size;
    ULONG BlockLength;
    LARGE_INTEGER NumberOfBlocks;
    LARGE_INTEGER DiskLength;
} FALCON_STORAGE_READ_CAPACITY;

/* A session TOC has the same shape as the plain TOC but names sessions, not
 * tracks; one session, its first track at LBA 0. */
typedef struct {
    UCHAR Length[2];
    UCHAR FirstSession;
    UCHAR LastSession;
    struct {
        UCHAR Reserved;
        UCHAR Control : 4;
        UCHAR Adr : 4;
        UCHAR TrackNumber;
        UCHAR Reserved1;
        UCHAR Address[4];
    } TrackData[1];
} FALCON_CDROM_SESSION_DATA;

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

/* The backing store. Two sources behind one seam: the raw-disk extents winjump
 * hands over (g_disk_handle + g_ext), and a pool buffer stand-in (g_pool) that
 * brought the mount up before the extent path existed. When extents are set
 * they win; the pool path stays for the test tool. */
static PUCHAR    g_pool;
static ULONG     g_last_unhandled;   /* diagnostic: last CD IOCTL we did not handle */
static ULONGLONG g_size;      /* the medium's declared size in bytes */

/* The physical disk the extents read from, a synchronous kernel handle so the
 * servicing thread reads it with ZwReadFile, which builds the lower IRP and
 * MDL itself. */
static HANDLE g_disk_handle;
static struct falcon_vdisk_extent *g_ext;
static ULONG g_ext_count;

/* Read IRPs are serviced on a dedicated thread, never inline in the dispatch
 * routine: a filesystem may issue reads above PASSIVE_LEVEL, where the disk
 * read below (a wait) cannot run. The thread dequeues pended IRPs and does the
 * disk I/O at PASSIVE. */
static LIST_ENTRY g_queue;
static KSPIN_LOCK g_qlock;
static KSEMAPHORE g_qsem;
static PVOID      g_thread;
static BOOLEAN    g_stop;

/* Reads `len` bytes at absolute disk byte offset `off` into `buf`, a nonpaged
 * pool buffer, with ZwReadFile on the synchronous disk handle: it builds the
 * lower IRP and MDL itself, unlike a hand-rolled IoBuildSynchronousFsdRequest,
 * which faulted forwarding a mapped MDL. `off` and `len` are sector-aligned
 * (CDFS/UDFS read 2048-aligned, extents are sector-aligned), so no read-modify
 * is needed on a 512-byte-sector disk. PASSIVE_LEVEL only, where the servicing
 * thread runs. */
static NTSTATUS disk_read(ULONGLONG off, ULONG len, PUCHAR buf)
{
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER at;

    at.QuadPart = (LONGLONG)off;
    return ZwReadFile(g_disk_handle, NULL, NULL, NULL, &iosb, buf, len, &at,
                      NULL);
}

/* Serves `len` bytes of the medium at `off` into `dst`. From the raw-disk
 * extents when they are set, else from the pool buffer, zero-filling any gap
 * between extents and any read past the medium's end (a pressed CD answers its
 * padding). The seam both backings meet at. */
static void backing_read(ULONGLONG off, ULONG len, PUCHAR dst)
{
    if (g_disk_handle != NULL) {
        PUCHAR bounce = ExAllocatePoolWithTag(NonPagedPool, len, 'DVaF');
        ULONG done = 0;

        if (bounce == NULL) {
            RtlZeroMemory(dst, len);
            return;
        }
        while (done < len) {
            ULONG i, run = 0;
            ULONGLONG here = off + done;
            ULONGLONG disk_off = 0;

            for (i = 0; i < g_ext_count; i++) {
                struct falcon_vdisk_extent *e = &g_ext[i];

                if (here >= e->iso_offset && here < e->iso_offset + e->length) {
                    ULONGLONG left = e->iso_offset + e->length - here;
                    ULONG want = len - done;

                    run = (ULONG)(left < want ? left : want);
                    disk_off = e->disk_offset + (here - e->iso_offset);
                    break;
                }
            }
            if (run == 0) {                 /* a gap, or past the last extent */
                RtlZeroMemory(bounce + done, len - done);
                break;
            }
            if (!NT_SUCCESS(disk_read(disk_off, run, bounce + done)))
                RtlZeroMemory(bounce + done, run);
            done += run;
        }
        RtlCopyMemory(dst, bounce, len);
        ExFreePoolWithTag(bounce, 'DVaF');
        return;
    }

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
}

/* Drops the disk reference and the extent map. */
static void backing_release(void)
{
    if (g_disk_handle != NULL) {
        ZwClose(g_disk_handle);
        g_disk_handle = NULL;
    }
    if (g_ext != NULL) {
        ExFreePoolWithTag(g_ext, 'DVaF');
        g_ext = NULL;
    }
    g_ext_count = 0;
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

/* Does the work of one read IRP at PASSIVE_LEVEL and completes it. */
static void serve_read(PIRP irp)
{
    PIO_STACK_LOCATION io = IoGetCurrentIrpStackLocation(irp);
    ULONG len = io->Parameters.Read.Length;
    ULONGLONG off = (ULONGLONG)io->Parameters.Read.ByteOffset.QuadPart;
    PUCHAR dst;

    if (len == 0) {
        complete(irp, STATUS_SUCCESS, 0);
        return;
    }
    /* DO_DIRECT_IO: the caller's pages are described by an MDL. */
    dst = MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority);
    if (dst == NULL) {
        complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        return;
    }
    backing_read(off, len, dst);
    complete(irp, STATUS_SUCCESS, len);
}

/* The servicing thread: waits for queued read IRPs and serves them. */
static void NTAPI vdisk_thread(PVOID ctx)
{
    (void)ctx;
    for (;;) {
        PLIST_ENTRY e;

        KeWaitForSingleObject(&g_qsem, Executive, KernelMode, FALSE, NULL);
        if (g_stop && IsListEmpty(&g_queue))
            break;
        e = ExInterlockedRemoveHeadList(&g_queue, &g_qlock);
        if (e == NULL)
            continue;
        serve_read(CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry));
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS NTAPI vdisk_read(PDEVICE_OBJECT dev, PIRP irp)
{
    vdisk_ext *ext = dev->DeviceExtension;

    if (!ext->is_cd)
        return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    /* Pend and hand to the thread; never touch the lower disk inline. */
    IoMarkIrpPending(irp);
    ExInterlockedInsertTailList(&g_queue, &irp->Tail.Overlay.ListEntry,
                                &g_qlock);
    KeReleaseSemaphore(&g_qsem, 0, 1, FALSE);
    return STATUS_PENDING;
}

/* Tell the mount manager the CD-ROM has arrived: it queries the device
 * (the MOUNTDEV IOCTLs above), assigns a drive letter, and a first open then
 * mounts CDFS on it. Done from an IOCTL rather than DriverEntry so the backing
 * already holds the ISO when the mount reads it. */
static NTSTATUS notify_mountmgr(void)
{
    UNICODE_STRING mm;
    PFILE_OBJECT fobj = NULL;
    PDEVICE_OBJECT mdev = NULL;
    PMOUNTMGR_TARGET_NAME tn;
    KEVENT ev;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    NTSTATUS st;
    ULONG len = FIELD_OFFSET(MOUNTMGR_TARGET_NAME, DeviceName) +
                g_cd_name.Length;

    RtlInitUnicodeString(&mm, MOUNTMGR_DEVICE_NAME);
    st = IoGetDeviceObjectPointer(&mm, FILE_READ_ATTRIBUTES, &fobj, &mdev);
    if (!NT_SUCCESS(st))
        return st;
    tn = ExAllocatePoolWithTag(NonPagedPool, len, 'DVaF');
    if (tn == NULL) {
        ObDereferenceObject(fobj);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    tn->DeviceNameLength = g_cd_name.Length;
    RtlCopyMemory(tn->DeviceName, g_cd_name.Buffer, g_cd_name.Length);
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(
        IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION, mdev, tn, len, NULL, 0,
        FALSE, &ev, &iosb);
    if (irp == NULL) {
        ExFreePoolWithTag(tn, 'DVaF');
        ObDereferenceObject(fobj);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    st = IoCallDriver(mdev, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = iosb.Status;
    }
    ExFreePoolWithTag(tn, 'DVaF');
    ObDereferenceObject(fobj);
    return st;
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
    if (code == IOCTL_FALCON_VDISK_SET_BACKING) {
        const struct falcon_vdisk_backing *b = buf;
        const UCHAR *p = (const UCHAR *)buf + sizeof *b;
        ULONG need, ext_bytes;
        UNICODE_STRING disk;
        OBJECT_ATTRIBUTES oa;
        IO_STATUS_BLOCK iosb;
        HANDLE h;
        NTSTATUS st;

        if (in < sizeof *b)
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        ext_bytes = b->extent_count * (ULONG)sizeof(struct falcon_vdisk_extent);
        need = (ULONG)sizeof *b + b->path_bytes + ext_bytes;
        if (in < need || b->extent_count == 0 || (b->path_bytes & 1) != 0)
            return complete(irp, STATUS_INVALID_PARAMETER, 0);

        disk.Length = b->path_bytes;
        disk.MaximumLength = b->path_bytes;
        disk.Buffer = (PWCH)p;
        InitializeObjectAttributes(&oa, &disk,
                                   OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                   NULL, NULL);
        st = ZwCreateFile(&h, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                          NULL, 0);
        if (!NT_SUCCESS(st))
            return complete(irp, st, 0);

        backing_release();
        g_ext = ExAllocatePoolWithTag(NonPagedPool, ext_bytes, 'DVaF');
        if (g_ext == NULL) {
            ZwClose(h);
            return complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        }
        RtlCopyMemory(g_ext, p + b->path_bytes, ext_bytes);
        g_ext_count = b->extent_count;
        g_disk_handle = h;
        g_size = b->iso_bytes;
        return complete(irp, STATUS_SUCCESS, 0);
    }
    if (code == IOCTL_FALCON_VDISK_ONLINE)
        return complete(irp, notify_mountmgr(), 0);
    if (code == IOCTL_FALCON_VDISK_LASTIOCTL) {
        if (io->Parameters.DeviceIoControl.OutputBufferLength < sizeof(ULONG))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        *(ULONG *)buf = g_last_unhandled;
        return complete(irp, STATUS_SUCCESS, sizeof(ULONG));
    }
    return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

/* An LBA as the minute:second:frame a TOC entry carries: 75 frames a second,
 * and the 2-second (150-frame) lead-in the addressing counts from. */
static void lba_to_msf(ULONG lba, UCHAR addr[4])
{
    ULONG f = lba + 150;

    addr[0] = 0;
    addr[1] = (UCHAR)(f / (75u * 60u));
    addr[2] = (UCHAR)((f / 75u) % 60u);
    addr[3] = (UCHAR)(f % 75u);
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
    case IOCTL_DISK_CHECK_VERIFY:
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
        ULONG sectors = (ULONG)(g_size / 2048);

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
        lba_to_msf(0, t->TrackData[0].Address);
        /* The lead-out (track 0xAA) sits at the end of the data, which is how
         * the disc says how large it is. Without it the mount path cannot find
         * the UDF anchor at the last sector. */
        t->TrackData[1].TrackNumber = 0xAA;
        t->TrackData[1].Control = 0x04;
        t->TrackData[1].Adr = 0x01;
        lba_to_msf(sectors, t->TrackData[1].Address);
        return complete(irp, STATUS_SUCCESS, sizeof *t);
    }
    case IOCTL_DISK_GET_LENGTH_INFO: {
        FALCON_GET_LENGTH_INFORMATION *l = buf;

        if (out < sizeof *l)
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        l->Length.QuadPart = (LONGLONG)g_size;
        return complete(irp, STATUS_SUCCESS, sizeof *l);
    }
    case IOCTL_STORAGE_READ_CAPACITY: {
        FALCON_STORAGE_READ_CAPACITY *c = buf;

        if (out < sizeof *c)
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(c, sizeof *c);
        c->Version = sizeof *c;
        c->Size = sizeof *c;
        c->BlockLength = 2048;
        c->NumberOfBlocks.QuadPart = (LONGLONG)(g_size / 2048);
        c->DiskLength.QuadPart = (LONGLONG)g_size;
        return complete(irp, STATUS_SUCCESS, sizeof *c);
    }
    case IOCTL_CDROM_GET_LAST_SESSION: {
        FALCON_CDROM_SESSION_DATA *s = buf;

        if (out < sizeof *s)
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(s, sizeof *s);
        s->Length[0] = (UCHAR)((sizeof *s - 2) >> 8);
        s->Length[1] = (UCHAR)(sizeof *s - 2);
        s->FirstSession = 1;
        s->LastSession = 1;
        s->TrackData[0].TrackNumber = 1;
        s->TrackData[0].Control = 0x04;
        s->TrackData[0].Adr = 0x01;
        lba_to_msf(0, s->TrackData[0].Address);
        return complete(irp, STATUS_SUCCESS, sizeof *s);
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
        g_last_unhandled = code;
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
    if (g_thread != NULL) {
        g_stop = TRUE;
        KeReleaseSemaphore(&g_qsem, 0, 1, FALSE);
        KeWaitForSingleObject(g_thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_thread);
    }
    IoDeleteSymbolicLink(&g_ctl_link);
    if (g_cd_dev != NULL)
        IoDeleteDevice(g_cd_dev);
    if (g_ctl_dev != NULL)
        IoDeleteDevice(g_ctl_dev);
    if (g_pool != NULL)
        ExFreePoolWithTag(g_pool, 'DVaF');
    backing_release();
}

#ifdef VDISK_SERIAL_TRACE
/* A diagnostic-only marker written straight to COM1 (0x3F8) at DriverEntry, so
 * a rig run can tell from the serial log whether text-mode setup loaded this
 * driver at all through txtsetup.sif's [SCSI.Load], independent of whether the
 * kernel debugger is wired. Never compiled into a shipped driver: it does port
 * I/O the installed system would not want. WRITE_PORT_UCHAR/READ_PORT_UCHAR
 * are HAL routines the driver already links. */
static void serial_trace(const char *s)
{
    WRITE_PORT_UCHAR((PUCHAR)0x3FB, 0x80);   /* LCR: DLAB on */
    WRITE_PORT_UCHAR((PUCHAR)0x3F8, 0x01);   /* divisor low: 115200 */
    WRITE_PORT_UCHAR((PUCHAR)0x3F9, 0x00);   /* divisor high */
    WRITE_PORT_UCHAR((PUCHAR)0x3FB, 0x03);   /* LCR: 8N1, DLAB off */
    WRITE_PORT_UCHAR((PUCHAR)0x3FA, 0xC7);   /* FCR: FIFO on, cleared */
    WRITE_PORT_UCHAR((PUCHAR)0x3FC, 0x03);   /* MCR: DTR, RTS */
    while (*s != '\0') {
        while ((READ_PORT_UCHAR((PUCHAR)0x3FD) & 0x20) == 0)
            ;                                /* wait for THR empty */
        WRITE_PORT_UCHAR((PUCHAR)0x3F8, (UCHAR)*s++);
    }
}
#endif

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg);
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    NTSTATUS st;
    int i;

    (void)reg;
#ifdef VDISK_SERIAL_TRACE
    serial_trace("FALCONVDISK_SCSILOAD_OK\r\n");
#endif
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

    InitializeListHead(&g_queue);
    KeInitializeSpinLock(&g_qlock);
    KeInitializeSemaphore(&g_qsem, 0, MAXLONG);
    {
        HANDLE h;
        OBJECT_ATTRIBUTES oa;

        InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL,
                                  vdisk_thread, NULL);
        if (!NT_SUCCESS(st)) {
            IoDeleteSymbolicLink(&g_ctl_link);
            IoDeleteDevice(g_cd_dev);
            IoDeleteDevice(g_ctl_dev);
            return st;
        }
        ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, NULL, KernelMode,
                                  &g_thread, NULL);
        ZwClose(h);
    }

    DbgPrint("falcon-vdisk: loaded\n");
    return STATUS_SUCCESS;
}
