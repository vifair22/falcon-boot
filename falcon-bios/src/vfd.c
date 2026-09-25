/* falcon-bios: an El Torito floppy image served as a memory disk
 * (docs/notes/bios-route.md Sections 4.1 and 7.1).
 *
 * Some media boot a BIOS through a floppy-emulation image rather than a
 * no-emulation CD: the catalog's media type is 1, 2 or 3, and the BIOS is
 * meant to present the image as drive 0x00 and boot its first sector.
 * memtest86+ and Haiku do.
 *
 * The whole image is small - 2.88 MB at most - so this loads it into
 * extended memory once and serves reads from it with the resident of
 * vfdres.S, which asks INT 15h AH=87h to move the bytes. That mirrors the
 * CD route (vcd.c) but is simpler: a floppy is a flat memory disk, not a
 * disk-backed run table, and this route hooks INT 13h only. The geometry and
 * the CHS-to-LBA arithmetic are vfdcore.c's, host-tested.
 */

#include <bios.h>
#include <vfd.h>
#include <vfdcore.h>
#include <heap.h>
#include <screen.h>

#include <fs/fs.h>
#include <media/eltorito.h>
#include <lib/string.h>

/* The BIOS data area words this touches: the conventional-memory size the
 * resident hides itself behind, and the equipment word that says a floppy is
 * present. */
#define BDA_MEMSIZE 0x413
#define BDA_EQUIP   0x410
#define IVT_INT13   (0x13u * 4u)

/* The resident may not start below the bounce buffer's end (biosdefs.h). */
#define RESIDENT_FLOOR 0x98000u

/* The blob, as vfdres.S lays it out; copied, never called where it is linked
 * (stage 3 is above a megabyte, out of real mode's reach). */
extern const unsigned char vfd_blob_start[];
extern const unsigned char vfd_blob_end[];

static size_t vfd_blob_len(void)
{
    return (size_t)((uintptr_t)vfd_blob_end - (uintptr_t)vfd_blob_start);
}

/* The top-of-conventional-memory block the resident lives in, without taking
 * it yet. 0 when the machine cannot give it up. The same rule as vcd.c's:
 * asked for here, taken only past the point where nothing can fail. */
static uint16_t resident_segment(void)
{
    uint16_t have = peek16(BDA_MEMSIZE);
    uint16_t want = (uint16_t)(VCD_BYTES / 1024u);

    if (have < 256u || have <= want)
        return 0;
    if ((uint32_t)(have - want) * 1024u < RESIDENT_FLOOR)
        return 0;
    return (uint16_t)((uint32_t)(have - want) * 1024u / 16u);
}

static void take_conventional_memory(uint16_t seg)
{
    poke16(BDA_MEMSIZE, (uint16_t)((uint32_t)seg * 16u / 1024u));
}

