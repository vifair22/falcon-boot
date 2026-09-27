/* falcon-bios: the decisions of a partition or disk chain. See vbrcore.h. */

#include <mbr/vbrcore.h>

#include <lib/string.h>

#define BPB_HIDDEN        0x1Cu
#define BPB_DRIVE_FAT     0x24u   /* also NTFS's */
#define BPB_SIG_FAT       0x26u
#define BPB_DRIVE_FAT32   0x40u
#define BPB_SIG_FAT32     0x42u

#define ENTRY_STATUS 0u
#define ENTRY_TYPE   4u
#define ENTRY_START  8u

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void vbr_fix_bpb(uint8_t *sec, uint64_t start_lba, uint8_t dl, vbr_fix *out)
{
    size_t drive_off = 0;
    int has_drive = 0;

    memset(out, 0, sizeof *out);
    out->bpb = vbr_bpb_kind(sec);
    if (out->bpb == VBR_BPB_NONE)
        return;

    out->hidden_was = get_le32(sec + BPB_HIDDEN);
    if (start_lba > 0xFFFFFFFFu) {
        out->hidden_too_far = 1;
    } else if (out->hidden_was != (uint32_t)start_lba) {
        put32(sec + BPB_HIDDEN, (uint32_t)start_lba);
        out->hidden_patched = 1;
    }

    switch (out->bpb) {
    case VBR_BPB_FAT:
        drive_off = BPB_DRIVE_FAT;
        has_drive = sec[BPB_SIG_FAT] == 0x28u || sec[BPB_SIG_FAT] == 0x29u;
        break;
    case VBR_BPB_FAT32:
        drive_off = BPB_DRIVE_FAT32;
        has_drive = sec[BPB_SIG_FAT32] == 0x28u || sec[BPB_SIG_FAT32] == 0x29u;
        break;
    case VBR_BPB_NTFS:
        drive_off = BPB_DRIVE_FAT;
        has_drive = 1;
        break;
    case VBR_BPB_NONE:
    default:
        break;
    }
    out->drive_was = has_drive ? sec[drive_off] : 0;
    if (has_drive && sec[drive_off] != dl) {
        sec[drive_off] = dl;
        out->drive_patched = 1;
    }
}

static int is_extended(uint8_t type)
{
    return type == 0x05u || type == 0x0Fu || type == 0x85u;
}

int vbr_mark_active(uint8_t *mbr, unsigned slot, uint64_t start_lba)
{
    uint8_t *e;
    unsigned i;

    if (slot > 3u)
        return -1;
    e = mbr + VBR_TABLE_OFF + VBR_ENTRY_LEN * slot;
    if (e[ENTRY_TYPE] == 0 || is_extended(e[ENTRY_TYPE]) ||
        (uint64_t)get_le32(e + ENTRY_START) != start_lba)
        return -1;
    for (i = 0; i < 4u; i++)
        mbr[VBR_TABLE_OFF + VBR_ENTRY_LEN * i + ENTRY_STATUS] = 0;
    e[ENTRY_STATUS] = 0x80u;
    return 0;
}
