/* falcon-bios: installing the virtual CD and handing the machine to the
 * medium's own bootloader.
 *
 * The chainload route, which is the one a BIOS build exists for: isolinux,
 * an ISO's own GRUB, FreeBSD's loader and Windows' bootmgr all read their
 * medium through INT 13h and have no other way to. Under UEFI falcon-boot
 * answers that by publishing a Block I/O protocol over the ISO; the
 * equivalent here is a handler resident in conventional memory on a drive
 * number that did not exist.
 *
 * WHERE THE RESIDENT GOES, and why not just anywhere. A chainloaded
 * bootloader believes it owns memory below a megabyte: isolinux relocates
 * itself, GRUB's core image decompresses into it, bootmgr uses most of it.
 * The only region it will leave alone is one the BIOS says is not there. So
 * the word at 0x413 - how many kilobytes of conventional memory the machine
 * has, which every one of them reads - is lowered by sixteen (VCD_BYTES), and
 * the resident lives in the sixteen kilobytes that just stopped existing:
 * its code and tables in the first four, the overlay pool in the rest.
 * MEMDISK has done this since 2001 and it is the only thing that works.
 */

#include <start/bios.h>
#include <iso/vcd.h>
#include <iso/vcdcore.h>
#include <vfd/vfd.h>
#include <mem/e820.h>
#include <mem/lowmem.h>

#include <fs/fs.h>
#include <media/eltorito.h>
#include <lib/string.h>

/* The UEFI loader's words for the same failure (falcon-efi/src/direct). */
#define TOO_MANY_PIECES \
    "the ISO is in too many pieces on the stick. Copy it again, onto a " \
    "stick with more free space."

/* The blob, as vcdres.S lays it out. Copied, never called where it is
 * linked: stage 3 is above a megabyte and real mode cannot reach it.
 *
 * Its length is taken through integers rather than by subtracting the two
 * symbols, because they are two declared objects and the difference of
 * pointers into different objects is undefined however true the arithmetic
 * is. The linker knows they bracket one section; C does not. */
extern const unsigned char vcd_blob_start[];
extern const unsigned char vcd_blob_end[];

static size_t vcd_blob_len(void)
{
    return (size_t)((uintptr_t)vcd_blob_end - (uintptr_t)vcd_blob_start);
}

/* How many kilobytes of conventional memory the BIOS says there are, and
 * the vector this loader takes over. */
#define IVT_INT13   (0x13u * 4u)
#define IVT_INT15   (0x15u * 4u)


/* The memory map the resident answers E820 with. */
static e820_entry g_carved[VCD_E820_MAX];

static vcd_map    g_map;

/* The overlay pool, staged here and copied into conventional memory when the
 * resident is installed: what an override serves has to be somewhere real
 * mode can read, and where that is is not known until the BIOS data area has
 * been asked to give it up. */
static uint8_t  g_pool[(size_t)VCD_OVER_MAX_CD * VCD_CD_BYTES];
static uint32_t g_pool_used;      /* CD sectors */

static struct {
    uint32_t cd_lba;
    uint32_t n_cd;
    uint32_t pool_at;             /* CD sectors into g_pool */
} g_pending[VCD_MAX_OVERRIDES];
static unsigned g_n_pending;

/* The appended region, when there is one: bytes above a megabyte served past
 * the ISO's own end, which the medium has to be grown to hold. */
static uint32_t g_append_phys;
static uint32_t g_append_cd;

/* The redirect, when there is one: a stretch grown past the ISO's end whose
 * sectors are served from where CD sector g_redir_src and the ones after it
 * already sit on the disk. It is a relocated boot.wim's bulk (vcd.h). */
static uint32_t g_redir_src;
static uint32_t g_redir_cd;

static unsigned g_n_carved;
static uint16_t g_e801_cap;
static uint16_t g_e801_low;

/* Memory a caller keeps in use behind the handoff besides the virtual CD's
 * own (falcon jump's reverse thunk and driver region): carved out of the map
 * with it. */
#define VCD_KEEP_MAX 2u
static e820_range g_keep_extra[VCD_KEEP_MAX];
static unsigned   g_n_keep_extra;

