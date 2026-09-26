/* falcon-bios: stage 2's last job, reading stage 3 in
 * (docs/notes/bios-route.md Section 6).
 *
 * This is C rather than more assembly for the reason Section 5.3 gives: it
 * decides things - whether the header is a header, how many sectors that
 * length is, what to say when the read fails - and everything decidable is C.
 * The transport underneath it is bios_disk_read, the same callback
 * falcon-core reads a partition table through.
 *
 * It runs in protected mode, so writing to a megabyte is an ordinary store;
 * the BIOS read underneath still goes through the bounce buffer in low
 * memory, because the far end of it is a real-mode interrupt handler.
 */

#include <start/bios.h>

#include <lib/string.h>
#include <gz/gz.h>

static __attribute__((noreturn)) void die(const char *why)
{
    bios_fatal(why);
}

/* A compressed stage 3 (biosdefs.h, STAGE3_MAGIC_GZ): the file to
 * GZ_STAGE_ADDR, inflated to STAGE3_LOAD with its CRC-32 and length checked,
 * and then the same checks and start as an ordinary one. On a floppy the
 * bytes read are the time taken, which is the whole reason: falcon jump's
 * stage 3 is half the size this way (DESIGN Section 14.3). Does not return. */
static __attribute__((noreturn)) void load_gz(bios_disk *d, uint64_t base)
{
    volatile uint32_t *zh = (volatile uint32_t *)(uintptr_t)STAGE3_LOAD;
    uint32_t *hdr = (uint32_t *)(uintptr_t)STAGE3_LOAD;
    uint8_t *packed = (uint8_t *)(uintptr_t)GZ_STAGE_ADDR;
    uint64_t file = zh[1];
    uint64_t mem = zh[2];
    uint64_t want = zh[3];
    uint64_t sectors;
    size_t got = 0;

    if (file <= STAGE3_HDR ||
        file > (uint64_t)STAGE3_MAX_SECTORS * d->sector_size)
        die("stage 3 declares a length this stage will not read");
    if (want < STAGE3_HDR || mem < want ||
        mem > GZ_STAGE_ADDR - STAGE3_LOAD)
        die("stage 3 declares a size that will not fit below its staging");

    sectors = (file + d->sector_size - 1u) / d->sector_size;
    if (bios_disk_read(d, base, sectors * d->sector_size, packed) != 0)
        die("stage 3 is on the disk but would not read whole");
    if (gz_member(packed + STAGE3_HDR, (size_t)(file - STAGE3_HDR),
                  (uint8_t *)(uintptr_t)STAGE3_LOAD, (size_t)want,
                  &got) != GZ_OK || got != want)
        die("stage 3 did not inflate whole; the disk may have read wrong");
    if (hdr[0] != (uint32_t)STAGE3_MAGIC || hdr[1] != want || hdr[2] != mem)
        die("stage 3 inflated to something that is not stage 3");

    memset((void *)(uintptr_t)(STAGE3_LOAD + want), 0, (size_t)(mem - want));

    bios_puts("falcon-bios: stage 3 inflated, ");
    bios_put_dec(file);
    bios_puts(" bytes to ");
    bios_put_dec(want);
    bios_puts(" at ");
    bios_put_hex64(STAGE3_LOAD);
    bios_puts("\n");

    ((void (*)(void))(uintptr_t)(STAGE3_LOAD + STAGE3_HDR))();
    die("stage 3 returned");
}

/* Entered from stage2.S in 32-bit protected mode with the drive the BIOS booted from.
 * Does not return: either stage 3 runs or the machine resets. */
void stage3_load(uint8_t drive);

void stage3_load(uint8_t drive)
{
    volatile uint32_t *hdr = (volatile uint32_t *)(uintptr_t)STAGE3_LOAD;
    bios_disk d;
    uint64_t base;
    uint64_t len;
    uint64_t mem;
    uint64_t sectors;

    /* The gate's copy, which is where stage 3 reads it from: stage 3 is
     * linked separately and shares no symbol with the assembly. */
    bios_boot_drive = drive;

    /* A drive that does not answer is left at 512-byte sectors and AH=42h,
     * which is what a stick is, and what stage 3 assumes too; the read below
     * is what decides whether that was right. A floppy answers AH=08h and is
     * read by CHS (DESIGN Section 14.3). */
    if (!bios_disk_probe(&d, drive))
        bios_puts("falcon-bios: the boot drive answered neither AH=48h nor AH=08h\n");
    else if (d.geom.sectors != 0)
        bios_puts("falcon-bios: the boot drive has no INT 13h extensions, reading by CHS\n");

    base = (uint64_t)STAGE3_LBA * d.sector_size;
    if (bios_disk_read(&d, base, d.sector_size, (void *)(uintptr_t)hdr) != 0)
        die("stage 3 could not be read");

    if (hdr[0] == (uint32_t)STAGE3_MAGIC_GZ) {
        load_gz(&d, base);
        return;
    }
    if (hdr[0] != (uint32_t)STAGE3_MAGIC)
        die("stage 3 is not where the header says it should be");

    len = hdr[1];
    mem = hdr[2];
    if (len < STAGE3_HDR ||
        len > (uint64_t)STAGE3_MAX_SECTORS * d.sector_size)
        die("stage 3 declares a length this stage will not read");
    if (mem < len || mem > STAGE3_MAX_MEM)
        die("stage 3 declares a size that will not fit below its stack");

    /* Round up: the BIOS reads whole sectors and the last one is partly the
     * image. Reading the first sector twice costs one interrupt and keeps
     * the arithmetic to one place. */
    sectors = (len + d.sector_size - 1u) / d.sector_size;
    if (bios_disk_read(&d, base, sectors * d.sector_size,
                       (void *)(uintptr_t)hdr) != 0)
        die("stage 3 is on the disk but would not read whole");

    /* Everything past the image is .bss, which is not in the image and which
     * nothing else will clear: stage 3 is the only thing here with a loader.
     * A C program whose statics start as whatever the BIOS left behind fails
     * in ways that look like anything but this. */
    memset((void *)(uintptr_t)(STAGE3_LOAD + len), 0, (size_t)(mem - len));

    bios_puts("falcon-bios: stage 3 loaded, ");
    bios_put_dec(len);
    bios_puts(" bytes at ");
    bios_put_hex64(STAGE3_LOAD);
    bios_puts(", ");
    bios_put_dec(mem - len);
    bios_puts(" zeroed\n");

    ((void (*)(void))(uintptr_t)(STAGE3_LOAD + STAGE3_HDR))();
    die("stage 3 returned");
}
