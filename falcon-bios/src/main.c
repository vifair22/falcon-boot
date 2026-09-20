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
#include <fs/exfat.h>
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

/* The ISO partition, once found, and the offset shim that turns a
 * disk-relative callback into a partition-relative one. Same shape as
 * medium.c's, and for the same reason: everything above wants to think it is
 * reading a volume from zero. */
typedef struct {
    bios_disk *disk;
    uint64_t   base;
} part_src;

static part_src  g_p2_src;
static exfat_vol g_p2;
static exfat_dir g_p2_dir;

static int part_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const part_src *p = ctx;

    if (off > ~(uint64_t)0 - p->base)
        return -1;
    return bios_disk_read(p->disk, p->base + off, len, buf);
}

/* Is this name one the menu would show? The rule is DESIGN Section 6's and
 * is asked here only of the extension, because the depth cap and the dotfile
 * filter belong with the scan rather than with this demonstration. */
static int is_image(const uint16_t *name)
{
    size_t n = 0;
    uint16_t c3, c2, c1;

    while (name[n] != 0)
        n++;
    if (n < 5u)
        return 0;
    if (name[n - 4u] != (uint16_t)'.')
        return 0;
    c3 = name[n - 3u] | 0x20u;
    c2 = name[n - 2u] | 0x20u;
    c1 = name[n - 1u] | 0x20u;
    if (c3 == 'i' && c2 == 's' && c1 == 'o')
        return 1;
    return c3 == 'i' && c2 == 'm' && c1 == 'g';
}

static void put_name(const uint16_t *name)
{
    size_t i;

    for (i = 0; name[i] != 0; i++)
        bios_putc(name[i] < 0x80u ? (char)name[i] : '?');
}
static bios_disk g_boot;
static bios_disk g_second;
static uint8_t g_sector[512];

/* Defined below, beside the exFAT state it uses. */
static void scan_iso_partition(bios_disk *d, const gpt_part *p);

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

    /* The one the menu is built from. */
    for (i = 0; i < t.count; i++) {
        if (gpt_part_is_role(&g_parts[i], PART_ROLE_ISO)) {
            scan_iso_partition(d, &g_parts[i]);
            break;
        }
    }
}

/* Mount the falcon ISO partition and list what the menu would show.
 *
 * exfat.c is called directly rather than through the filesystem registry,
 * because linking fs.c pulls in every driver it knows and stage 2 has 32 KB.
 * A real BIOS loader loads a third stage above a megabyte and uses the
 * registry like everything else; this is the same driver either way. */
static void scan_iso_partition(bios_disk *d, const gpt_part *p)
{
    exfat_dirent e;
    unsigned shown = 0;
    int rc;

    g_p2_src.disk = d;
    g_p2_src.base = p->start_lba * (uint64_t)d->sector_size;

    if (exfat_mount(&g_p2, part_read, &g_p2_src) != EXFAT_OK) {
        bios_puts("falcon-bios:   p2 did not mount as exFAT\n");
        return;
    }
    bios_puts("falcon-bios:   p2 mounted, ");
    bios_put_dec(g_p2.cluster_count);
    bios_puts(" clusters of ");
    bios_put_dec((uint64_t)1u << (g_p2.bps_shift + g_p2.spc_shift));
    bios_puts(" bytes\n");

    if (exfat_dir_open_root(&g_p2, &g_p2_dir) != EXFAT_OK) {
        bios_puts("falcon-bios:   p2 root unreadable\n");
        return;
    }
    while ((rc = exfat_dir_next(&g_p2_dir, &e)) == 1) {
        if (e.is_dir || !is_image(e.name))
            continue;
        bios_puts("falcon-bios:     ");
        put_name(e.name);
        bios_puts("  ");
        bios_put_dec(e.size);
        bios_puts(" bytes\n");
        shown++;
    }
    if (rc < 0)
        bios_puts("falcon-bios:   the root directory ended badly\n");
    bios_puts("falcon-bios:   ");
    bios_put_dec(shown);
    bios_puts(" bootable image(s) on p2\n");
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