static vcd_extent g_ext[VCD_MAX_RUNS + 8u];
static fs_extent  g_fsext[VCD_MAX_RUNS + 8u];
static uint8_t    g_sector[2048];

void vcd_keep_reset(void)
{
    g_n_keep_extra = 0;
}

int vcd_keep(uint64_t base, uint64_t len)
{
    if (g_n_keep_extra >= VCD_KEEP_MAX || len == 0)
        return -1;
    g_keep_extra[g_n_keep_extra].base = base;
    g_keep_extra[g_n_keep_extra].len = len;
    g_n_keep_extra++;
    return 0;
}

void vcd_overlay_reset(void)
{
    g_pool_used = 0;
    g_n_pending = 0;
    g_append_phys = 0;
    g_append_cd = 0;
    g_redir_src = 0;
    g_redir_cd = 0;
}

int vcd_overlay_append(uint32_t phys, uint32_t n_cd)
{
    if (phys == 0 || n_cd == 0 || g_append_cd != 0)
        return -1;
    g_append_phys = phys;
    g_append_cd = n_cd;
    return 0;
}

int vcd_overlay_redirect(uint32_t src_cd_lba, uint32_t n_cd)
{
    if (n_cd == 0 || g_redir_cd != 0)
        return -1;
    g_redir_src = src_cd_lba;
    g_redir_cd = n_cd;
    return 0;
}

int vcd_overlay_at(uint32_t cd_lba, const uint8_t *sector)
{
    uint8_t *at;

    if (sector == (const uint8_t *)0)
        return -1;
    if (g_n_pending >= VCD_MAX_OVERRIDES || g_pool_used >= VCD_OVER_MAX_CD)
        return -1;

    at = g_pool + (size_t)g_pool_used * VCD_CD_BYTES;
    memcpy(at, sector, VCD_CD_BYTES);
    g_pending[g_n_pending].cd_lba = cd_lba;
    g_pending[g_n_pending].n_cd = 1;
    g_pending[g_n_pending].pool_at = g_pool_used;
    g_n_pending++;
    g_pool_used += 1;
    return 0;
}

int vcd_overlay(fs_file *isofile, uint64_t iso_off, const void *bytes,
                uint32_t len)
{
    uint64_t first;
    uint64_t last;
    uint32_t n_cd;
    uint8_t *at;

    if (isofile == (fs_file *)0 || bytes == (const void *)0 || len == 0)
        return -1;

    first = iso_off / VCD_CD_BYTES;
    last = (iso_off + len - 1u) / VCD_CD_BYTES;
    n_cd = (uint32_t)(last - first + 1u);

    if (g_n_pending >= VCD_MAX_OVERRIDES ||
        n_cd > VCD_OVER_MAX_CD - g_pool_used)
        return -1;

    at = g_pool + (size_t)g_pool_used * VCD_CD_BYTES;
    if (fs_file_read(isofile, first * VCD_CD_BYTES,
                     (uint64_t)n_cd * VCD_CD_BYTES, at) != FS_OK)
        return -1;
    memcpy(at + (size_t)(iso_off - first * VCD_CD_BYTES), bytes, len);

    g_pending[g_n_pending].cd_lba = (uint32_t)first;
    g_pending[g_n_pending].n_cd = n_cd;
    g_pending[g_n_pending].pool_at = g_pool_used;
    g_n_pending++;
    g_pool_used += n_cd;
    return 0;
}

/* The medium's BIOS boot image. The rule is core's (media/eltorito.h), so
 * the menu's {EFI ONLY} tag (media/biosboot.c) and this route cannot
 * disagree about which entry a medium boots from. */
const elt_entry *vcd_boot_entry(const elt_catalog *cat)
{
    return elt_bios_entry(cat);
}

/* Writes the El Torito status packet AH=4Bh returns, at the offset vcd.S
 * expects it. It is how a bootloader started from a CD learns which drive it
 * came off and where its own image is; isolinux asks first and gives up when
 * the answer does not come. */
