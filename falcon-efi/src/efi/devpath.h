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
#define MEDIA_DEVICE_PATH               0x04
#define MEDIA_VENDOR_DP                 0x03
#define MEDIA_FILEPATH_DP               0x04
#define END_DEVICE_PATH_TYPE            0x7F
#define END_ENTIRE_DEVICE_PATH_SUBTYPE  0xFF

typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;
    EFI_GUID                 Guid;
    /* Vendor-defined data may follow; ours carries none. */
} VENDOR_DEVICE_PATH;

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
