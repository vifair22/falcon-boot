/* falcon-bios: the station's chains (docs/DESIGN.md Section 12.4, routes
 * VBR and MBR).
 *
 * THE PARTITION CHAIN is the Windows MBR's job done by falcon. What a DOS
 * or Windows boot sector finds when it starts, and what this hands it:
 *
 *   0000:0600  the disk's LBA 0, with the chosen primary's entry marked
 *              active (0x80) and every other entry inactive, in this copy
 *              only: the disk's own table is never written
 *   DS:SI      0000:07BE + 16 * slot, that entry in the copy
 *   0000:7C00  the partition's first sector, ending 0x55AA, its BPB's
 *              hidden-sectors field the entry's start and its drive-number
 *              field DL, patched in this copy when the disk's differ
 *   DL         the drive: 0x80 behind the swap, else the disk's own number
 *   CS:IP      0000:7C00, from boot16: real mode, DS = ES = SS = 0,
 *              SP 0x7000, interrupts on
 *
 * NT's and Windows 9x's boot sectors find the rest of themselves through
 * the hidden-sectors field; Windows 98's FAT32 one also reads the disk's LBA
 * 0 for the entry that starts there and takes its type byte (0x0C: read by
 * LBA, 0x0B: by CHS), which is why that field has to match the table and
 * why the entry itself is handed over unchanged but for its status byte.
 *
 * THE DISK CHAIN is mbr.c's, with the swap: the disk's LBA 0 at 0x7C00, DL
 * as above, SI 0.
 *
 * THE SWAP (swapres.S) goes in last, after every read: from the moment its
 * vector is in the table every INT 13h call for these two drives is
 * redirected, stage 3's own included, and the thunk reads nothing more.
 * Nothing between the copy to 0x7C00 and the jump can fail, so a refusal
 * never leaves conventional memory taken or a vector hooked.
 *
 * 0x0600-0x07FF and 0x7C00-0x7DFF are free when this runs: nothing of the
 * loader lives there (Section 11.3.4), and nothing calls the BIOS between
 * the copies and the jump, so the bounce buffer cannot overwrite them.
 */

#include <mbr/chain.h>

#include <mem/lowmem.h>
#include <lib/string.h>

extern const unsigned char swap_blob_start[];
extern const unsigned char swap_blob_end[];

#define IVT_INT13 (0x13u * 4u)

void (*chain_before_handover)(uint8_t drive);

static uint8_t g_mbr[VBR_SECTOR];
static uint8_t g_vbr[VBR_SECTOR];

static int read512(bios_disk *d, uint64_t lba, uint8_t *buf)
{
    if (d->sector_size != VBR_SECTOR || lba > ~(uint64_t)0 / VBR_SECTOR)
        return -1;
    return bios_disk_read(d, lba * VBR_SECTOR, VBR_SECTOR, buf);
}

/* What stops a partition being chained before anything is read: its
 * advice, or NULL when it is a primary on an MBR disk of 512-byte
 * sectors. */
static const char *part_refusal(const bios_disk *d, const part_entry *p)
{
    if (d->sector_size != VBR_SECTOR)
        return "The disk's sectors are not 512 bytes, and a BIOS starts "
               "boot code\nonly from a disk whose sectors are.";
    if (p->mbr_type == 0)
        return "A partition on a GPT disk has no boot sector a BIOS starts.";
    if (!p->primary || p->number < 1u || p->number > 4u)
        return "It is a logical partition; DOS, Windows 9x and NT start "
               "only\nfrom a primary one.";
    if (part_mbr_is_extended(p->mbr_type))
        return "It is an extended partition, which holds other partitions "
               "and\nhas no boot sector.";
    return (const char *)0;
}

typedef struct {
    bios_disk *d;
    uint64_t   start;
} part_reader;

static int read_part(void *ctx, uint32_t sector, uint8_t *buf)
{
    part_reader *pr = (part_reader *)ctx;

    return read512(pr->d, pr->start + sector, buf);
}

vbr_loader chain_part_loader(bios_disk *d, const part_entry *p)
{
    part_reader pr;

    if (part_refusal(d, p) != (const char *)0 ||
        read512(d, p->start_lba, g_vbr) != 0)
        return VBR_LOADER_NONE;
    pr.d = d;
    pr.start = p->start_lba;
    return vbr_identify(g_vbr, read_part, &pr, g_mbr);
}

int chain_disk_bootable(bios_disk *d)
{
    return read512(d, 0, g_mbr) == 0 && vbr_mbr_bootable(g_mbr);
}

uint8_t chain_dl(const bios_disk *d, int swap)
{
    return swap ? 0x80u : d->drive;
}

/* The swap's segment, asked for before anything is copied; 0 for no swap
 * wanted or needed, 1 (never a segment it hands out) for no room. */
static uint16_t swap_reserve(const bios_disk *d, int swap)
{
    uint16_t seg;

    if (!swap || d->drive == 0x80u)
        return 0;
    seg = lowmem_segment(SWAP_BYTES);
    return seg == 0 ? 1u : seg;
}

static void swap_install(uint16_t seg, uint8_t drive)
{
    uint32_t at = (uint32_t)seg * 16u;
    size_t len = (size_t)((uintptr_t)swap_blob_end -
                          (uintptr_t)swap_blob_start);

    memcpy((void *)(uintptr_t)at, swap_blob_start, len);
    poke32(at + SWAP_OFF_OLDINT13, peek32(IVT_INT13));
    poke8(at + SWAP_OFF_DRIVE, drive);
    lowmem_take(seg);
    poke32(IVT_INT13, ((uint32_t)seg << 16) | (uint32_t)SWAP_OFF_CODE);
    bios_puts("falcon-bios: drive ");
    bios_put_hex64(drive);
    bios_puts(" swapped with 0x80, INT 13h resident at segment ");
    bios_put_hex64(seg);
    bios_puts(" (");
    bios_put_dec(len);
    bios_puts(" bytes, 1 KB taken)\n");
}

