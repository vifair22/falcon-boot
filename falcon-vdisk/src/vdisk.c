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
 * CD sectors. A pool buffer the userland side fills through an IOCTL is the
 * fallback the test tool uses instead, enough on its own to bring the device
 * and its CDFS mount up. `backing_read` is the seam between the two.
 *
 * Nothing of this survives the install: the driver runs only in the
 * installer's WinPE. 32-bit WinPE enforces no kernel-mode code signing, so
 * the unsigned driver loads there; 64-bit WinPE enforces it and does not
 * load the driver at all (DESIGN Section 6.3).
 *
 * Built for both widths from this one source, as the hook and winjump are.
 */

#include <ntddk.h>
#include <mountdev.h>
#include <mountmgr.h>

#include "vdiskio.h"

#include <media/extmap.h>
#include <winjumpcore.h>

/* Piece 3 (DESIGN Section 6.3): the boot-start backing, for Windows 2000/XP
 * text-mode setup, which has no WinPE and so no winjump to hand the driver its
 * disk and extents through SET_BACKING. The loader (win_xp_boot) instead patches
 * an extent map into this driver's served copy: it scans the .sys for the magic
 * below and writes map_len and the extmap (falcon-core/src/media/extmap) after
 * it. At load the driver finds the stick itself from that map - the same disk
 * match and run conversion winjump does in userland (winjumpcore) - and reads
 * the ISO from the stick's real disk. map_len != 0 means the loader configured
 * it; a plain winjump build leaves it zero and the SET_BACKING path is used.
 *
 * The magic is a distinctive 16 bytes present at build time so the loader can
 * find the struct by scanning, without symbols. */
#define VDISK_BAKE_MAGIC { 'F','A','L','C','O','N','v','d','B','A','K','E', \
                           '0','0','0','1' }
#define VDISK_BAKE_CAP 4096u
static struct {
    unsigned char magic[16];
    unsigned long map_len;
    unsigned char map[VDISK_BAKE_CAP];
} g_bake = { VDISK_BAKE_MAGIC, 0, { 0 } };

#ifdef VDISK_SERIAL_TRACE
static void serial_trace(const char *s);
static void trace_object_dir(const WCHAR *path);
#endif

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
/* Piece 3: the standard CD-ROM device name the boot-device ARC path resolves
 * to, aliased to this device so 2000/XP text-mode setup's kernel finds its boot
 * CD (else 0x7B). Created only on the baked path; g_cdrom0 records whether it
 * was, so unload removes it. */
static UNICODE_STRING g_cdrom0_name;
static BOOLEAN g_cdrom0;
/* Piece 3: the ARC name of the boot CD, which no real CD-ROM registers here
 * (the medium is served over INT 13h), so this driver creates it and points it
 * at this device, else the kernel's boot-device open finds no such name (0x7B).
 * g_arc records whether it was, so unload removes it. */
static UNICODE_STRING g_arc_name;
static BOOLEAN g_arc;
/* The boot CD's ARC name by the fixed served drive, created unconditionally as
 * well: Windows 2000 keeps no SystemBootDevice for bind_boot_device to read,
 * and names its boot CD by the BIOS drive falcon-bios serves on, 0xE0 = 224
 * (falcon-bios/src/iso/vcd.h VCD_DRIVE) - the same name XP records, so cdrom(224)
 * covers 2000 too. */
static UNICODE_STRING g_arc0_name;
static BOOLEAN g_arc0;
/* Signalled once the backing is resolved (or given up on), so a read waits for
 * the stick to be found rather than serving zeros: this is what makes the
 * shipped build reliable without the trace build's serial delays. The wait is
 * on the read-servicing thread, never DriverEntry, so it does not stall the
 * boot-start sequence. */
static KEVENT g_backing_ready;
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
    /* Wait for the backing to be resolved before serving, so the kernel's boot
     * mount and setup's CD reads never get zeros because the stick was not
     * found yet. Signalled at once on the winjump path (SET_BACKING). At
     * PASSIVE on this thread, so it does not stall the boot-start sequence. */
    KeWaitForSingleObject(&g_backing_ready, Executive, KernelMode, FALSE, NULL);
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

/* An unsigned integer as decimal WCHARs into `p` (no kernel swprintf here),
 * returning the count. For the \Device\Harddisk<n>\DR<n> path (piece 3). */
static int put_udec(WCHAR *p, unsigned v)
{
    WCHAR t[12];
    int n = 0, i;

    if (v == 0) {
        p[0] = L'0';
        return 1;
    }
    while (v != 0) {
        t[n++] = (WCHAR)(L'0' + v % 10u);
        v /= 10u;
    }
    for (i = 0; i < n; i++)
        p[i] = t[n - 1 - i];
    return n;
}

