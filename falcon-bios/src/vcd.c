/* falcon-bios: installing the virtual CD and handing the machine to the
 * medium's own bootloader (docs/notes/bios-route.md Sections 4.1 and 7.1).
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

#include <bios.h>
#include <vcd.h>
#include <vcdcore.h>

#include <fs/fs.h>
#include <media/eltorito.h>
#include <lib/string.h>

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
#define BDA_MEMSIZE 0x413
#define IVT_INT13   (0x13u * 4u)

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

static vcd_extent g_ext[VCD_MAX_RUNS + 8u];
static fs_extent  g_fsext[VCD_MAX_RUNS + 8u];
static uint8_t    g_sector[2048];

static void put(const char *s) { bios_screen_puts(s); }

void vcd_overlay_reset(void)
{
    g_pool_used = 0;
    g_n_pending = 0;
    g_append_phys = 0;
    g_append_cd = 0;
}

int vcd_overlay_append(uint32_t phys, uint32_t n_cd)
{
    if (phys == 0 || n_cd == 0 || g_append_cd != 0)
        return -1;
    g_append_phys = phys;
    g_append_cd = n_cd;
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

static void put_hex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    char one[2];
    int shift;
    int leading = 1;

    one[1] = '\0';
    put("0x");
    for (shift = 60; shift >= 0; shift -= 4) {
        unsigned nib = (unsigned)((v >> shift) & 0xFu);

        if (nib == 0 && leading && shift != 0)
            continue;
        leading = 0;
        one[0] = digits[nib];
        put(one);
    }
}

/* The medium's BIOS boot image: the default entry, or the first entry that
 * says x86. The platform byte is reported rather than trusted for filtering
 * (media/eltorito.c's header says why), so the default entry comes first and
 * the platform is only a fallback. */
