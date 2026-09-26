/* falcon-bios: floppy-emulation El Torito, the part that can be tested
 * (docs/notes/bios-route.md Sections 4.1 and 7.1).
 *
 * Some media boot a BIOS through an El Torito image that emulates a floppy
 * rather than a no-emulation CD: the catalog's media type is 1, 2 or 3
 * (1.2M, 1.44M, 2.88M), the boot image is a raw floppy image, and the BIOS
 * is meant to present it as drive 0x00 and boot its first sector. Haiku's
 * and memtest86+'s BIOS entries are 2.88M and 1.44M floppies.
 *
 * falcon-bios answers this the way it answers a no-emulation CD: a resident
 * INT 13h handler on the drive number the medium expects. It is a memory
 * disk rather than a disk-backed one - the whole image is small (2.88 MB at
 * most) and is loaded into extended memory once, then served with the same
 * INT 15h AH=87h block move the CD's appended region uses. So a floppy's
 * geometry and the CHS-to-LBA arithmetic the handler copies are worked out
 * here, in ordinary host-tested C, and the assembly (vfdres.S) does a
 * multiply, an add and a re-issued move.
 *
 * Hard-disk emulation (media type 4) is not here: no corpus medium needs it,
 * and its geometry has to be inferred from the image's own partition table
 * rather than fixed by media type. It is a later addition.
 */

#ifndef FALCON_BIOS_VFDCORE_H
#define FALCON_BIOS_VFDCORE_H

#include <stddef.h>
#include <stdint.h>

/* El Torito boot media types (1.0 section 2.1), the floppy ones. */
#define VFD_MEDIA_120M 1u
#define VFD_MEDIA_144M 2u
#define VFD_MEDIA_288M 3u

/* A floppy sector is 512 bytes, always. */
#define VFD_SECTOR 512u

typedef struct {
    uint16_t cylinders;      /* 80 for every standard floppy */
    uint8_t  heads;          /* 2 */
    uint8_t  sectors;        /* per track: 15, 18 or 36 */
    uint32_t total_sectors;  /* cylinders * heads * sectors */
    uint8_t  media;          /* the VFD_MEDIA_* it was built for */
    uint8_t  bios_type;      /* AH=08h/AH=15h drive type: 1, 3 or 4 */
} vfd_geom;

enum {
    VFD_OK          = 0,
    VFD_ERR_BAD_ARG = -1,
    VFD_ERR_MEDIA   = -2,   /* not a floppy media type */
    VFD_ERR_SIZE    = -3,   /* the image is larger than the media holds */
    VFD_ERR_RANGE   = -4    /* a CHS address outside the geometry */
};

/* Fills in the geometry for a floppy media type carrying an image of
 * `image_bytes`. The image may be shorter than the media (the tail reads as
 * zeros, as a formatted floppy's unused sectors do) but not longer. */
int vfd_geometry(uint8_t media, uint64_t image_bytes, vfd_geom *g);

/* CHS to a linear sector number, validated against the geometry. Cylinder
 * and head are zero-based, the sector number is one-based, as INT 13h AH=02h
 * passes them. *lba is the 512-byte sector index into the image. */
int vfd_chs_to_lba(const vfd_geom *g, uint16_t cyl, uint8_t head,
                   uint8_t sector, uint32_t *lba);

/* The inverse, for reading a real floppy rather than serving an image
 * (falcon-bios/src/disk/disk.c, DESIGN Section 14.3): the CHS INT 13h AH=02h
 * takes for a linear sector. VFD_ERR_RANGE past the geometry's last sector. */
int vfd_lba_to_chs(const vfd_geom *g, uint32_t lba, uint16_t *cyl,
                   uint8_t *head, uint8_t *sector);

/* A real drive's geometry from what INT 13h AH=08h returns in CH, CL and
 * DH: the last cylinder's index split across CH and CL's top two bits, the
 * sectors per track in CL's low six, the last head's index in DH. `media`
 * and `bios_type` are left zero; they describe an emulated floppy, not a
 * real one. VFD_ERR_RANGE when the answer describes no sectors, or more
 * heads than the geometry can hold. */
int vfd_geometry_int13(uint8_t ch, uint8_t cl, uint8_t dh, vfd_geom *g);

/* How many sectors of a CHS read stay on one track, so the handler serves a
 * multi-sector read in track-sized passes: a read may not cross a track
 * boundary in one INT 13h AH=02h, and real code (a boot sector loading its
 * next stage) asks for a whole track at once. `want` is the count the caller
 * asked for; the result is at most that and at least one. */
int vfd_run_on_track(const vfd_geom *g, uint32_t lba, uint32_t want,
                     uint32_t *n);

#endif /* FALCON_BIOS_VFDCORE_H */