/* Opens \Device\Harddisk<n>\DR<n> for reading, into *out. The physical disk,
 * not a partition: piece 3 reads the ISO by absolute disk offset. */
static NTSTATUS open_physical_disk(unsigned n, PHANDLE out)
{
    static const WCHAR pre[] = L"\\Device\\Harddisk";
    static const WCHAR mid[] = L"\\DR";
    WCHAR buf[44];
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    int i = 0, j;

    for (j = 0; pre[j] != 0; j++)
        buf[i++] = pre[j];
    i += put_udec(buf + i, n);
    for (j = 0; mid[j] != 0; j++)
        buf[i++] = mid[j];
    i += put_udec(buf + i, n);
    buf[i] = 0;

    us.Buffer = buf;
    us.Length = (USHORT)((size_t)i * sizeof(WCHAR));
    us.MaximumLength = (USHORT)sizeof buf;
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);
    return ZwCreateFile(out, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                        FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                        NULL, 0);
}

/* A raw read of an open disk for winjumpcore's match: whole sectors, which
 * is what wj_disk_holds hands down. */
static int disk_sector_read(void *ctx, uint64_t off, uint32_t len, void *buf)
{
    HANDLE h = *(HANDLE *)ctx;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER at;

    at.QuadPart = (LONGLONG)off;
    if (!NT_SUCCESS(ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, len, &at,
                               NULL)) ||
        iosb.Information != len)
        return -1;
    return 0;
}

/* The match's bounce buffer and record scratch: kilobytes, and this runs on
 * a system thread's kernel stack. */
static wj_scratch g_match;

/* Piece 3 (DESIGN Section 6.3): resolve the baked extent map to a live
 * backing, on its own thread so notify_mountmgr's wait (and the CDFS mount and
 * reads it triggers, serviced by the other thread) do not deadlock. Finds the
 * stick by the same match winjump makes (the ISO library's /.falcon record at
 * the map's LBA, winjumpcore.h), retrying while the disk stack comes up, then
 * converts the map's partition-relative runs to absolute disk extents and
 * brings the CD online. A plain winjump build leaves g_bake.map_len zero and
 * this returns at once; SET_BACKING is used instead. */
