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
 * It runs as stage 3, read in above a megabyte by stage 2 and reaching the
 * BIOS back down through the gate; nothing here is size-constrained.
 *
 * The ISO scan and the boot paths come next and are mostly already
 * written: `medium.c` assembles a medium's volumes over the same callback,
 * `bootcfg.c` reads an ISO's own boot configuration, and falcon-hook ports
 * untouched because it runs in the initramfs and has no opinion about
 * firmware.
 */

#include <bios.h>

#include <scan.h>
#include <pos.h>
#include <e820.h>

#include <part/part.h>
#include <fs/fs.h>
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

static part_src g_p2_src;
static fs_vol   g_p2;
static fs_dir   g_p2_dir;
static int      g_p2_mounted;

/* p2's own GPT entry and the block size it was read with: between them they
 * are what names the partition to falcon-hook, which has to find the same
 * one from Linux (DESIGN Section 6.4). */
static const gpt_part *g_p2_part;
static uint32_t        g_p2_block;

/* p3 and the disk it is on, for the personal-OS row (DESIGN Section 5). */
static const gpt_part *g_p3_part;
static bios_disk      *g_p3_disk;

/* What the menu draws. The names are kept rather than re-read, because
 * re-walking the directory to redraw a row would mean a thunk call per
 * sector per keystroke. */
#define MENU_MAX 32u

/* The dirent is kept whole rather than just the name, because opening the
 * file wants it back and re-walking the directory to find it again would be
 * a thunk call per sector per selection. */
typedef struct {
    uint16_t  name[64];
    uint64_t  size;
    fs_dirent ent;
} menu_row;

static menu_row g_rows[MENU_MAX];
static unsigned g_row_count;

/* Whether the menu has a personal-OS row, which it does when p3 opened as a
 * LUKS2 volume. One row, always last, as under UEFI. */
static int g_pos_row;

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
        bios_putc(bios_ascii(name[i], '?'));
}

/* The same, to the screen and serial together. */
static void screen_name(const uint16_t *name)
{
    char buf[2];
    size_t i;

    buf[1] = '\0';
    for (i = 0; name[i] != 0; i++) {
        buf[0] = bios_ascii(name[i], '?');
        bios_screen_puts(buf);
    }
}