static void write_spec(uint8_t *at, const elt_entry *e, uint16_t load_seg)
{
    memset(at, 0, 0x13u);
    at[0x00] = 0x13;                 /* the packet's own size */
    at[0x01] = ELT_MEDIA_NONE;       /* no emulation */
    at[0x02] = (uint8_t)VCD_DRIVE;
    at[0x03] = 0;                    /* controller index */
    at[0x04] = (uint8_t)(e->lba & 0xFFu);
    at[0x05] = (uint8_t)((e->lba >> 8) & 0xFFu);
    at[0x06] = (uint8_t)((e->lba >> 16) & 0xFFu);
    at[0x07] = (uint8_t)((e->lba >> 24) & 0xFFu);
    at[0x08] = (uint8_t)VCD_DRIVE;   /* device specification */
    at[0x0C] = (uint8_t)(load_seg & 0xFFu);
    at[0x0D] = (uint8_t)(load_seg >> 8);
    at[0x0E] = (uint8_t)(e->sectors & 0xFFu);
    at[0x0F] = (uint8_t)(e->sectors >> 8);
}

/* Everything after the map: the boot image staged, the resident's block
 * found, the map finished and carved, the resident written and the machine
 * handed over. `read` reads the medium by byte offset, in whole CD sectors.
 * Returns only on a refusal. */
