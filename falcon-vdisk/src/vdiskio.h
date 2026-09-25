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

struct falcon_vdisk_geometry {
    unsigned long long iso_bytes;
};

struct falcon_vdisk_write {
    unsigned long long offset;
    unsigned long long length;
    /* `length` bytes follow. */
};

#endif /* FALCON_VDISK_IO_H */