static const elt_entry *bios_entry(const elt_catalog *cat)
{
    size_t i;

    for (i = 0; i < cat->n; i++) {
        if (cat->entry[i].is_default &&
            cat->entry[i].platform != ELT_PLATFORM_UEFI)
            return &cat->entry[i];
    }
    for (i = 0; i < cat->n; i++) {
        if (cat->entry[i].platform == ELT_PLATFORM_X86)
            return &cat->entry[i];
    }
    return (const elt_entry *)0;
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

/* The segment of the block at the top of conventional memory the resident
 * will live in, without taking it yet. 0 when the machine cannot give it up.
 *
 * Asked for, then taken separately (take_conventional_memory) at the point
 * past which nothing can fail. Everything between can refuse, and a refusal
 * that had already lowered the BDA would leave the machine sixteen
 * kilobytes shorter for the rest of its uptime, once per attempt. Writing
 * the block before the BDA admits it is gone is safe: nothing of this
 * loader's lives up there, and nothing else runs until the handoff. */
static uint16_t resident_segment(void)
{
    uint16_t have = peek16(BDA_MEMSIZE);
    uint16_t want = (uint16_t)(VCD_BYTES / 1024u);

    /* A machine reporting less than 256 KB is one whose BDA we do not
     * believe, and taking memory from it would be worse than refusing. */
    if (have < 256u || have <= want)
        return 0;
    return (uint16_t)((uint32_t)(have - want) * 1024u / 16u);
}

static void take_conventional_memory(uint16_t seg)
{
    poke16(BDA_MEMSIZE, (uint16_t)((uint32_t)seg * 16u / 1024u));
}

int vcd_chainload(bios_disk *d, fs_vol *p2, const gpt_part *p2part,
                  const fs_dirent *iso, fs_file *isofile)
{
    elt_catalog cat;
    const elt_entry *e;
    uint8_t *at;
    uint16_t seg;
    uint16_t load_seg;
    uint32_t load_bytes;
    uint32_t old_vector;
    size_t n_ext = 0;
    size_t i;
    int rc;

    if (elt_parse(&cat, fs_file_read_cb, isofile, iso->size) != ELT_OK) {
        put("  the medium has no boot catalog\n");
        return -1;
    }
    e = bios_entry(&cat);
    if (e == (const elt_entry *)0) {
        put("  the medium has no BIOS boot image; it is UEFI-only\n");
        return -1;
    }
    if (e->media != ELT_MEDIA_NONE) {
        put("  the medium's boot image asks for floppy or hard disk"
            " emulation, which this loader does not do\n");
        return -1;
    }

    /* Where the ISO is on the disk, as runs the handler can scan. */
    rc = fs_extents(p2, iso, g_fsext,
                    sizeof g_fsext / sizeof g_fsext[0], &n_ext);
    if (rc != FS_OK) {
        put("  the ISO is in more pieces on p2 than can be served."
            " Copy it again, onto a stick with more free space.\n");
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
            put("  the ISO is in too many pieces on p2. Copy it again,"
                " onto a stick with more free space.\n");
        else if (rc == VCD_ERR_ALIGN)
            put("  the ISO is not laid out on p2 in whole CD sectors\n");
        else
            put("  the ISO cannot be served as a CD\n");
        return -1;
    }

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

        while (done < load_bytes) {
            uint32_t chunk = load_bytes - done;

            if (chunk > (uint32_t)sizeof g_sector)
                chunk = (uint32_t)sizeof g_sector;
            if (fs_file_read(isofile,
                             (uint64_t)e->lba * ELT_SECTOR + done,
                             chunk, g_sector) != FS_OK) {
                put("  the medium's boot image would not read\n");
                return -1;
            }
            memcpy(dst + done, g_sector, (size_t)chunk);
            done += chunk;
        }
    }

    /* Where the resident will live. Not taken until the end. */
    seg = resident_segment();
    if (seg == 0) {
        put("  this machine will not give up the memory the handler needs\n");
        return -1;
    }

    /* The map finished before any of it is copied: the appended region
     * grows the medium, and the resident has to be handed the size it ends
     * up, not the size it started. The appended region goes first because
     * the medium has to say it is there before anything may be placed in
     * it. */
    if (g_append_cd != 0) {
        if (vcd_extend(&g_map, g_append_cd) != VCD_OK ||
            vcd_override_add_ext(&g_map, g_map.n_real - g_append_cd,
                                 g_append_cd, g_append_phys) != VCD_OK) {
            put("  the appended blocks do not fit the medium\n");
            return -1;
        }
    }
    for (i = 0; i < g_n_pending; i++) {
        uint16_t pool_seg = (uint16_t)(seg + (VCD_OVER_OFF / 16u) +
                                       g_pending[i].pool_at *
                                       (VCD_CD_BYTES / 16u));

        if (vcd_override_add(&g_map, g_pending[i].cd_lba,
                             g_pending[i].n_cd, pool_seg) != VCD_OK) {
            put("  an overlay does not fit the medium\n");
            return -1;
        }
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
        uint8_t *r = at + VCD_OFF_RUNS + i * VCD_RUN_LEN;

        memcpy(r, &g_map.run[i].cd_lba, 4);
        memcpy(r + 4, &g_map.run[i].n_cd, 4);
        memcpy(r + 8, &g_map.run[i].disk_lba, 8);
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

    write_spec(at + VCD_OFF_SPEC, e, load_seg);

    /* What the launch stub moves, and where to. */
    at[VCD_OFF_LSRC] = (uint8_t)((VCD_STAGE_ADDR / 16u) & 0xFFu);
    at[VCD_OFF_LSRC + 1u] = (uint8_t)((VCD_STAGE_ADDR / 16u) >> 8);
    at[VCD_OFF_LDST] = (uint8_t)(load_seg & 0xFFu);
    at[VCD_OFF_LDST + 1u] = (uint8_t)(load_seg >> 8);
    at[VCD_OFF_LLEN] = (uint8_t)(load_bytes & 0xFFu);
    at[VCD_OFF_LLEN + 1u] = (uint8_t)((load_bytes >> 8) & 0xFFu);
    at[VCD_OFF_LDRIVE] = (uint8_t)VCD_DRIVE;

    put("  serving the medium on drive ");
    put_hex(VCD_DRIVE);
    put(" from ");
    put_hex((uint64_t)seg * 16u);
    put("\n  boot image at CD sector ");
    put_hex(e->lba);
    put(", ");
    put_hex(g_map.n_runs);
    put(" run(s)\n  handing over\n");

    /* From here nothing may fail: the memory is taken, the vector is
     * hooked, the machine is about to be somebody else's. */
    take_conventional_memory(seg);
    poke32(IVT_INT13, ((uint32_t)seg << 16) | (uint32_t)VCD_OFF_CODE);

    /* Out through the stub, not straight to the boot image: what boot16
     * lands on has to still be there after the image is placed. */
    bios_boot_drive = (uint8_t)VCD_DRIVE;
    poke16(GATE_TARGET, (uint16_t)VCD_OFF_LAUNCH);
    poke16(GATE_TARGET + 2u, seg);
    ((void (*)(void))(uintptr_t)GATE_BOOT16)();
    return -1;
}