static int launch(bios_disk *d, const elt_entry *e, fs_read_fn read,
                  void *ctx, boot_result *r)
{
    uint8_t *at;
    uint16_t seg;
    uint16_t load_seg;
    uint32_t load_bytes;
    uint32_t old_vector;
    size_t i;

    /* The boot image, where the medium says it goes. A load segment of zero
     * means 0x7C0, which is the boot sector address every BIOS has used. */
    load_seg = 0x07C0u;
    load_bytes = (uint32_t)e->sectors * 512u;
    if (load_bytes == 0)
        load_bytes = 2048u;          /* isolinux's four virtual sectors */
    if (load_bytes > 63u * 1024u)
        load_bytes = 63u * 1024u;

    /* Staged, not placed: where it belongs is 0x7C00, and stage 2 starts
     * 512 bytes above that. The launch stub inside the resident moves it,
     * from memory the BIOS no longer admits to having. */
    {
        uint32_t done = 0;
        uint8_t *dst = (uint8_t *)(uintptr_t)VCD_STAGE_ADDR;

        /* A whole CD sector at a time: a drive reads nothing smaller. */
        while (done < load_bytes) {
            uint32_t chunk = load_bytes - done;

            if (chunk > (uint32_t)sizeof g_sector)
                chunk = (uint32_t)sizeof g_sector;
            boot_stage(r, "reading the medium's boot image");
            if (read(ctx, (uint64_t)e->lba * ELT_SECTOR + done,
                     (uint64_t)sizeof g_sector, g_sector) != 0)
                return -1;
            memcpy(dst + done, g_sector, (size_t)chunk);
            done += chunk;
        }
    }

    /* Where the resident will live. Not taken until the end. */
    boot_stage(r, "reserving memory for the virtual CD");
    seg = lowmem_segment(VCD_BYTES);
    if (seg == 0) {
        r->advice = "this machine will not give up the conventional memory "
                    "the virtual CD needs.";
        return -1;
    }

    /* The map finished before any of it is copied: the appended region
     * grows the medium, and the resident has to be handed the size it ends
     * up, not the size it started. The appended region goes first because
     * the medium has to say it is there before anything may be placed in
     * it. */
    boot_stage(r, "placing the medium's edits");
    if (g_redir_cd != 0) {
        /* The redirect grows the medium and maps its new sectors, run for run,
         * to where the original bytes already lie: the disk serves them, so
         * nothing here is carved out of the memory map below. */
        uint32_t dst = g_map.n_real;    /* the redirect starts at the old end */
        uint32_t placed = 0;

        if (vcd_extend(&g_map, g_redir_cd) != VCD_OK)
            return -1;
        while (placed < g_redir_cd) {
            uint64_t disk;
            uint32_t n;

            if (vcd_locate(&g_map, g_redir_src + placed, g_redir_cd - placed,
                           &disk, &n) != VCD_OK ||
                vcd_add_run(&g_map, dst + placed, n, disk) != VCD_OK)
                return -1;
            placed += n;
        }
    }
    if (g_append_cd != 0) {
        if (vcd_extend(&g_map, g_append_cd) != VCD_OK ||
            vcd_override_add_ext(&g_map, g_map.n_real - g_append_cd,
                                 g_append_cd, g_append_phys) != VCD_OK)
            return -1;
    }
    for (i = 0; i < g_n_pending; i++) {
        uint16_t pool_seg = (uint16_t)(seg + (VCD_OVER_OFF / 16u) +
                                       g_pending[i].pool_at *
                                       (VCD_CD_BYTES / 16u));

        if (vcd_override_add(&g_map, g_pending[i].cd_lba,
                             g_pending[i].n_cd, pool_seg) != VCD_OK)
            return -1;
    }

    /* The memory map a chainloaded loader will allocate from, with what
     * stays in use behind the handoff taken out of it: this block, and the
     * appended region the handler serves from extended memory. The BDA
     * hides the block from a caller that asks it; this hides both from one
     * that asks E820 or E801, which is what bootmgr and a kernel ask. */
    {
        e820_range keep[2u + VCD_KEEP_MAX];
        unsigned n_keep = 0;
        const e820_entry *first = e820_at(0);

        keep[n_keep].base = (uint64_t)seg * 16u;
        keep[n_keep].len = VCD_BYTES;
        n_keep++;
        if (g_append_cd != 0) {
            keep[n_keep].base = g_append_phys;
            keep[n_keep].len = (uint64_t)g_append_cd * VCD_CD_BYTES;
            n_keep++;
        }
        for (i = 0; i < g_n_keep_extra; i++)
            keep[n_keep++] = g_keep_extra[i];
        boot_stage(r, "building the memory map the ISO is handed");
        g_n_carved = 0;
        if (first != (const e820_entry *)0 &&
            e820_carve(first, e820_count(), keep, n_keep, g_carved,
                       VCD_E820_MAX, &g_n_carved) != 0) {
            r->advice = "the memory map is too long to hand on with the "
                        "virtual CD's memory taken out.";
            return -1;
        }
        g_e801_cap = e820_e801_cap(keep, n_keep);
        g_e801_low = e820_e801_cap_low(keep, n_keep);
    }

    /* The resident, in memory the BIOS is about to be told it does not
     * have. */
    at = (uint8_t *)(uintptr_t)((uint32_t)seg * 16u);
    memcpy(at, vcd_blob_start, vcd_blob_len());

    /* Its state. The previous vector first, because every call the handler
     * does not answer and every read it does go through it. */
    old_vector = peek32(IVT_INT13);
    memcpy(at + VCD_OFF_OLDINT13, &old_vector, 4);
    at[VCD_OFF_DRIVE] = (uint8_t)VCD_DRIVE;
    at[VCD_OFF_PHYS] = d->drive;
    at[VCD_OFF_NRUNS] = (uint8_t)(g_map.n_runs & 0xFFu);
    at[VCD_OFF_NRUNS + 1u] = (uint8_t)(g_map.n_runs >> 8);
    at[VCD_OFF_PERCD] = (uint8_t)(g_map.per_cd & 0xFFu);
    at[VCD_OFF_PERCD + 1u] = (uint8_t)(g_map.per_cd >> 8);
    memcpy(at + VCD_OFF_NCD, &g_map.n_cd, 4);
    memcpy(at + VCD_OFF_NREAL, &g_map.n_real, 4);
    memcpy(at + VCD_OFF_NLIMIT, &g_map.n_limit, 4);
    for (i = 0; i < g_map.n_runs; i++) {
        uint8_t *run = at + VCD_OFF_RUNS + i * VCD_RUN_LEN;

        memcpy(run, &g_map.run[i].cd_lba, 4);
        memcpy(run + 4, &g_map.run[i].n_cd, 4);
        memcpy(run + 8, &g_map.run[i].disk_lba, 8);
    }
    /* The overlay: its bytes into the pool behind the resident, and the
     * table the handler scans before every pass. */
    if (g_pool_used > 0)
        memcpy(at + VCD_OVER_OFF, g_pool,
               (size_t)g_pool_used * VCD_CD_BYTES);
    at[VCD_OFF_NOVER] = (uint8_t)(g_map.n_over & 0xFFu);
    at[VCD_OFF_NOVER + 1u] = (uint8_t)(g_map.n_over >> 8);
    for (i = 0; i < g_map.n_over; i++) {
        uint8_t *o = at + VCD_OFF_OVER + i * VCD_OV_LEN;

        memcpy(o, &g_map.over[i].cd_lba, 4);
        memcpy(o + 4, &g_map.over[i].n_cd, 4);
        memcpy(o + 8, &g_map.over[i].seg, 2);
        memcpy(o + 10, &g_map.over[i].flags, 2);
        memcpy(o + 12, &g_map.over[i].phys, 4);
    }

    /* The INT 15h half: the vector it chains to, the map, the ceiling. */
    old_vector = peek32(IVT_INT15);
    memcpy(at + VCD_OFF_OLDINT15, &old_vector, 4);
    at[VCD_OFF_NE820] = (uint8_t)(g_n_carved & 0xFFu);
    at[VCD_OFF_NE820 + 1u] = (uint8_t)(g_n_carved >> 8);
    at[VCD_OFF_E801CAP] = (uint8_t)(g_e801_cap & 0xFFu);
    at[VCD_OFF_E801CAP + 1u] = (uint8_t)(g_e801_cap >> 8);
    at[VCD_OFF_E801LOW] = (uint8_t)(g_e801_low & 0xFFu);
    at[VCD_OFF_E801LOW + 1u] = (uint8_t)(g_e801_low >> 8);
    for (i = 0; i < g_n_carved; i++) {
        uint8_t *m = at + VCD_OFF_E820 + i * VCD_E820_LEN;

        memcpy(m, &g_carved[i].base, 8);
        memcpy(m + 8, &g_carved[i].len, 8);
        memcpy(m + 16, &g_carved[i].type, 4);
    }

    write_spec(at + VCD_OFF_SPEC, e, load_seg);

    /* What the launch stub moves, and where to. */
    at[VCD_OFF_LSRC] = (uint8_t)((VCD_STAGE_ADDR / 16u) & 0xFFu);
    at[VCD_OFF_LSRC + 1u] = (uint8_t)((VCD_STAGE_ADDR / 16u) >> 8);
    at[VCD_OFF_LDST] = (uint8_t)(load_seg & 0xFFu);
    at[VCD_OFF_LDST + 1u] = (uint8_t)(load_seg >> 8);
    at[VCD_OFF_LLEN] = (uint8_t)(load_bytes & 0xFFu);
    at[VCD_OFF_LLEN + 1u] = (uint8_t)((load_bytes >> 8) & 0xFFu);
    at[VCD_OFF_LDRIVE] = (uint8_t)VCD_DRIVE;

    bios_puts("falcon-bios: serving the medium on drive ");
    bios_put_hex64(VCD_DRIVE);
    bios_puts(" from ");
    bios_put_hex64((uint64_t)seg * 16u);
    bios_puts("\nfalcon-bios: boot image at CD sector ");
    bios_put_hex64(e->lba);
    bios_puts(", ");
    bios_put_hex64(g_map.n_runs);
    bios_puts(" run(s)\nfalcon-bios: handing over\n");

    /* From here nothing may fail: the memory is taken, the vector is
     * hooked, the machine is about to be somebody else's. */
    lowmem_take(seg);
    poke32(IVT_INT13, ((uint32_t)seg << 16) | (uint32_t)VCD_OFF_CODE);
    poke32(IVT_INT15, ((uint32_t)seg << 16) | (uint32_t)VCD_OFF_CODE15);

    /* Out through the stub, not straight to the boot image: what boot16
     * lands on has to still be there after the image is placed. */
    bios_boot_drive = (uint8_t)VCD_DRIVE;
    poke16(GATE_TARGET, (uint16_t)VCD_OFF_LAUNCH);
    poke16(GATE_TARGET + 2u, seg);
    tui_handoff();
    ((void (*)(void))(uintptr_t)GATE_BOOT16)();
    return -1;
}

