/* falcon-vdisk: the interface between falcon-winjump (userland) and the
 * kernel driver. Shared by both.
 */

#ifndef FALCON_VDISK_IO_H
#define FALCON_VDISK_IO_H

/* CTL_CODE, spelled out so this header needs only <winioctl.h>'s macro or the
 * DDK's: FILE_DEVICE_UNKNOWN (0x22), a custom function base above 0x800,
 * METHOD_BUFFERED (0), FILE_ANY_ACCESS (0). */
#define IOCTL_FALCON_VDISK_SET_GEOMETRY \
    CTL_CODE(0x00000022, 0x800, 0 /*METHOD_BUFFERED*/, 0 /*FILE_ANY_ACCESS*/)

/* What the loader tells the driver about the medium. The extent map and the
 * physical disk that backs it follow this in later revisions; for now it is
 * the declared size, enough to bring the device and its mount up. */
struct falcon_vdisk_geometry {
    unsigned long long iso_bytes;
};

#endif /* FALCON_VDISK_IO_H */
