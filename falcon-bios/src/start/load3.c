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

/* Entered from stage2.S in 32-bit protected mode with the drive the BIOS
 * booted from. Does not return: either stage 3 runs or the machine resets.
 *
 * Stage 3 is always stored compressed (biosdefs.h, STAGE3_MAGIC_GZ): a
 * "BOSZ" header, then a gzip member of the linked image, "BOS3" header and
 * all. The member goes to GZ_STAGE_ADDR and is inflated to STAGE3_LOAD, its
 * CRC-32 and length checked, which is the check that the disk gave back what
 * was written; and it is half the bytes to read, which on a floppy or USB
 * 1.1 is most of the time stage 2 takes (DESIGN Section 14.3). */
void stage3_load(uint8_t drive);

void stage3_load(uint8_t drive)
{
    volatile uint32_t *zh = (volatile uint32_t *)(uintptr_t)STAGE3_LOAD;
    uint32_t *hdr = (uint32_t *)(uintptr_t)STAGE3_LOAD;
    uint8_t *packed = (uint8_t *)(uintptr_t)GZ_STAGE_ADDR;
    bios_disk d;
    uint64_t base;
    uint64_t file;
    uint64_t mem;
    uint64_t want;
    uint64_t sectors;
    size_t got = 0;
    unsigned attempt;

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
    if (bios_disk_read(&d, base, d.sector_size, (void *)(uintptr_t)zh) != 0)
        die("stage 3 could not be read");
    if (zh[0] != (uint32_t)STAGE3_MAGIC_GZ)
        die("stage 3 is not where the header says it should be");

    file = zh[1];
    mem = zh[2];
    want = zh[3];
    if (file <= STAGE3_HDR ||
        file > (uint64_t)STAGE3_MAX_SECTORS * d.sector_size)
        die("stage 3 declares a length this stage will not read");
    if (want < STAGE3_HDR || mem < want ||
        mem > GZ_STAGE_ADDR - STAGE3_LOAD)
        die("stage 3 declares a size that will not fit below its staging");

    /* Twice, the second time from the disk rather than the track cache: a
     * floppy that misread once often reads right the next time, and the
     * member's CRC-32 is what tells the two apart. */
    sectors = (file + d.sector_size - 1u) / d.sector_size;
    for (attempt = 0; ; attempt++) {
        if (bios_disk_read(&d, base, sectors * d.sector_size, packed) != 0)
            die("stage 3 is on the disk but would not read whole");
        if (gz_member(packed + STAGE3_HDR, (size_t)(file - STAGE3_HDR),
                      (uint8_t *)(uintptr_t)STAGE3_LOAD, (size_t)want,
                      &got) == GZ_OK && got == want)
            break;
        if (attempt == 1)
            die("stage 3 did not read back as it was written, twice; the "
                "disk may be failing");
        bios_puts("falcon-bios: stage 3 failed its check; reading it again\n");
        bios_disk_forget();
    }
    if (hdr[0] != (uint32_t)STAGE3_MAGIC || hdr[1] != want || hdr[2] != mem)
        die("stage 3 inflated to something that is not stage 3");

    /* Everything past the image is .bss, which is not in the image and which
     * nothing else will clear: stage 3 is the only thing here with a loader.
     * A C program whose statics start as whatever the BIOS left behind fails
     * in ways that look like anything but this. */
    memset((void *)(uintptr_t)(STAGE3_LOAD + want), 0, (size_t)(mem - want));

    bios_puts("falcon-bios: stage 3 inflated, ");
    bios_put_dec(file);
    bios_puts(" bytes to ");
    bios_put_dec(want);
    bios_puts(" at ");
    bios_put_hex64(STAGE3_LOAD);
    bios_puts(", ");
    bios_put_dec(mem - want);
    bios_puts(" zeroed\n");

    ((void (*)(void))(uintptr_t)(STAGE3_LOAD + STAGE3_HDR))();
    die("stage 3 returned");
}