int vcd_chainload(bios_disk *d, fs_vol *p2, const part_entry *p2part,
                  const fs_dirent *iso, fs_file *isofile, boot_result *r)
{
    elt_catalog cat;
    const elt_entry *e;
    size_t n_ext = 0;
    size_t i;
    int rc;

    boot_stage(r, "reading the ISO's boot catalog");
    if (elt_parse(&cat, fs_file_read_cb, isofile, iso->size) != ELT_OK) {
        r->advice = "the medium has no El Torito boot catalog.";
        return -1;
    }
    e = vcd_boot_entry(&cat);
    if (e == (const elt_entry *)0) {
        r->advice = "the medium boots only on UEFI.";
        return -1;
    }
    if (e->media == ELT_MEDIA_1200K || e->media == ELT_MEDIA_1440K ||
        e->media == ELT_MEDIA_2880K) {
        /* A floppy-emulation image is served as a memory disk (vfd.c),
         * not chainloaded off the virtual CD. Never returns on success. */
        return vfd_serve(fs_file_read_cb, isofile, e, r);
    }
    if (e->media != ELT_MEDIA_NONE) {
        r->advice = "the medium's boot image asks for hard-disk emulation, "
                    "which this loader does not do.";
        return -1;
    }

    /* Where the ISO is on the disk, as runs the handler can scan. */
    boot_stage(r, "mapping the ISO's extents on p2");
    rc = fs_extents(p2, iso, g_fsext,
                    sizeof g_fsext / sizeof g_fsext[0], &n_ext);
    if (rc != FS_OK) {
        r->advice = TOO_MANY_PIECES;
        return -1;
    }
    for (i = 0; i < n_ext; i++) {
        g_ext[i].off = g_fsext[i].vol_off;
        g_ext[i].len = g_fsext[i].len;
    }
    rc = vcd_build(&g_map, g_ext, n_ext,
                   p2part->start_lba, d->sector_size, iso->size,
                   (uint8_t)VCD_DRIVE, d->drive);
    if (rc != VCD_OK) {
        if (rc == VCD_ERR_TOO_MANY)
            r->advice = TOO_MANY_PIECES;
        else if (rc == VCD_ERR_ALIGN)
            r->advice = "the ISO is not laid out on the stick in whole CD "
                        "sectors.";
        return -1;
    }

    return launch(d, e, fs_file_read_cb, isofile, r);
}

