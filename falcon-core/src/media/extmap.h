/* falcon-boot: the ISO extent map handed to falcon-hook (docs/DESIGN.md
 * Section 6.4; docs/notes/coverage-roadmap.md W1, L2).
 *
 * The loader boots an ISO's kernel; the kernel's initramfs then looks for
 * the ISO's contents as a block device, which exists only as a file on p2.
 * falcon-hook rebuilds it: a device-mapper linear device over the file's
 * physical runs, carrying the ISO's own filesystem and so its own label.
 * This map is how the hook learns where the runs are.
 *
 * It travels in a volatile EFI variable the loader sets immediately before
 * starting the kernel (FALCON_EXTMAP_VAR under FALCON_EXTMAP_GUID, boot and
 * runtime access), and the hook reads it from efivarfs. A variable rather
 * than a file inside the initrd because the extents belong to this boot, not
 * to the medium: an ISO copied again lands somewhere else, and a signed
 * wrap's embedded initrd could not carry them at all.
 *
 * The partition is named by its GPT unique partition GUID, which is in the
 * UEFI device path of p2's handle; the hook finds the disk whose partition
 * table carries it, so no /dev name is ever guessed. Offsets are in bytes
 * from the start of that partition, the extent map's own convention
 * (fs_extent), and the partition's first LBA is carried so the hook can map
 * the whole disk and check that the table it found agrees.
 *
 * Layout, little-endian, version 1:
 *
 *   0   magic      8 bytes  "FLCNXT01"
 *   8   version    u32      1
 *   12  hdr_len    u32      64
 *   16  part_guid  16 bytes GPT unique partition GUID, as stored on disk
 *   32  part_lba   u64      the partition's first LBA
 *   40  block_size u32      the disk's logical block size, for part_lba
 *   44  n_extents  u32
 *   48  iso_size   u64      bytes; the extents cover it exactly
 *   56  flags      u32      0
 *   60  reserved   u32      0
 *   64  extents    n_extents x { u64 offset, u64 length }, bytes
 *
 * EFI-free and allocation-free, shared by the loader (encode), falcon-hook
 * (decode, inside a foreign initramfs) and the host tests.
 */

#ifndef FALCON_EXTMAP_H
#define FALCON_EXTMAP_H

#include <stddef.h>
#include <stdint.h>

enum {
    EXTMAP_OK          = 0,
    EXTMAP_ERR_BAD_ARG = -1,
    EXTMAP_ERR_NOSPACE = -2,  /* buffer too small */
    EXTMAP_ERR_FORMAT  = -3,  /* not a map, or not one this version reads */
    EXTMAP_ERR_RANGE   = -4   /* extents inconsistent with the header */
};

#define FALCON_EXTMAP_VAR  "FalconIsoMap"
/* {9417f285-55ab-4c5d-9ad4-109e1ec4cb35}, as efivarfs spells it. */
#define FALCON_EXTMAP_GUID_STR "9417f285-55ab-4c5d-9ad4-109e1ec4cb35"
#define FALCON_EXTMAP_GUID_INIT \
    { 0x9417f285u, 0x55abu, 0x4c5du, \
      { 0x9au, 0xd4u, 0x10u, 0x9eu, 0x1eu, 0xc4u, 0xcbu, 0x35u } }

#define EXTMAP_HDR_LEN 64u
#define EXTMAP_EXT_LEN 16u

/* The most runs a map may carry. Firmware may cap one variable at 8 KiB
 * (older OVMF builds do), and 500 runs is 8064 bytes with the header. An ISO
 * copied onto a freshly formatted p2 is one run; one fragmented past this
 * is refused at boot with a message saying to copy it again. */
#define EXTMAP_MAX_EXTENTS 500u

typedef struct {
    uint64_t offset;   /* bytes from the start of the partition */
    uint64_t length;   /* bytes */
} extmap_extent;

typedef struct {
    uint8_t  part_guid[16];
    uint64_t part_lba;
    uint32_t block_size;
    uint32_t n_extents;
    uint64_t iso_size;
} extmap_hdr;

/* Bytes a map of n extents takes. */
size_t extmap_size(uint32_t n_extents);

/* Writes the map. EXTMAP_ERR_RANGE when the extents do not add up to
 * hdr->iso_size, when there are none or more than EXTMAP_MAX_EXTENTS, or
 * when block_size is not a power of two from 512 to 4096. */
int extmap_encode(const extmap_hdr *hdr, const extmap_extent *ext,
                  uint8_t *buf, size_t cap, size_t *len);

/* Reads and checks a map: the magic, the version, the header length, the
 * count against the bytes present, the block size, and that the extents add
 * up to iso_size without overflowing. On success *hdr is filled in and
 * extmap_extent_at reads the runs out of buf. */
int extmap_decode(const uint8_t *buf, size_t len, extmap_hdr *hdr);

/* Extent i of a map extmap_decode accepted. */
void extmap_extent_at(const uint8_t *buf, uint32_t i, extmap_extent *out);

#endif /* FALCON_EXTMAP_H */