static void screen_dec(uint64_t v)
{
    char buf[21];
    char one[2];
    size_t n = 0;

    one[1] = '\0';
    if (v == 0) {
        bios_screen_puts("0");
        return;
    }
    while (v > 0 && n < sizeof buf) {
        buf[n++] = (char)('0' + (unsigned)(v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        one[0] = buf[--n];
        bios_screen_puts(one);
    }
}

/* The menu of DESIGN Section 6 stage 1, as much of it as a BIOS build has
 * so far: the images found on p2, one per row, and a selection that moves.
 *
 * The model belongs in falcon-core eventually, shared with the EFI menu
 * (bios-route.md open question 2). It is not moved yet, because moving
 * working code that real hardware has booted, for the benefit of a platform
 * that cannot boot anything yet, is the wrong order. When this build boots a
 * payload, that is the moment. */
static void menu_draw(unsigned selected)
{
    unsigned i;

    bios_screen_clear();
    bios_screen_puts("falcon-boot (BIOS)\n\n");

    if (g_row_count == 0 && !g_pos_row)
        bios_screen_puts("  no bootable images on p2\n");
    for (i = 0; i < g_row_count; i++) {
        bios_screen_puts(i == selected ? "  > " : "    ");
        screen_name(g_rows[i].name);
        bios_screen_puts("   ");
        screen_dec(g_rows[i].size / 1048576u);
        bios_screen_puts(" MiB\n");
    }
    if (g_pos_row) {
        bios_screen_puts(selected == g_row_count ? "  > " : "    ");
        bios_screen_puts("Personal OS (p3)\n");
    }
    bios_screen_puts("\n  up and down to move, enter to boot,"
                     " c to chainload\n");
}

static void menu_run(void)
{
    unsigned selected = 0;
    unsigned rows = g_row_count + (g_pos_row ? 1u : 0u);

    for (;;) {
        int key;

        menu_draw(selected);
        key = bios_getkey();
        if (key == BIOS_KEY_UP && selected > 0)
            selected--;
        else if (key == BIOS_KEY_DOWN && selected + 1u < rows)
            selected++;
        else if (key == BIOS_KEY_ENTER && g_pos_row &&
                 selected == g_row_count) {
            bios_screen_puts("\n  Personal OS\n");
            (void)pos_run(g_p3_disk, g_p3_part);
        }
        else if (key == 'c' && g_row_count > 0 && selected < g_row_count) {
            /* The medium's own bootloader, through the resident handler.
             * Same key as the UEFI menu's, for the same job. */
            bios_screen_puts("\n  ");
            screen_name(g_rows[selected].name);
            bios_screen_puts("\n");
            if (!g_p2_mounted) {
                bios_screen_puts("  p2 is not mounted\n");
                return;
            }
            (void)scan_chain(g_p2_src.disk, &g_p2, g_p2_part,
                             &g_rows[selected].ent);
            bios_screen_puts("\n  press a key\n");
            (void)bios_getkey();
        }
        else if (key == BIOS_KEY_ENTER && g_row_count > 0) {
            bios_screen_puts("\n  ");
            screen_name(g_rows[selected].name);
            bios_screen_puts("\n");
            if (!g_p2_mounted) {
                bios_screen_puts("  p2 is not mounted\n");
                return;
            }
            (void)scan_iso(&g_p2, g_p2_part, &g_rows[selected].ent,
                           g_p2_block);
            bios_screen_puts("\n  loading a kernel is the next stage of the"
                             " route; press a key\n");
            (void)bios_getkey();
        }
    }
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

    /* The one the menu is built from, and the one the personal-OS row needs.
     * Both are identified by DESIGN Section 3's rules, asked of falcon-core's
     * own matcher. */
    for (i = 0; i < t.count; i++) {
        if (gpt_part_is_role(&g_parts[i], PART_ROLE_SHELF)) {
            g_p3_part = &g_parts[i];
            g_p3_disk = d;
            break;
        }
    }
    for (i = 0; i < t.count; i++) {
        if (gpt_part_is_role(&g_parts[i], PART_ROLE_ISO)) {
            scan_iso_partition(d, &g_parts[i]);
            break;
        }
    }
}

/* Mount the falcon ISO partition and list what the menu would show.
 *
 * Through the registry, which identifies the filesystem rather than being
 * told it: p2 is exFAT today by DESIGN Section 3, and nothing here says so.
 * That was not possible in stage 2, where linking fs.c meant linking every
 * driver it knows and there were 33 KB for all of it. */
static void scan_iso_partition(bios_disk *d, const gpt_part *p)
{
    const fs_ops *ops;
    fs_dirent e;
    unsigned shown = 0;
    int rc;

    g_p2_src.disk = d;
    g_p2_src.base = p->start_lba * (uint64_t)d->sector_size;
    g_p2_part = p;
    g_p2_block = d->sector_size;
    scan_disk(d);

    ops = fs_identify(part_read, &g_p2_src, FS_HINT_NONE);
    if (ops == (const fs_ops *)0) {
        bios_puts("falcon-bios:   no driver claimed p2\n");
        return;
    }
    if (fs_mount(&g_p2, ops, part_read, &g_p2_src) != FS_OK) {
        bios_puts("falcon-bios:   p2 would not mount\n");
        return;
    }
    g_p2_mounted = 1;
    bios_puts("falcon-bios:   p2 mounted as ");
    bios_puts(fs_vol_name(&g_p2));
    bios_puts(", label ");
    bios_puts(fs_vol_label(&g_p2));
    bios_puts("\n");

    if (fs_dir_open_root(&g_p2, &g_p2_dir) != FS_OK) {
        bios_puts("falcon-bios:   p2 root unreadable\n");
        return;
    }
    while ((rc = fs_dir_next(&g_p2_dir, &e)) == 1) {
        if (e.is_dir || !is_image(e.name))
            continue;
        bios_puts("falcon-bios:     ");
        put_name(e.name);
        bios_puts("  ");
        bios_put_dec(e.size);
        bios_puts(" bytes\n");
        if (g_row_count < MENU_MAX) {
            size_t k;

            for (k = 0; k + 1u < sizeof g_rows[0].name / sizeof(uint16_t) &&
                        e.name[k] != 0; k++)
                g_rows[g_row_count].name[k] = e.name[k];
            g_rows[g_row_count].name[k] = 0;
            g_rows[g_row_count].size = e.size;
            g_rows[g_row_count].ent = e;
            g_row_count++;
        }
        shown++;
    }
    if (rc < 0)
        bios_puts("falcon-bios:   the root directory ended badly\n");
    bios_puts("falcon-bios:   ");
    bios_put_dec(shown);
    bios_puts(" bootable image(s) on p2\n");
}

/* The memory map, which the loader needs to place an initrd and the kernel
 * needs because by the time it runs the call that answers is gone. */
static void show_memory(void)
{
    unsigned n = e820_read();
    unsigned i;
    uint64_t usable = 0;

    if (n == 0) {
        bios_puts("falcon-bios: no E820 memory map\n");
        return;
    }
    for (i = 0; i < n; i++) {
        const e820_entry *e = e820_at(i);

        if (e != (const e820_entry *)0 && e->type == E820_USABLE)
            usable += e->len;
    }
    bios_puts("falcon-bios: ");
    bios_put_dec(n);
    bios_puts(" E820 entries, ");
    bios_put_dec(usable / 1048576u);
    bios_puts(" MiB usable\n");
}

void bios_main(uint8_t drive)
{
    bios_puts("falcon-bios: C is running in long mode\n");
    show_memory();

    if (!bios_disk_probe(&g_boot, drive))
        bios_puts("falcon-bios: the boot drive did not answer AH=48h\n");
    show_first_sector(&g_boot, "boot");
    show_partitions(&g_boot);

    if (bios_disk_probe(&g_second, DRIVE_SECOND)) {
        show_first_sector(&g_second, "second");
        show_partitions(&g_second);
    }

    /* The personal-OS row, if p3 is there and is LUKS2. Asked once, before
     * the menu draws, because the answer does not change. */
    if (g_p3_part != (const gpt_part *)0)
        g_pos_row = pos_probe(g_p3_disk, g_p3_part);

    bios_puts("falcon-bios: done\n");
    menu_run();
    for (;;)
        __asm__ __volatile__("hlt");
}
