/* falcon-bios: the decisions of a partition or disk chain. See vbrcore.h. */

#include <mbr/vbrcore.h>

#include <lib/string.h>

#define BPB_BYTES_PER_SEC 0x0Bu
#define BPB_SEC_PER_CLUS  0x0Du
#define BPB_RESERVED      0x0Eu
#define BPB_NUM_FATS      0x10u
#define BPB_ROOT_ENTRIES  0x11u
#define BPB_FAT16_SIZE    0x16u
#define BPB_HIDDEN        0x1Cu
#define BPB_FAT32_SIZE    0x24u
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

int vbr_signed(const uint8_t *sec)
{
    return sec[VBR_SIG_OFF] == 0x55u && sec[VBR_SIG_OFF + 1u] == 0xAAu;
}

/* The signatures, strongest first. Each is a string the loader's boot code
 * carries in its sector: the file it loads (8.3 names padded as in a
 * directory entry, since that is how FAT boot code holds them) or the
 * message it prints when that file is missing. BOOTMGR before NTLDR and
 * WINBOOT before IO.SYS because the later one is a subset of what the
 * earlier one's sector can say: a Windows 9x boot sector names IO.SYS too,
 * as MS-DOS's does. */
typedef struct {
    const char *text;
    vbr_loader  loader;
} vbr_sig;

static const vbr_sig k_sigs[] = {
    { "BOOTMGR",     VBR_LOADER_BOOTMGR },
    { "NTLDR",       VBR_LOADER_NTLDR },
    { "WINBOOT SYS", VBR_LOADER_WIN9X },
    { "KERNEL  SYS", VBR_LOADER_FREEDOS },
    { "IO      SYS", VBR_LOADER_MSDOS },
    { "IBMBIO  COM", VBR_LOADER_MSDOS },
    { "GRUB ",       VBR_LOADER_GRUB },
    { "SYSLINUX",    VBR_LOADER_SYSLINUX },
    { "EXTLINUX",    VBR_LOADER_SYSLINUX },
};

const uint8_t vbr_fat32_code_sectors[] = { 2u, 12u, 0u };

static int contains(const uint8_t *sec, const char *text)
{
    size_t n = 0;
    size_t i;

    while (text[n] != '\0')
        n++;

    for (i = 0; i + n <= VBR_SIG_OFF; i++) {
        if (memcmp(sec + i, text, n) == 0)
            return 1;
    }
    return 0;
}

vbr_loader vbr_detect(const uint8_t *sec)
{
    size_t i;

    if (!vbr_signed(sec))
        return VBR_LOADER_NONE;
    for (i = 0; i < sizeof k_sigs / sizeof k_sigs[0]; i++) {
        if (contains(sec, k_sigs[i].text))
            return k_sigs[i].loader;
    }
    return VBR_LOADER_NONE;
}

const char *vbr_loader_name(vbr_loader l)
{
    switch (l) {
    case VBR_LOADER_BOOTMGR:  return "Windows (BOOTMGR)";
    case VBR_LOADER_NTLDR:    return "Windows (NTLDR)";
    case VBR_LOADER_WIN9X:    return "Windows 9x";
    case VBR_LOADER_FREEDOS:  return "FreeDOS";
    case VBR_LOADER_MSDOS:    return "MS-DOS";
    case VBR_LOADER_GRUB:     return "GRUB";
    case VBR_LOADER_SYSLINUX: return "SYSLINUX";
    case VBR_LOADER_NONE:
    default:                  return (const char *)0;
    }
}

static int pow2_byte(uint8_t v)
{
    return v != 0 && (v & (v - 1u)) == 0;
}

vbr_bpb vbr_bpb_kind(const uint8_t *sec)
{
    uint16_t bps = get_le16(sec + BPB_BYTES_PER_SEC);

    if (bps != 512u && bps != 1024u && bps != 2048u && bps != 4096u)
        return VBR_BPB_NONE;
    if (memcmp(sec + 3, "NTFS    ", 8) == 0)
        return VBR_BPB_NTFS;
    if (!pow2_byte(sec[BPB_SEC_PER_CLUS]) || get_le16(sec + BPB_RESERVED) == 0 ||
        sec[BPB_NUM_FATS] == 0 || sec[BPB_NUM_FATS] > 2u)
        return VBR_BPB_NONE;
    if (get_le16(sec + BPB_FAT16_SIZE) == 0 && get_le16(sec + BPB_ROOT_ENTRIES) == 0 &&
        get_le32(sec + BPB_FAT32_SIZE) != 0)
        return VBR_BPB_FAT32;
    if (get_le16(sec + BPB_FAT16_SIZE) != 0)
        return VBR_BPB_FAT;
    return VBR_BPB_NONE;
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

int vbr_mbr_bootable(const uint8_t *mbr)
{
    int code = 0;
    unsigned i;

    if (!vbr_signed(mbr))
        return 0;
    for (i = 0; i < 440u; i++) {
        if (mbr[i] != 0) {
            code = 1;
            break;
        }
    }
    if (!code)
        return 0;
    for (i = 0; i < 4u; i++) {
        const uint8_t *e = mbr + VBR_TABLE_OFF + VBR_ENTRY_LEN * i;

        if (e[ENTRY_STATUS] == 0x80u && e[ENTRY_TYPE] != 0)
            return 1;
    }
    return 0;
}
