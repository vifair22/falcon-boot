/* falcon-bios: installing the drive swap (swapres.S, DESIGN Section 12.4). */

#include <mbr/swap.h>

#include <start/bios.h>
#include <start/biosdefs.h>
#include <lib/string.h>
#include <mem/lowmem.h>

#define IVT_INT13 (0x13u * 4u)

extern const unsigned char swap_blob_start[];
extern const unsigned char swap_blob_end[];

void swap_install(uint16_t seg, uint8_t drive)
{
    uint32_t at = (uint32_t)seg * 16u;
    size_t len = (size_t)((uintptr_t)swap_blob_end -
                          (uintptr_t)swap_blob_start);

    memcpy((void *)(uintptr_t)at, swap_blob_start, len);
    poke32(at + SWAP_OFF_OLDINT13, peek32(IVT_INT13));
    poke8(at + SWAP_OFF_DRIVE, drive);
    lowmem_take(seg);
    poke32(IVT_INT13, ((uint32_t)seg << 16) | (uint32_t)SWAP_OFF_CODE);
    bios_puts("falcon-bios: drive ");
    bios_put_hex64(drive);
    bios_puts(" swapped with 0x80, INT 13h resident at segment ");
    bios_put_hex64(seg);
    bios_puts(" (");
    bios_put_dec(len);
    bios_puts(" bytes, 1 KB taken)\n");
}
