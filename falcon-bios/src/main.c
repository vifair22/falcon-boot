/* falcon-bios: the C half, entered once long mode is up
 * (docs/notes/bios-route.md Sections 1 and 5.3).
 *
 * What this file is for, at this stage of the route, is to be the evidence
 * for Section 1's claim rather than a loader: falcon-core is EFI-free by
 * construction, the per-platform boundary is a read callback, and a BIOS
 * build is its fourth consumer after the host tests, the CLI tools and the
 * signing appliance. So it asks falcon-core to parse a real GPT over a
 * callback whose other end is INT 13h, with not one line of falcon-core
 * changed to allow it.
 *
 * The menu, the ISO scan and the boot paths come next and are mostly already
 * written: `medium.c` assembles a medium's volumes over the same callback,
 * `bootcfg.c` reads an ISO's own boot configuration, and falcon-hook ports
 * untouched because it runs in the initramfs and has no opinion about
 * firmware.
 */

#include <bios.h>

#include <part/part.h>
#include <lib/string.h>

/* The drive the BIOS booted from, and the second one the rig attaches. Both
 * are asked about, because "did the callback work" and "does falcon-core
 * read a real partition table through it" are different questions and a
 * single disk cannot answer both: the boot disk here is a raw image with no
 * partition table at all. */
#define DRIVE_SECOND 0x81u

/* Caller-allocated, as everywhere in falcon-core, and static rather than on
 * the stack because the stack in this environment is the 30 KB below the
 * boot sector. */
static gpt_part g_parts[16];
static bios_disk g_boot;
static bios_disk g_second;
static uint8_t g_sector[512];

static void show_first_sector(bios_disk *d, const char *what)
{
    bios_puts("falcon-bios: ");
    bios_puts(what);
    bios_puts(" drive ");
    bios_put_hex64(d->drive);
    bios_puts(", sector size ");
    bios_put_dec(d->sector_size);
    bios_puts("\n");

    if (bios_disk_read(d, 0, d->sector_size < sizeof g_sector
                              ? d->sector_size : sizeof g_sector,
                       g_sector) != 0) {
        bios_puts("falcon-bios:   LBA 0 unreadable\n");
        return;
    }
    bios_puts("falcon-bios:   LBA 0 ends ");
    bios_put_hex64((uint64_t)g_sector[510] | ((uint64_t)g_sector[511] << 8));
    bios_puts("\n");
}

static const char *gpt_error(int rc)
{
    switch (rc) {
    case GPT_ERR_IO:          return "the read callback failed";
    case GPT_ERR_NOT_GPT:     return "no GPT signature at LBA 1";
    case GPT_ERR_CRC:         return "a CRC did not check out";
    case GPT_ERR_BAD_ARG:     return "bad argument";
    case GPT_ERR_UNSUPPORTED: return "geometry this parser declines";
    default:                  return "unknown";
    }
}

static void show_partitions(bios_disk *d)
{
    gpt_table t;
    uint32_t i;
    int rc;

    t.parts = g_parts;
    t.cap = (uint32_t)(sizeof g_parts / sizeof g_parts[0]);
    t.count = 0;
    t.truncated = 0;

    rc = gpt_parse(&t, bios_disk_read, d, d->sector_size);
    if (rc != GPT_OK) {
        bios_puts("falcon-bios:   no partition table: ");
        bios_puts(gpt_error(rc));
        bios_puts("\n");
        return;
    }

    bios_puts("falcon-bios:   GPT with ");
    bios_put_dec(t.count);
    bios_puts(" partition(s)");
    if (t.truncated)
        bios_puts(" (more than fitted)");
    bios_puts("\n");

    for (i = 0; i < t.count; i++) {
        bios_puts("falcon-bios:     p");
        bios_put_dec(g_parts[i].number);
        bios_puts("  LBA ");
        bios_put_dec(g_parts[i].start_lba);
        bios_puts("..");
        bios_put_dec(g_parts[i].end_lba);
        /* The roles DESIGN Section 3 identifies, asked of falcon-core's own
         * matcher rather than of a GUID comparison written again here. */
        if (gpt_part_is_role(&g_parts[i], PART_ROLE_ISO))
            bios_puts("  [falcon p2]");
        else if (gpt_part_is_role(&g_parts[i], PART_ROLE_SHELF))
            bios_puts("  [falcon p3]");
        bios_puts("\n");
    }
}

void bios_main(uint8_t drive)
{
    bios_puts("falcon-bios: C is running in long mode\n");

    if (!bios_disk_probe(&g_boot, drive))
        bios_puts("falcon-bios: the boot drive did not answer AH=48h\n");
    show_first_sector(&g_boot, "boot");
    show_partitions(&g_boot);

    if (bios_disk_probe(&g_second, DRIVE_SECOND)) {
        show_first_sector(&g_second, "second");
        show_partitions(&g_second);
    }

    bios_puts("falcon-bios: done\n");
    for (;;)
        __asm__ __volatile__("hlt");
}
