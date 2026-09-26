/* falcon-bios: another disk's own boot code (docs/DESIGN.md Section 13.4,
 * route MBR).
 *
 * The stationary build's way to the machine's other systems on a BIOS: a
 * Windows install on a second disk is started exactly as the BIOS would
 * start it had that disk been first in its boot order. Its LBA 0 goes to
 * 0x7C00, DL names its drive, and boot16 drops to real mode and jumps.
 *
 * Nothing is resident at the handoff. This build hooks no interrupt and
 * takes no conventional memory, so the boot code finds the IVT, the BDA and
 * the memory map as the BIOS left them, which is the whole difference from
 * the stick's CHN route and why this needs none of vcd.c.
 *
 * 0x7C00 is free by the time this runs: stage 1 was there and is done with,
 * and stage 2 starts at 0x7E00, exactly one sector above. A boot sector is
 * one 512-byte sector, so it lands without the relocation the CD route's
 * larger boot images need. Nothing calls the BIOS between the copy and the
 * jump, so the thunk's bounce buffer cannot overwrite it.
 *
 * The drive number is the one the BIOS gave the disk, typically 0x81 for a
 * second internal disk. Windows since 7 boots from 0x81 as it is; one that
 * insists on 0x80 would want the two drives swapped by a resident INT 13h
 * hook (GRUB's drivemap), which is not built until a machine needs it.
 */

#include <mbr/mbr.h>

#include <lib/string.h>

#define BOOT_SIG_OFF 510u

/* One sector of the largest size a BIOS disk reports. */
static uint8_t g_sector[4096];

static int read_lba0(bios_disk *d)
{
    uint32_t n = d->sector_size;

    if (n < 512u || n > sizeof g_sector)
        return -1;
    return bios_disk_read(d, 0, n, g_sector);
}

int mbr_bootable(bios_disk *d)
{
    if (read_lba0(d) != 0)
        return 0;
    return g_sector[BOOT_SIG_OFF] == 0x55 && g_sector[BOOT_SIG_OFF + 1] == 0xAA;
}

int mbr_boot(bios_disk *d, boot_result *r)
{
    r->stage = "reading the disk's first sector";
    if (read_lba0(d) != 0) {
        r->advice = "The disk did not answer a read.";
        return -1;
    }
    r->stage = "checking the boot signature";
    if (g_sector[BOOT_SIG_OFF] != 0x55 || g_sector[BOOT_SIG_OFF + 1] != 0xAA) {
        r->advice = "Its first sector carries no boot code.";
        return -1;
    }

    bios_puts("falcon-bios: MBR of drive ");
    bios_put_hex64(d->drive);
    bios_puts(" to 0x7c00\nfalcon-bios: handing over\n");

    memcpy((void *)(uintptr_t)STAGE1_LOAD, g_sector, 512u);
    bios_boot_drive = d->drive;
    poke16(GATE_TARGET, (uint16_t)STAGE1_LOAD);
    poke16(GATE_TARGET + 2u, 0);
    ((void (*)(void))(uintptr_t)GATE_BOOT16)();
    r->stage = "starting its boot code";
    return -1;
}