static void NTAPI baked_resolve(PVOID ctx)
{
    extmap_hdr hdr;
    int tries, n;
    uint32_t got = 0;
    ULONG bytes;

    (void)ctx;
    if (g_bake.map_len == 0 || g_bake.map_len > VDISK_BAKE_CAP)
        goto done;
    if (extmap_decode(g_bake.map, g_bake.map_len, &hdr) != EXTMAP_OK)
        goto done;

    for (tries = 0; tries < 1200 && g_disk_handle == NULL; tries++) {
        for (n = 0; n < 32; n++) {
            HANDLE h;

            if (!NT_SUCCESS(open_physical_disk((unsigned)n, &h)))
                continue;
            if (wj_disk_holds(&hdr, disk_sector_read, &h, &g_match) ==
                WJ_OK) {
                g_disk_handle = h;                      /* keep it open */
                break;
            }
            ZwClose(h);
        }
        if (g_disk_handle == NULL) {
            LARGE_INTEGER d;

            /* Poll fast (25 ms) so the scan catches the stick the instant
             * disk.sys enumerates it, before the kernel needs the boot CD or
             * setup scans for a CD-ROM - the window is narrow and a slow retry
             * loses the race. 1200 tries is still ~30 s of patience. */
            d.QuadPart = -250000;                       /* 25 ms, relative */
            KeDelayExecutionThread(KernelMode, FALSE, &d);
        }
    }
    if (g_disk_handle == NULL)
        goto done;      /* no stick found: reads serve zeros, setup 0x7B's */

    bytes = hdr.n_extents * (ULONG)sizeof(struct falcon_vdisk_extent);
    g_ext = ExAllocatePoolWithTag(NonPagedPool, bytes, 'DVaF');
    if (g_ext == NULL) {
        backing_release();
        goto done;
    }
    /* wj_extent is byte-identical to falcon_vdisk_extent, so the runs decode
     * straight into g_ext. */
    if (wj_backing_extents(&hdr, g_bake.map, g_bake.map_len, (wj_extent *)g_ext,
                           hdr.n_extents, &got) != WJ_OK || got == 0) {
        backing_release();
        goto done;
    }
    g_ext_count = got;
    g_size = hdr.iso_size;
#ifdef VDISK_SERIAL_TRACE
    serial_trace("FALCONVDISK_BACKING_OK\r\n");
    /* \ArcName again, now the disk stack is up: shows what the kernel has
     * registered by this point, to compare against the boot ARC name. */
    trace_object_dir(L"\\ArcName");
#endif
    (void)notify_mountmgr();

done:
    /* Release any reads that were waiting for the backing - whether it was set
     * or given up on (they then serve zeros, which is the honest failure). */
    KeSetEvent(&g_backing_ready, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
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
    if (g_cdrom0)
        IoDeleteSymbolicLink(&g_cdrom0_name);
    if (g_arc)
        IoDeleteSymbolicLink(&g_arc_name);
    if (g_arc0)
        IoDeleteSymbolicLink(&g_arc0_name);
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

/* Trace a UNICODE_STRING as ASCII to COM1, one line. */
static void serial_trace_ustr(const UNICODE_STRING *u)
{
    USHORT i, n = (USHORT)(u->Length / sizeof(WCHAR));

    for (i = 0; i < n; i++) {
        WCHAR c = u->Buffer[i];
        char two[2];

        two[0] = (c >= 32 && c < 127) ? (char)c : '?';
        two[1] = '\0';
        serial_trace(two);
    }
    serial_trace("\r\n");
}

/* The object-directory query the boot-device diagnostic needs; mingw's ddk
 * headers do not declare it. Exported by ntoskrnl. */
typedef struct {
    UNICODE_STRING Name;
    UNICODE_STRING TypeName;
} FALCON_OBJDIR_INFO;
NTSTATUS NTAPI ZwOpenDirectoryObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
NTSTATUS NTAPI ZwQueryDirectoryObject(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN,
                                      PULONG, PULONG);

/* Diagnostic: list an object directory (e.g. \ArcName, whose links name the
 * boot device the 2000/XP route must resolve to) to COM1, one entry a line,
 * between BEGIN/END markers. */
static void trace_object_dir(const WCHAR *path)
{
    static UCHAR buf[1024];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    ULONG ctx = 0, ret;
    BOOLEAN first = TRUE;

    RtlInitUnicodeString(&name, path);
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);
    if (!NT_SUCCESS(ZwOpenDirectoryObject(&h, 1u /*DIRECTORY_QUERY*/, &oa))) {
        serial_trace("FALCONVDISK_ARCDIR_OPEN_FAIL\r\n");
        return;
    }
    serial_trace("FALCONVDISK_ARCDIR_BEGIN\r\n");
    for (;;) {
        FALCON_OBJDIR_INFO *di = (FALCON_OBJDIR_INFO *)buf;

        if (!NT_SUCCESS(ZwQueryDirectoryObject(h, buf, sizeof buf, TRUE, first,
                                               &ctx, &ret)))
            break;
        first = FALSE;
        serial_trace_ustr(&di->Name);
    }
    serial_trace("FALCONVDISK_ARCDIR_END\r\n");
    ZwClose(h);
}

/* Diagnostic: list every value of a registry key to COM1 (name = string), to
 * find where 2000 keeps its boot-device ARC name when XP's SystemBootDevice is
 * absent. */
static void trace_reg_values(const WCHAR *keypath)
{
    static UCHAR buf[1024];
    KEY_VALUE_FULL_INFORMATION *vi = (KEY_VALUE_FULL_INFORMATION *)buf;
    UNICODE_STRING kp, nm, v;
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    ULONG idx = 0, got;

    RtlInitUnicodeString(&kp, keypath);
    InitializeObjectAttributes(&oa, &kp, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&h, KEY_READ, &oa))) {
        serial_trace("FALCONVDISK_VALS_OPEN_FAIL\r\n");
        return;
    }
    serial_trace("FALCONVDISK_VALS_BEGIN\r\n");
    for (idx = 0;; idx++) {
        if (!NT_SUCCESS(ZwEnumerateValueKey(h, idx, KeyValueFullInformation, buf,
                                            sizeof buf, &got)))
            break;
        nm.Buffer = vi->Name;
        nm.Length = (USHORT)vi->NameLength;
        nm.MaximumLength = nm.Length;
        serial_trace_ustr(&nm);
        if (vi->Type == REG_SZ && vi->DataLength >= sizeof(WCHAR)) {
            v.Buffer = (PWCH)((PUCHAR)buf + vi->DataOffset);
            v.Length = (USHORT)(vi->DataLength - sizeof(WCHAR));
            v.MaximumLength = v.Length;
            serial_trace("  = ");
            serial_trace_ustr(&v);
        }
    }
    serial_trace("FALCONVDISK_VALS_END\r\n");
    ZwClose(h);
}

