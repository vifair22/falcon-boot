/* falcon-boot: what a boot sector is. See vbr.h. */

#include <media/vbr.h>

#include <lib/string.h>

#define BPB_BYTES_PER_SEC 0x0Bu
#define BPB_SEC_PER_CLUS  0x0Du
#define BPB_RESERVED      0x0Eu
#define BPB_NUM_FATS      0x10u
#define BPB_ROOT_ENTRIES  0x11u
#define BPB_FAT16_SIZE    0x16u
#define BPB_FAT32_SIZE    0x24u

#define ENTRY_STATUS 0u
#define ENTRY_TYPE   4u

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

/* The first signature the sector's bytes carry, signed or not. */
static vbr_loader find(const uint8_t *sec)
{
    size_t i;

    for (i = 0; i < sizeof k_sigs / sizeof k_sigs[0]; i++) {
        if (contains(sec, k_sigs[i].text))
            return k_sigs[i].loader;
    }
    return VBR_LOADER_NONE;
}

vbr_loader vbr_detect(const uint8_t *sec)
{
    if (!vbr_signed(sec))
        return VBR_LOADER_NONE;
    return find(sec);
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

vbr_loader vbr_identify(const uint8_t *sec0, vbr_read_fn read, void *ctx,
                        uint8_t *scratch)
{
    vbr_loader l;
    size_t i;

    if (!vbr_signed(sec0))
        return VBR_LOADER_NONE;
    l = find(sec0);
    if (l != VBR_LOADER_NONE || vbr_bpb_kind(sec0) != VBR_BPB_FAT32 ||
        read == (vbr_read_fn)0)
        return l;
    /* FAT32 boot code runs on into the reserved sectors after it, and the
     * name it loads can be there rather than in sector 0. */
    for (i = 0; vbr_fat32_code_sectors[i] != 0; i++) {
        if (read(ctx, vbr_fat32_code_sectors[i], scratch) != 0)
            break;
        l = find(scratch);
        if (l != VBR_LOADER_NONE)
            return l;
    }
    return VBR_LOADER_NONE;
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
        const uint8_t *e = mbr + VBR_TABLE_OFF + (size_t)VBR_ENTRY_LEN * i;

        if (e[ENTRY_STATUS] == 0x80u && e[ENTRY_TYPE] != 0)
            return 1;
    }
    return 0;
}