int vcd_chainload_drive(bios_disk *d, uint64_t n_cd, boot_result *r)
{
    elt_catalog cat;
    const elt_entry *e;
    vcd_extent whole;
    int rc;

    boot_stage(r, "reading the disc's boot catalog");
    if (d->sector_size != VCD_CD_BYTES ||
        elt_parse(&cat, bios_disk_read, d, n_cd * VCD_CD_BYTES) != ELT_OK) {
        r->advice = "the disc has no El Torito boot catalog.";
        return -1;
    }
    e = vcd_boot_entry(&cat);
    if (e == (const elt_entry *)0) {
        r->advice = "the disc boots only on UEFI.";
        return -1;
    }
    if (e->media != ELT_MEDIA_NONE) {
        r->advice = "the disc boots through floppy or hard-disk emulation, "
                    "which this build does not serve from a drive.";
        return -1;
    }

    /* The whole disc is one run: CD sector n is the drive's sector n. */
    boot_stage(r, "mapping the disc");
    whole.off = 0;
    whole.len = n_cd * VCD_CD_BYTES;
    rc = vcd_build(&g_map, &whole, 1, 0, VCD_CD_BYTES, whole.len,
                   (uint8_t)VCD_DRIVE, d->drive);
    if (rc != VCD_OK)
        return -1;
    return launch(d, e, bios_disk_read, d, r);
}