/* Diagnostic: trace one registry value (a string) to COM1, framed by the value
 * name, to find the boot-device ARC name setupldr recorded. */
static void trace_reg_value(const WCHAR *keypath, const WCHAR *valname)
{
    static UCHAR buf[512];
    UNICODE_STRING kp, vn, v;
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    ULONG got = 0;
    KEY_VALUE_PARTIAL_INFORMATION *pi = (KEY_VALUE_PARTIAL_INFORMATION *)buf;

    RtlInitUnicodeString(&kp, keypath);
    InitializeObjectAttributes(&oa, &kp, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&h, KEY_READ, &oa))) {
        serial_trace("FALCONVDISK_REG_KEY_FAIL\r\n");
        return;
    }
    RtlInitUnicodeString(&vn, valname);
    serial_trace("FALCONVDISK_REGVAL ");
    serial_trace_ustr(&vn);
    if (NT_SUCCESS(ZwQueryValueKey(h, &vn, KeyValuePartialInformation, buf,
                                   sizeof buf, &got)) &&
        pi->DataLength >= sizeof(WCHAR)) {
        v.Buffer = (PWCH)pi->Data;
        v.Length = (USHORT)(pi->DataLength - sizeof(WCHAR));   /* drop the NUL */
        v.MaximumLength = v.Length;
        serial_trace_ustr(&v);
    } else {
        serial_trace("(missing)\r\n");
    }
    ZwClose(h);
}
#endif

/* Binds this CD as the kernel's boot device (piece 3, the 2000/XP 0x7B). The
 * kernel resolves its boot device through an ARC name, but no real device
 * registers one for the INT-13h-served medium, so this driver must. The name
 * is not a fixed cdrom(0): setupldr records it as the BIOS drive number the
 * loader served on (measured multi(0)disk(0)cdrom(224), 224 = 0xE0), so it is
 * READ from SystemBootDevice in the registry and \ArcName\<that> is pointed at
 * this device. Returns TRUE if the link was made. */
static BOOLEAN bind_boot_device(void)
{
    static UCHAR buf[512];
    static WCHAR link[300];
    static const WCHAR pre[] = L"\\ArcName\\";
    KEY_VALUE_PARTIAL_INFORMATION *pi = (KEY_VALUE_PARTIAL_INFORMATION *)buf;
    UNICODE_STRING kp, vn;
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    ULONG got = 0, arclen, i, n = 0;
    const WCHAR *arc;

    RtlInitUnicodeString(&kp,
        L"\\Registry\\Machine\\SYSTEM\\ControlSet001\\Control");
    InitializeObjectAttributes(&oa, &kp, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&h, KEY_READ, &oa)))
        return FALSE;
    RtlInitUnicodeString(&vn, L"SystemBootDevice");
    if (!NT_SUCCESS(ZwQueryValueKey(h, &vn, KeyValuePartialInformation, buf,
                                    sizeof buf, &got)) ||
        pi->DataLength < 2u * sizeof(WCHAR)) {
        ZwClose(h);
        return FALSE;
    }
    ZwClose(h);

    arc = (const WCHAR *)pi->Data;
    arclen = pi->DataLength / sizeof(WCHAR);
    while (arclen > 0 && arc[arclen - 1] == 0)
        arclen--;                                   /* drop the NUL(s) */

    for (i = 0; pre[i] != 0; i++)
        link[n++] = pre[i];
    for (i = 0; i < arclen && n < 299u; i++)
        link[n++] = arc[i];
    link[n] = 0;
    RtlInitUnicodeString(&g_arc_name, link);        /* link is static */
    return NT_SUCCESS(IoCreateSymbolicLink(&g_arc_name, &g_cd_name));
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg);
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    NTSTATUS st;
    int i;

    (void)reg;