static void say_fix(const vbr_fix *f, uint64_t start, uint8_t dl)
{
    if (f->hidden_patched) {
        bios_puts("falcon-bios: the boot sector's hidden sectors were ");
        bios_put_dec(f->hidden_was);
        bios_puts(", the partition starts at ");
        bios_put_dec(start);
        bios_puts("; patched in memory\n");
    }
    if (f->hidden_too_far)
        bios_puts("falcon-bios: the partition starts past 32 bits of "
                  "hidden sectors; the field is left as it is\n");
    if (f->drive_patched) {
        bios_puts("falcon-bios: the boot sector's drive number was ");
        bios_put_hex64(f->drive_was);
        bios_puts(", DL is ");
        bios_put_hex64(dl);
        bios_puts("; patched in memory\n");
    }
}

/* The jump, through boot16. Nothing may fail from here. */
static void handover(uint8_t dl, uint16_t si, uint16_t swap_seg,
                     uint8_t drive)
{
    if (chain_before_handover != 0)
        chain_before_handover(drive);
    if (swap_seg > 1u)
        swap_install(swap_seg, drive);
    bios_puts("falcon-bios: handing over\n");
    bios_boot_drive = dl;
    poke16(GATE_SI, si);
    poke16(GATE_TARGET, (uint16_t)STAGE1_LOAD);
    poke16(GATE_TARGET + 2u, 0);
    tui_handoff();
    ((void (*)(void))(uintptr_t)GATE_BOOT16)();
}

int chain_part(bios_disk *d, const part_entry *p, int swap, boot_result *r)
{
    const char *why = part_refusal(d, p);
    unsigned slot;
    uint16_t seg;
    uint8_t dl = chain_dl(d, swap);
    vbr_fix fix;

    boot_stage(r, "checking the partition");
    if (why != (const char *)0) {
        r->advice = why;
        return -1;
    }
    slot = p->number - 1u;

    boot_stage(r, "reading the disk's partition table");
    if (read512(d, 0, g_mbr) != 0) {
        r->advice = "The disk did not answer a read.";
        return -1;
    }
    if (vbr_mark_active(g_mbr, slot, p->start_lba) != 0) {
        r->advice = "The disk's partition table is not the one the menu was "
                    "built from.";
        return -1;
    }

    boot_stage(r, "reading the partition's boot sector");
    if (read512(d, p->start_lba, g_vbr) != 0) {
        r->advice = "The disk did not answer a read.";
        return -1;
    }
    if (!vbr_signed(g_vbr)) {
        r->advice = "The partition's first sector carries no boot code.";
        return -1;
    }

    boot_stage(r, "installing the drive swap");
    seg = swap_reserve(d, swap);
    if (seg == 1u) {
        r->advice = "There is no conventional memory left for it;\nswap = no "
                    "in STATION.INI starts this entry without it.";
        return -1;
    }

    vbr_fix_bpb(g_vbr, p->start_lba, dl, &fix);
    say_fix(&fix, p->start_lba, dl);
    bios_puts("falcon-bios: partition ");
    bios_put_dec(p->number);
    bios_puts(" of drive ");
    bios_put_hex64(d->drive);
    bios_puts(" (LBA ");
    bios_put_dec(p->start_lba);
    bios_puts(", type ");
    bios_put_hex64(p->mbr_type);
    bios_puts(") to 0x7c00, its table to 0x600, DS 0, SI ");
    bios_put_hex64(VBR_ENTRY_SI(slot));
    bios_puts(", DL ");
    bios_put_hex64(dl);
    bios_puts("\n");

    memcpy((void *)(uintptr_t)VBR_MBR_COPY, g_mbr, VBR_SECTOR);
    memcpy((void *)(uintptr_t)STAGE1_LOAD, g_vbr, VBR_SECTOR);
    handover(dl, (uint16_t)VBR_ENTRY_SI(slot), seg, d->drive);
    boot_stage(r, "starting its boot code");
    return -1;
}

int chain_disk(bios_disk *d, int swap, boot_result *r)
{
    uint16_t seg;
    uint8_t dl = chain_dl(d, swap);

    boot_stage(r, "reading the disk's first sector");
    if (d->sector_size != VBR_SECTOR) {
        r->advice = "The disk's sectors are not 512 bytes, and a BIOS starts "
                    "boot code\nonly from a disk whose sectors are.";
        return -1;
    }
    if (read512(d, 0, g_mbr) != 0) {
        r->advice = "The disk did not answer a read.";
        return -1;
    }
    boot_stage(r, "checking the boot signature");
    if (!vbr_signed(g_mbr)) {
        r->advice = "Its first sector carries no boot code.";
        return -1;
    }

    boot_stage(r, "installing the drive swap");
    seg = swap_reserve(d, swap);
    if (seg == 1u) {
        r->advice = "There is no conventional memory left for it;\nswap = no "
                    "in STATION.INI starts this entry without it.";
        return -1;
    }

    bios_puts("falcon-bios: MBR of drive ");
    bios_put_hex64(d->drive);
    bios_puts(" to 0x7c00, DL ");
    bios_put_hex64(dl);
    bios_puts("\n");
    memcpy((void *)(uintptr_t)STAGE1_LOAD, g_mbr, VBR_SECTOR);
    handover(dl, 0, seg, d->drive);
    boot_stage(r, "starting its boot code");
    return -1;
}
