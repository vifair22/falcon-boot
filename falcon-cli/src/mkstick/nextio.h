/* falcon-boot mkstick: the next-boot marker sector on a disk, and what
 * `next-boot --print` says about either kind of marker (docs/DESIGN.md
 * Section 6.3.1).
 *
 * The half of `next-boot` that touches a disk, apart from the command line,
 * the RTC and /sys/block so a test can drive it over image files: which disk
 * carries the boot partition, reading the marker sector, writing it, and the
 * printed report. The format and the loader's rules are falcon-core's
 * (media/nextboot.h); nextcore.h is the Linux side's pure half.
 *
 * The marker is one whole sector of the disk: 512 bytes, or 4096 on a 4Kn
 * disk, the text in the first 512 and zeros to the end. Every read and write
 * here moves exactly one sector of the disk's own size.
 */

#ifndef FALCON_MKSTICK_NEXTIO_H
#define FALCON_MKSTICK_NEXTIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <common/status.h>
#include <mkstick/disk.h>

#include <media/nextboot.h>

typedef struct {
    char     path[MKS_PATH_MAX];
    uint32_t sector;       /* the disk's logical sector size: the marker's */
    uint64_t marker_lba;   /* in the disk's own sectors */
} next_target;

/* The boot partition's marker on the disk or image at `path`, read as a disk
 * of `sector`-byte sectors and `size` bytes: the partition whose record
 * says boot (records_boot, the loaders' rule) and names a marker sector.
 * 1 with *out filled, or 0 when this disk has none. */
int nextio_probe(const char *path, uint32_t sector, uint64_t size,
                 next_target *out);

/* The same for a path whose size and sector size are the kernel's
 * (disk_stat): a block device, or an image file as 512-byte sectors. */
int nextio_probe_path(const char *path, next_target *out);

/* Which disk's marker. When `disk` is given it is the only disk looked at:
 * cands is not read, and a disk with no boot partition is an error rather
 * than a reason to look further. Otherwise every path in cands is probed and
 * exactly one must carry a boot partition. */
cli_status nextio_resolve(const char *disk, const char *const *cands,
                          size_t ncands, next_target *out);

typedef struct {
    nextboot_sector state;  /* NONE: nothing queued (zero, or not a marker) */
    uint64_t        stamp;  /* VALUE: the RTC's reading when it was set */
    char            value[NEXTBOOT_MAX];
} next_queued;

/* Reads the marker sector and parses it with the loaders' parse. */
cli_status nextio_read(const next_target *t, next_queued *q);

/* Writes a marker naming `value`, dated `now`, or zeros when value is NULL,
 * and reads it back. Refused, with nothing written, when the sector is
 * neither all zeros nor already a marker, or the value would not decode. */
cli_status nextio_write(const next_target *t, const char *value,
                        uint64_t now);

/* `--print`'s report for a sector marker, key=value lines:
 *
 *   source=sector
 *   disk=<path>
 *   lba=<the marker's LBA, in the disk's sectors>
 *   sector_size=<512 or 4096>
 *   queued=yes|no|invalid
 *
 * and when queued=yes:
 *
 *   value=<the value>
 *   set_rtc=<the RTC's reading when it was set, seconds>
 *   age=<now minus set_rtc, seconds; negative when the clock went back>
 *   window=yes|no     (the loader would honour it now)
 *
 * age and window are `unknown` when have_now is 0 (no RTC to read). */
void nextio_print_sector(FILE *f, const next_target *t, const next_queued *q,
                         int have_now, uint64_t now);

/* The report for the UEFI variable: `buf` is efivarfs's file (the
 * attributes, then the value), or NULL when the variable is not set.
 *
 *   source=efivar
 *   queued=yes|no|invalid
 *   value=<the value>        (when queued=yes)
 */
void nextio_print_efivar(FILE *f, const uint8_t *buf, size_t len);

#endif /* FALCON_MKSTICK_NEXTIO_H */