#ifdef VDISK_SERIAL_TRACE
    serial_trace("FALCONVDISK_SCSILOAD_OK\r\n");
    /* What the kernel's boot-device ARC name resolves through: the links here
     * name the boot device the route must make resolve to this CD. */
    trace_object_dir(L"\\ArcName");
    /* The boot-device ARC name setupldr recorded, the string this route must
     * make resolve to this CD: XP keeps it here. */
    trace_reg_value(L"\\Registry\\Machine\\SYSTEM\\ControlSet001\\Control",
                    L"FirmwareBootDevice");
    trace_reg_value(L"\\Registry\\Machine\\SYSTEM\\ControlSet001\\Control",
                    L"SystemBootDevice");
    trace_reg_value(L"\\Registry\\Machine\\SYSTEM\\Setup", L"SystemPartition");
    /* Where Windows 2000 keeps it, since SystemBootDevice is absent there. */
    trace_reg_values(L"\\Registry\\Machine\\SYSTEM\\ControlSet001\\Control");
    trace_reg_values(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control");
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

    /* Piece 3, boot-device binding (NOT yet complete). On the baked (2000/XP)
     * path the kernel bugchecks 0x7B with OBJECT_NAME_NOT_FOUND after this
     * driver has loaded and resolved its backing: it cannot resolve the ARC
     * name of its boot device to this CD. Aliasing the standard CD-ROM device
     * name to this device is a first step: \Device\CdRom0 is FREE and this
     * link is created (the CDROM0_LINKED marker), yet 0x7B still follows -
     * so the boot ARC path the kernel resolves is NOT cdrom(0). The
     * remaining work is to find the path setupldr set (enumerate \ArcName,
     * or read how it names an El Torito no-emulation CD served over
     * INT 13h) and make it resolve here (DESIGN Section 6.3). A WinPE build
     * leaves map_len zero and is untouched. */
    if (g_bake.map_len != 0) {
        extmap_hdr bh;

        /* Report the medium's size at once, from the baked map, so the CD
         * answers CHECK_VERIFY and geometry as present the instant it is
         * created - text-mode setup scans for a CD-ROM before the backing
         * thread has finished finding the stick, and a device reporting no
         * media is not counted. The reads come later, when the backing is up. */
        if (extmap_decode(g_bake.map, g_bake.map_len, &bh) == EXTMAP_OK)
            g_size = bh.iso_size;

        RtlInitUnicodeString(&g_cdrom0_name, L"\\Device\\CdRom0");
        g_cdrom0 = NT_SUCCESS(IoCreateSymbolicLink(&g_cdrom0_name, &g_cd_name));
        /* Bind this CD as the kernel's boot device by the ARC name setupldr
         * recorded (read from the registry, not assumed - it is the served BIOS
         * drive number, e.g. cdrom(224) for 0xE0). This clears XP's 0x7B. */
        g_arc = bind_boot_device();
        /* And cdrom(224) directly, for Windows 2000, which records no
         * SystemBootDevice: the boot CD is the drive falcon-bios serves on
         * (0xE0 = 224), the same name XP's SystemBootDevice holds. */
        RtlInitUnicodeString(&g_arc0_name,
                             L"\\ArcName\\multi(0)disk(0)cdrom(224)");
        g_arc0 = NT_SUCCESS(IoCreateSymbolicLink(&g_arc0_name, &g_cd_name));
#ifdef VDISK_SERIAL_TRACE
        serial_trace(g_cdrom0 ? "FALCONVDISK_CDROM0_LINKED\r\n"
                              : "FALCONVDISK_CDROM0_EXISTS\r\n");
        serial_trace(g_arc ? "FALCONVDISK_ARC_LINKED\r\n"
                           : "FALCONVDISK_ARC_FAIL\r\n");
#endif
    }

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        drv->MajorFunction[i] = vdisk_create_close;
    drv->MajorFunction[IRP_MJ_READ] = vdisk_read;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = vdisk_ioctl;
    drv->DriverUnload = vdisk_unload;

    InitializeListHead(&g_queue);
    KeInitializeSpinLock(&g_qlock);
    KeInitializeSemaphore(&g_qsem, 0, MAXLONG);
    /* Reads wait on this until the backing is resolved. On the winjump path
     * (no baked map) there is no resolve thread, so signal it now; SET_BACKING
     * fills the backing before the CD is brought online. */
    KeInitializeEvent(&g_backing_ready, NotificationEvent, FALSE);
    if (g_bake.map_len == 0)
        KeSetEvent(&g_backing_ready, IO_NO_INCREMENT, FALSE);
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

    /* Piece 3: when the loader baked an extent map in (2000/XP text-mode
     * setup), resolve the backing on its own detached thread. It must NOT block
     * DriverEntry: a boot-start driver that does not return holds up the rest of
     * the boot-start sequence, so disk.sys never enumerates the stick the
     * resolve is waiting for, and the deadlock ends in an unmountable boot
     * volume (0xED). Fire and forget; it terminates itself. A winjump build
     * leaves the map empty and this is skipped, SET_BACKING taking over. */
    if (g_bake.map_len != 0) {
        HANDLE rh;
        OBJECT_ATTRIBUTES roa;

        InitializeObjectAttributes(&roa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(PsCreateSystemThread(&rh, THREAD_ALL_ACCESS, &roa, NULL,
                                            NULL, baked_resolve, NULL)))
            ZwClose(rh);
    }

    DbgPrint("falcon-vdisk: loaded\n");
    return STATUS_SUCCESS;
}
