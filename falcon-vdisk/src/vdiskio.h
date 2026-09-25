/* falcon-vdisk: the interface between falcon-winjump (userland) and the
 * kernel driver. Shared by both.
 *
 * CTL_CODE is <winioctl.h>'s in userland and <wdm.h>'s in the driver; the
 * device type is FILE_DEVICE_UNKNOWN (0x22), the functions are above the
 * 0x800 vendor base, METHOD_BUFFERED (0), FILE_ANY_ACCESS (0).
 */

#ifndef FALCON_VDISK_IO_H
#define FALCON_VDISK_IO_H

/* The medium's size, and the point where the raw-disk extents that back it
 * will be told to the driver (a later revision). For now it also brings the
 * backing store into being at that size. */
#define IOCTL_FALCON_VDISK_SET_GEOMETRY \
    CTL_CODE(0x00000022, 0x800, 0, 0)

/* Feed a range of the medium's bytes into the backing store, for bringing
 * the device and its mount up before the raw-disk read path exists. The
 * input buffer is a falcon_vdisk_write header followed by its bytes. */
#define IOCTL_FALCON_VDISK_WRITE \
    CTL_CODE(0x00000022, 0x801, 0, 0)

/* Once the backing holds the medium, bring the CD-ROM online: the driver
 * tells the mount manager it has arrived, which assigns a drive letter and
 * lets CDFS mount it. Called after the writes, so the mount reads real
 * bytes. */
#define IOCTL_FALCON_VDISK_ONLINE \
    CTL_CODE(0x00000022, 0x802, 0, 0)

/* Diagnostic: the last CD-ROM IOCTL the driver did not handle, so a failed
 * CDFS mount can be traced to the missing one. */
#define IOCTL_FALCON_VDISK_LASTIOCTL \
    CTL_CODE(0x00000022, 0x803, 0, 0)

/* The real backing: name a physical disk and the ISO's byte extents on it,
 * and the driver serves CD sectors by reading those disk sectors directly. No
 * copy into pool, so an ISO of any size is served from where it already sits.
 * This is the path winjump uses; SET_GEOMETRY/WRITE is only the pool stand-in
 * that brought the mount up. The input buffer is a falcon_vdisk_backing
 * header, then `path_bytes` of a UTF-16 NT device path (no NUL, e.g.
 * "\Device\Harddisk0\DR0"), then `extent_count` falcon_vdisk_extent. */
#define IOCTL_FALCON_VDISK_SET_BACKING \
    CTL_CODE(0x00000022, 0x804, 0, 0)

struct falcon_vdisk_geometry {
    unsigned long long iso_bytes;
};

struct falcon_vdisk_write {
    unsigned long long offset;
    unsigned long long length;
    /* `length` bytes follow. */
};

/* One contiguous run of the served ISO. `iso_offset` is the byte position
 * within the medium, `disk_offset` the absolute byte position on the disk,
 * both a multiple of the disk's sector size; `length` is the run's bytes.
 * Runs are ordered by iso_offset and any gap between them reads as zero. */
struct falcon_vdisk_extent {
    unsigned long long iso_offset;
    unsigned long long disk_offset;
    unsigned long long length;
};

struct falcon_vdisk_backing {
    unsigned long long iso_bytes;    /* the medium's declared size */
    unsigned short     path_bytes;   /* UTF-16 device-path length, no NUL */
    unsigned short     reserved;
    unsigned long      extent_count;
    /* path_bytes of UTF-16, then extent_count falcon_vdisk_extent. */
};

#endif /* FALCON_VDISK_IO_H */