int vfd_serve(fs_file *isofile, const elt_entry *e, boot_result *r)
{
    vfd_geom g;
    uint64_t image_bytes;
    uint64_t iso_off;
    uint64_t done;
    uint8_t *buf;
    uint8_t *at;
    uint32_t phys;
    uint16_t seg;
    uint16_t equip;

    r->stage = "reading the floppy image's geometry";
    if (isofile == (fs_file *)0 || e == (const elt_entry *)0)
        return -1;
    if (vfd_geometry((uint8_t)e->media, 0, &g) != VFD_OK) {
        r->advice = "the boot image is a floppy type this loader does not "
                    "know.";
        return -1;
    }
    image_bytes = (uint64_t)g.total_sectors * VFD_SECTOR;

    /* The image, in extended memory, padded with zeros as a formatted
     * floppy's unused sectors are. */
    r->stage = "reserving memory for the floppy image";
    heap_reset();
    if (!heap_open(image_bytes + 0x1000u)) {
        r->advice = "this machine has too little memory above 64 MB to hold "
                    "the floppy image.";
        return -1;
    }
    buf = heap_alloc(image_bytes);
    if (buf == (uint8_t *)0) {
        r->advice = "there is not enough memory to hold the floppy image.";
        return -1;
    }
    phys = heap_phys(buf);
    memset(buf, 0, (size_t)image_bytes);

    r->stage = "reading the floppy image";
    iso_off = (uint64_t)e->lba * ELT_SECTOR;
    done = 0;
    while (done < image_bytes) {
        uint64_t chunk = image_bytes - done;

        if (chunk > 0x8000u)
            chunk = 0x8000u;
        /* A read that runs off the ISO's end is where the padding begins;
         * the rest stays zero. */
        if (fs_file_read(isofile, iso_off + done, chunk, buf + done) != FS_OK)
            break;
        done += chunk;
    }
    if (done == 0) {
        r->advice = "the floppy image could not be read off the medium.";
        return -1;
    }

    /* Sector 0 into conventional memory for the launch stub to place at
     * 0x7C00. */
    memcpy((void *)(uintptr_t)VFD_STAGE_ADDR, buf, VFD_SECTOR);

    r->stage = "reserving memory for the virtual floppy";
    seg = resident_segment();
    if (seg == 0) {
        r->advice = "this machine will not give up the conventional memory "
                    "the virtual floppy needs.";
        return -1;
    }

    /* The resident, in memory the BIOS is about to be told it does not
     * have. */
    at = (uint8_t *)(uintptr_t)((uint32_t)seg * 16u);
    memcpy(at, vfd_blob_start, vfd_blob_len());

    {
        uint32_t old = peek32(IVT_INT13);

        memcpy(at + VFD_OFF_OLDINT13, &old, 4);
    }
    at[VFD_OFF_DRIVE] = 0x00;
    at[VFD_OFF_HEADS] = g.heads;
    at[VFD_OFF_SECTORS] = g.sectors;
    at[VFD_OFF_BTYPE] = g.bios_type;
    memcpy(at + VFD_OFF_CYLS, &g.cylinders, 2);
    memcpy(at + VFD_OFF_TOTAL, &g.total_sectors, 4);
    memcpy(at + VFD_OFF_PHYS, &phys, 4);
    at[VFD_OFF_LDRIVE] = 0x00;

    /* A floppy disk base table for AH=08h to hand back, its bytes-per-sector
     * (0x02 = 512) and sectors-per-track filled from the geometry. Most boot
     * code ignores it, but a caller that reads ES:DI must find something
     * sane. */
    {
        static const uint8_t dbt[11] = {
            0xDF, 0x02, 0x25, 0x02, 0x00, 0x1B, 0xFF, 0x54, 0xF6, 0x0F, 0x08
        };

        memcpy(at + VFD_OFF_DBT, dbt, sizeof dbt);
        at[VFD_OFF_DBT + 4] = g.sectors;
    }

    bios_puts("falcon-bios: serving a ");
    bios_put_dec(image_bytes / 1024u);
    bios_puts(" KiB floppy image on drive 0x00 from ");
    bios_put_hex64((uint64_t)seg * 16u);
    bios_puts("\nfalcon-bios: image at ");
    bios_put_hex64(phys);
    bios_puts(", handing over\n");
    screen_out("booting the medium's floppy image\n\n");

    /* The equipment word: a floppy is present (bit 0), and there is one of
     * them (bits 6-7 clear). */
    equip = peek16(BDA_EQUIP);
    equip = (uint16_t)((equip & ~0x00C1u) | 0x0001u);
    poke16(BDA_EQUIP, equip);

    /* From here nothing may fail. */
    take_conventional_memory(seg);
    poke32(IVT_INT13, ((uint32_t)seg << 16) | (uint32_t)VFD_OFF_CODE);

    bios_boot_drive = 0x00;
    poke16(GATE_TARGET, (uint16_t)VFD_OFF_LAUNCH);
    poke16(GATE_TARGET + 2u, seg);
    ((void (*)(void))(uintptr_t)GATE_BOOT16)();
    return -1;
}
