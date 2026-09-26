/* falcon jump: installing the reverse thunk. See jres.h and rthunk.S. */

#include <start/bios.h>
#include <jump/jres.h>
#include <mem/lowmem.h>

#include <lib/string.h>

#define IVT_INT13 (0x13u * 4u)

/* The blob, copied, never called where it is linked (stage 3 is above a
 * megabyte, out of real mode's reach). Its length through integers, as
 * vcd.c takes vcd_blob's. */
extern const unsigned char jres_blob_start[];
extern const unsigned char jres_blob_end[];

static uint16_t g_seg;
static uint32_t g_old13;
static uint16_t g_old_kb;

static void put32(uint8_t *at, uint32_t v)
{
    memcpy(at, &v, 4);
}

int jres_install(uint8_t drive, uint32_t entry, uint32_t stack_top,
                 uint32_t *base)
{
    size_t len = (size_t)((uintptr_t)jres_blob_end -
                          (uintptr_t)jres_blob_start);
    uint16_t seg = lowmem_segment(JRES_BYTES);
    uint32_t b;
    uint32_t v;
    uint8_t *at;
    uint8_t *desc;

    if (seg == 0 || len > JRES_BYTES)
        return -1;
    b = (uint32_t)seg * 16u;
    at = (uint8_t *)(uintptr_t)b;
    memcpy(at, jres_blob_start, len);

    g_old13 = peek32(IVT_INT13);
    put32(at + JRES_OFF_OLDINT13, g_old13);
    at[JRES_OFF_DRIVE] = drive;
    put32(at + JRES_OFF_GDTR + 2u, b + JRES_OFF_GDT);
    put32(at + JRES_OFF_ENTRY, entry);
    put32(at + JRES_OFF_STACK, stack_top);
    /* The far pointers: the assembly wrote the offsets, relative to the
     * block; the 32-bit one needs the block's linear address added, the
     * real-mode one its segment. */
    memcpy(&v, at + JRES_OFF_PM32PTR, 4);
    put32(at + JRES_OFF_PM32PTR, v + b);
    at[JRES_OFF_RMPTR + 2u] = (uint8_t)(seg & 0xFFu);
    at[JRES_OFF_RMPTR + 3u] = (uint8_t)(seg >> 8);
    /* The 16-bit code descriptor's base is the block, so the 16-bit code
     * that turns protection off runs at the offsets it was assembled for. */
    desc = at + JRES_OFF_GDT + 0x18u;
    desc[2] = (uint8_t)(b & 0xFFu);
    desc[3] = (uint8_t)((b >> 8) & 0xFFu);
    desc[4] = (uint8_t)((b >> 16) & 0xFFu);
    desc[7] = (uint8_t)(b >> 24);

    g_old_kb = peek16(BDA_MEMSIZE);
    lowmem_take(seg);
    poke32(IVT_INT13, ((uint32_t)seg << 16) | (uint32_t)JRES_OFF_CODE);
    g_seg = seg;
    if (base != (uint32_t *)0)
        *base = b;
    bios_puts("falcon-bios: reverse thunk for drive ");
    bios_put_hex64(drive);
    bios_puts(" at ");
    bios_put_hex64(b);
    bios_puts("\n");
    return 0;
}

void jres_remove(void)
{
    if (g_seg == 0)
        return;
    if (peek32(IVT_INT13) == (((uint32_t)g_seg << 16) | JRES_OFF_CODE))
        poke32(IVT_INT13, g_old13);
    if (peek16(BDA_MEMSIZE) == (uint16_t)((uint32_t)g_seg * 16u / 1024u))
        poke16(BDA_MEMSIZE, g_old_kb);
    g_seg = 0;
}
