/* falcon-boot: Device Path protocol (UEFI spec 10.2, 10.3.5).
 *
 * Device paths are byte-packed variable-length node streams: a node's Length
 * is a two-byte array rather than a UINT16 precisely because a node may sit
 * at an odd offset. Never read Length as a UINT16; use the accessors below.
 */

#ifndef FALCON_UEFI_DEVPATH_H
#define FALCON_UEFI_DEVPATH_H

#include <efi/types.h>

#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    { 0x09576e91, 0x6d3f, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct EFI_DEVICE_PATH_PROTOCOL {
    UINT8 Type;
    UINT8 SubType;
    UINT8 Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

/* Node types and subtypes (spec Table 10.x); only the ones we build. */
#define HARDWARE_DEVICE_PATH            0x01
#define HW_VENDOR_DP                    0x04
#define MEDIA_DEVICE_PATH               0x04
#define MEDIA_HARDDRIVE_DP              0x01
#define MEDIA_VENDOR_DP                 0x03
#define MEDIA_FILEPATH_DP               0x04
#define END_DEVICE_PATH_TYPE            0x7F
#define END_ENTIRE_DEVICE_PATH_SUBTYPE  0xFF

typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;
    EFI_GUID                 Guid;
    /* Vendor-defined data may follow (FALCON_VENDOR_DEVICE_PATH). */
} VENDOR_DEVICE_PATH;

/* The vendor node of a device this loader publishes, with eight bytes of
 * vendor data naming the publishing instance. A second falcon-boot started
 * from a program the first one launched is alive at the same time as the
 * first, whose devices stay published under that program. With the GUID
 * alone the two paths would be equal: the firmware refuses a second handle
 * with a path that is already installed, and a match by path would find the
 * other loader's device. An array, not a UINT64, so the node packs to 28
 * bytes with no padding. */
typedef struct {
    VENDOR_DEVICE_PATH vendor;
    UINT8              instance[8];
} FALCON_VENDOR_DEVICE_PATH;

/* The Hard Drive media node (spec 10.3.5.1), as offsets rather than a struct:
 * its UINT64 fields sit at 8 and 16 behind a 4-byte header and a UINT32, and
 * the node is 42 bytes, so no C layout matches it without packing. */
#define HD_DP_LEN            42u
#define HD_DP_OFF_START       8u   /* PartitionStart, LBA */
#define HD_DP_OFF_SIG        24u   /* PartitionSignature[16] */
#define HD_DP_OFF_SIGTYPE    41u   /* SignatureType */
#define HD_DP_SIGTYPE_GUID    2u

static inline UINT16 dp_node_len(const EFI_DEVICE_PATH_PROTOCOL *node)
{
    return (UINT16)(node->Length[0] | ((UINT16)node->Length[1] << 8));
}

static inline void dp_set_node_len(EFI_DEVICE_PATH_PROTOCOL *node, UINT16 len)
{
    node->Length[0] = (UINT8)(len & 0xFFu);
    node->Length[1] = (UINT8)(len >> 8);
}

static inline void dp_set_end(EFI_DEVICE_PATH_PROTOCOL *node)
{
    node->Type = END_DEVICE_PATH_TYPE;
    node->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    dp_set_node_len(node, (UINT16)sizeof(*node));
}

static inline BOOLEAN dp_is_end(const EFI_DEVICE_PATH_PROTOCOL *node)
{
    return (BOOLEAN)(node->Type == END_DEVICE_PATH_TYPE &&
                     node->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE);
}

/* Byte length of a whole path, end node included. A malformed node (length
 * below the header size) stops the walk, so a corrupt path cannot spin. */
static inline UINTN dp_total_len(const EFI_DEVICE_PATH_PROTOCOL *dp)
{
    const UINT8 *p = (const UINT8 *)dp;
    UINTN total = 0;

    for (;;) {
        const EFI_DEVICE_PATH_PROTOCOL *node =
            (const EFI_DEVICE_PATH_PROTOCOL *)p;
        UINT16 len = dp_node_len(node);

        if (len < sizeof(*node))
            return 0;
        total += len;
        if (dp_is_end(node))
            return total;
        p += len;
    }
}

#endif /* FALCON_UEFI_DEVPATH_H */
