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

static __attribute__((noreturn)) void die(const char *why)
{
    bios_fatal(why);
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

    /* A drive that does not answer is left at 512-byte sectors, which is
     * what a stick is, and what stage 3 assumes too; the read below is
     * what decides whether that was right. */
    if (!bios_disk_probe(&d, drive))
        bios_puts("falcon-bios: the boot drive did not answer AH=48h\n");

    base = (uint64_t)STAGE3_LBA * d.sector_size;
    if (bios_disk_read(&d, base, d.sector_size, (void *)(uintptr_t)hdr) != 0)
        die("stage 3 could not be read");

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
