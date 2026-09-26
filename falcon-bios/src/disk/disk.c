/* falcon-bios: the read callback, over INT 13h (docs/notes/bios-route.md
 * Sections 1 and 4.1).
 *
 * This file is the claim Section 1 makes about the whole route, in code: the
 * per-platform boundary is a read callback, so falcon-core links unchanged
 * and a BIOS build is its fourth consumer after the host tests, the CLI
 * tools and the appliance. Everything above this - GPT, exFAT, ISO9660, UDF,
 * FAT, the El Torito catalog, the medium's volume set - is the same code the
 * UEFI loader runs.
 *
 * THE BOUNCE BUFFER IS NOT OPTIONAL, and it is the only thing here that is
 * not obvious. Real mode addresses twenty bits, so the BIOS cannot write
 * above one megabyte, and a caller in protected mode will happily pass a
 * buffer anywhere in the 4 GiB it can address. Every transfer therefore
 * lands in a fixed low buffer first and is copied up. Syslinux's MEMDISK and
 * every other BIOS loader does the same thing for the same reason.
 */

#include <start/bios.h>

#include <lib/string.h>

/* INT 13h AH=48h, the drive parameters. Same reachability rule.
 *
 * Not volatile. The BIOS writes it, but it does so inside thunk_bios, which
 * is a call the compiler already has to assume clobbers memory. */
typedef struct {
    uint16_t size;
    uint16_t flags;
    uint32_t cylinders;
    uint32_t heads;
    uint32_t sectors_per_track;
    uint64_t total_sectors;
    uint16_t bytes_per_sector;
} bios_driveparm;

#define g_params (*(bios_driveparm *)(uintptr_t)DRIVEPARM_ADDR)

static int read_sectors(const bios_disk *d, uint64_t lba, uint16_t count)
{
    bios_dap_at.size = 16;
    bios_dap_at.reserved = 0;
    bios_dap_at.sectors = count;
    bios_dap_at.offset = (uint16_t)(BOUNCE_ADDR & 0x0Fu);
    bios_dap_at.segment = (uint16_t)(BOUNCE_ADDR >> 4);
    bios_dap_at.lba = lba;

    thunk_regs.eax = 0x4200u;              /* AH=42h, extended read */
    thunk_regs.edx = d->drive;
    thunk_regs.esi = (uint32_t)DAP_ADDR;
    thunk_regs.es = 0;
    thunk_vector = 0x13;
    thunk_bios();

    return (thunk_regs.flags & BIOS_FLAG_CARRY) == 0;
}

int bios_disk_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const bios_disk *d = ctx;
    uint8_t *out = buf;
    uint32_t ss;

    if (d == (const bios_disk *)0 || buf == (void *)0)
        return -1;
    ss = d->sector_size;
    if (ss == 0)
        return -1;
    /* Aligned reads only, which is the same contract the loader's own
     * callback has (falcon-efi/src/discover/discover.c refuses anything
     * else, because Block I/O does). A driver that wants part of a sector
     * reads the sector. */
    if (off % ss != 0 || len % ss != 0)
        return -1;

    while (len > 0) {
        uint64_t chunk = len;
        uint16_t count;

        if (chunk > BOUNCE_BYTES)
            chunk = BOUNCE_BYTES;
        count = (uint16_t)(chunk / ss);
        if (count == 0)
            return -1;

        if (!read_sectors(d, off / ss, count))
            return -1;
        memcpy(out, (const void *)(uintptr_t)BOUNCE_ADDR, (size_t)chunk);

        out += chunk;
        off += chunk;
        len -= chunk;
    }
    return 0;
}

int bios_disk_probe(bios_disk *d, uint8_t drive)
{
    if (d == (bios_disk *)0)
        return 0;
    d->drive = drive;
    /* 512 unless the drive says otherwise. An El Torito boot from a CD
     * arrives with 2048, and reading it in 512-byte units would quietly
     * return the wrong bytes rather than fail. */
    d->sector_size = 512;

    memset(&g_params, 0, sizeof g_params);
    g_params.size = sizeof g_params;

    thunk_regs.eax = 0x4800u;              /* AH=48h, drive parameters */
    thunk_regs.edx = drive;
    thunk_regs.esi = (uint32_t)DRIVEPARM_ADDR;
    thunk_regs.es = 0;
    thunk_vector = 0x13;
    thunk_bios();

    if ((thunk_regs.flags & BIOS_FLAG_CARRY) != 0)
        return 0;
    if (g_params.bytes_per_sector == 512u || g_params.bytes_per_sector == 2048u ||
        g_params.bytes_per_sector == 4096u)
        d->sector_size = g_params.bytes_per_sector;
    return 1;
}
