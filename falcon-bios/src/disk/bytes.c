/* falcon-bios: the read callback the drivers are handed (DESIGN Section
 * 3.4).
 *
 * bios_disk_read moves whole sectors, which is what INT 13h moves and what
 * stage 2, the partition table and the marker ask for. The drivers ask for
 * what their formats lay out (512 bytes of a LUKS2 keyslot area, the
 * 1024-byte ext4 superblock, a boot sector's first 512 bytes), which on a
 * 4096-byte-sector disk is part of a sector. This is falcon-core's
 * re-blocking layer over bios_disk_read, and the one place falcon-bios uses
 * it: every route that hands a partition to a driver reads through
 * bios_disk_bytes (apps/stick part_read, pos/pos.c part_read).
 *
 * Its own file because disk.c is also stage 2's, and stage 2 neither needs
 * this nor has room for it, nor a .bss for the bounce buffer below.
 */

#include <start/bios.h>

#include <blk/rebl.h>

/* The ragged head or tail of one request: a sector of the largest size
 * bios_disk_probe accepts. In stage 3's .bss; nothing is kept in it between
 * calls. */
static uint8_t g_bounce[4096];

int bios_disk_bytes(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const bios_disk *d = ctx;
    blk_rebl r;

    if (d == (const bios_disk *)0)
        return -1;
    r.read = bios_disk_read;
    r.ctx = ctx;
    r.sector_size = d->sector_size;
    r.bounce = g_bounce;
    r.bounce_len = (uint32_t)sizeof g_bounce;
    return blk_rebl_read(&r, off, len, buf);
}
